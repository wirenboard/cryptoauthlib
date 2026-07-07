/** \file
 *  \brief OS Abstraction Functions (POSIX/Linux)
 *
 * \copyright Copyright (c) 2017 Microchip Technology Inc. and its subsidiaries (Microchip). All rights reserved.
 *
 * \page License
 *
 * You are permitted to use this software and its derivatives with Microchip
 * products. Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. The name of Microchip may not be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. This software may only be redistributed and used in connection with a
 *    Microchip integrated circuit.
 *
 * THIS SOFTWARE IS PROVIDED BY MICROCHIP "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * EXPRESSLY AND SPECIFICALLY DISCLAIMED. IN NO EVENT SHALL MICROCHIP BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* renameat2() */
#endif

#include "atca_hal.h"
#include "atca_diag.h"

/** \defgroup hal_ Hardware abstraction layer (hal_)
 *
 * \brief
 * These methods define the hardware abstraction layer for communicating with a CryptoAuth device
 *
   @{ */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

/** Shared-memory mutex regions.
 *
 *  Layout: a bare PTHREAD_PROCESS_SHARED robust mutex at offset 0 -
 *  the same layout every binary generation of this library has used,
 *  so all of them meet on the same region.
 *
 *  Safe concurrent creation comes from the publication protocol, not
 *  from the layout: a creator prepares the region under a hidden
 *  temporary name (".<name>.tmp.<pid>"), fully initializes the mutex,
 *  and only then publishes it into <name> with an atomic
 *  renameat2(RENAME_NOREPLACE). A region visible under its final name
 *  is therefore always fully initialized - even for old binaries that
 *  map and lock it without any handshake. Whoever loses the
 *  publication race deletes its temporary and opens the winner's
 *  region. A temporary abandoned by a crashed creator is deleted once
 *  its owning pid is gone, and the final name never turns into a
 *  permanently poisoned object: the next process simply creates the
 *  region anew.
 *
 *  The one remaining creation hazard is an OLD binary winning the
 *  creation race: it sizes and initializes the region in place, so
 *  for a short while the final name shows a not-yet-initialized
 *  mutex. Openers therefore give freshly-sized regions (ones watched
 *  to grow, or with an mtime within the last second - ftruncate()
 *  sets mtime, mmap stores do not touch it on tmpfs) a small grace
 *  period so such a creator can finish pthread_mutex_init(). That
 *  window dies out together with the old binaries. */

#define HAL_OS_SHM_DIR          "/dev/shm"
#define HAL_OS_SHM_WAIT_MS      500 /* max wait for an in-place (old) creator to size the region */
#define HAL_OS_SHM_WAIT_STEP_MS 5
#define HAL_OS_SHM_FRESH_SEC    1   /* a region this young may still be initializing in place... */
#define HAL_OS_SHM_GRACE_MS     50  /* ...give its creator this much time to finish */

/** Delete temporaries left behind by creators that died before
 *  publishing. Nobody ever opens a temporary by name (publication is
 *  a rename), so deleting one whose owner is gone cannot race a user;
 *  pid reuse merely postpones the cleanup until that pid exits too. */
static void hal_os_shm_cleanup_tmp(const char* pName)
{
    char           prefix[NAME_MAX + 1];
    DIR*           dir;
    struct dirent* de;
    int            len;

    len = snprintf(prefix, sizeof(prefix), ".%s.tmp.", pName);
    if (len <= 0 || len >= (int)sizeof(prefix))
    {
        return;
    }
    if (NULL == (dir = opendir(HAL_OS_SHM_DIR)))
    {
        return;
    }
    while (NULL != (de = readdir(dir)))
    {
        char* end = NULL;
        long  pid;

        if (0 != strncmp(de->d_name, prefix, (size_t)len))
        {
            continue;
        }
        pid = strtol(de->d_name + len, &end, 10);
        if (NULL == end || '\0' != *end || 0 >= pid)
        {
            continue;
        }
        if (0 != kill((pid_t)pid, 0) && ESRCH == errno)
        {
            (void)unlinkat(dirfd(dir), de->d_name, 0);
        }
    }
    (void)closedir(dir);
}

/** Map a region that already exists under its final name. Consumes fd. */
static ATCA_STATUS hal_os_shm_open_existing(int fd, void** ppMutex)
{
    struct stat st;
    int         waited_ms;
    int         saw_empty = 0;
    int         fresh = 1;

    for (waited_ms = 0; ; waited_ms += HAL_OS_SHM_WAIT_STEP_MS)
    {
        if (0 != fstat(fd, &st))
        {
            close(fd);
            return ATCA_GEN_FAIL;
        }
        if (st.st_size >= (off_t)sizeof(pthread_mutex_t))
        {
            break;
        }
        /* Only an old binary creates the region in place and leaves it
           unsized for a while; wait for its ftruncate(). Touching an
           unsized mapping would fault with SIGBUS. */
        saw_empty = 1;
        if (waited_ms >= HAL_OS_SHM_WAIT_MS)
        {
            /* The old creator died between shm_open() and ftruncate().
               Nothing safe to lock here. */
            close(fd);
            return ATCA_GEN_FAIL;
        }
        atca_delay_ms(HAL_OS_SHM_WAIT_STEP_MS);
    }

    if (!saw_empty)
    {
        struct timespec now;

        if (0 == clock_gettime(CLOCK_REALTIME, &now))
        {
            /* A negative difference (mtime in the future after a clock
               step) counts as fresh as well. */
            fresh = ((now.tv_sec - st.st_mtim.tv_sec) <= HAL_OS_SHM_FRESH_SEC);
        }
    }
    if (saw_empty || fresh)
    {
        /* Freshly sized: an old binary may have created it in place
           and may still be inside pthread_mutex_init(). A region just
           published by a new binary can look fresh too - for it the
           delay is merely harmless. */
        atca_delay_ms(HAL_OS_SHM_GRACE_MS);
    }

    *ppMutex = mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (MAP_FAILED == *ppMutex)
    {
        *ppMutex = NULL;
        return ATCA_GEN_FAIL;
    }
    return ATCA_SUCCESS;
}

/**
 * \brief Application callback for creating a mutex object
 * \param[IN/OUT] ppMutex location to receive ptr to mutex
 * \param[IN] pName Name of the mutex for systems using named objects
 */
ATCA_STATUS hal_os_create_mutex(void** ppMutex, const char *pName)
{
    char final_path[sizeof(HAL_OS_SHM_DIR) + 1 + NAME_MAX];
    char tmp_path[sizeof(HAL_OS_SHM_DIR) + 1 + NAME_MAX + 32];
    int  attempt;
    int  n;

    if (!ppMutex || !pName)
    {
        return ATCA_BAD_PARAM;
    }
    /* Accept the POSIX "/name" spelling: an shm name is a single path
       component either way. */
    if ('/' == pName[0])
    {
        pName++;
    }
    if ('\0' == pName[0] || NULL != strchr(pName, '/'))
    {
        return ATCA_BAD_PARAM;
    }
    *ppMutex = NULL;

    n = snprintf(final_path, sizeof(final_path), HAL_OS_SHM_DIR "/%s", pName);
    if (n <= 0 || n >= (int)sizeof(final_path))
    {
        return ATCA_BAD_PARAM;
    }
    n = snprintf(tmp_path, sizeof(tmp_path), HAL_OS_SHM_DIR "/.%s.tmp.%ld", pName, (long)getpid());
    if (n <= 0 || n >= (int)sizeof(tmp_path))
    {
        return ATCA_BAD_PARAM;
    }

    /* The loop repeats only on losing a creation/publication race, and
       every iteration makes forward progress through someone's region;
       needing more than a couple of iterations means something is
       genuinely broken. */
    for (attempt = 0; attempt < 3; attempt++)
    {
        pthread_mutexattr_t muattr;
        pthread_mutex_t*    mutex;
        int                 fd;

        fd = open(final_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (0 <= fd)
        {
            return hal_os_shm_open_existing(fd, ppMutex);
        }
        if (ENOENT != errno)
        {
            return ATCA_GEN_FAIL;
        }

        /* No region under the final name: prepare one under the
           temporary name and publish it. */
        hal_os_shm_cleanup_tmp(pName);
        fd = open(tmp_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (0 > fd)
        {
            if (EEXIST != errno)
            {
                return ATCA_GEN_FAIL;
            }
            /* Leftover with our own pid (pid reuse after a crash):
               nobody else writes this name while we live, delete and
               retry. */
            (void)unlink(tmp_path);
            continue;
        }
        /* 0666 despite the umask: unprivileged engine users (nginx
           workers) must be able to map the region read-write. */
        if (0 != fchmod(fd, 0666) ||
            0 != ftruncate(fd, (off_t)sizeof(pthread_mutex_t)))
        {
            close(fd);
            (void)unlink(tmp_path);
            return ATCA_GEN_FAIL;
        }
        mutex = (pthread_mutex_t*)mmap(NULL, sizeof(pthread_mutex_t),
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (MAP_FAILED == (void*)mutex)
        {
            (void)unlink(tmp_path);
            return ATCA_GEN_FAIL;
        }

        (void)pthread_mutexattr_init(&muattr);
        (void)pthread_mutexattr_settype(&muattr, PTHREAD_MUTEX_ERRORCHECK);
        (void)pthread_mutexattr_setprotocol(&muattr, PTHREAD_PRIO_INHERIT);
        (void)pthread_mutexattr_setpshared(&muattr, PTHREAD_PROCESS_SHARED);
        (void)pthread_mutexattr_setrobust(&muattr, PTHREAD_MUTEX_ROBUST);
        if (0 != pthread_mutex_init(mutex, &muattr))
        {
            (void)munmap(mutex, sizeof(pthread_mutex_t));
            (void)unlink(tmp_path);
            return ATCA_GEN_FAIL;
        }

        /* Publish the fully initialized region under the final name. */
        if (0 == renameat2(AT_FDCWD, tmp_path, AT_FDCWD, final_path, RENAME_NOREPLACE))
        {
            *ppMutex = mutex;
            return ATCA_SUCCESS;
        }
        /* Lost the publication race (or an old binary created the
           region in place meanwhile): discard ours, use the winner's. */
        n = errno;
        (void)pthread_mutex_destroy(mutex);
        (void)munmap(mutex, sizeof(pthread_mutex_t));
        (void)unlink(tmp_path);
        if (EEXIST != n)
        {
            return ATCA_GEN_FAIL;
        }
    }
    return ATCA_GEN_FAIL;
}

/*
 * \brief Application callback for destroying a mutex object
 * \param[IN] pMutex pointer to mutex
 */
ATCA_STATUS hal_os_destroy_mutex(void* pMutex)
{
    if (!pMutex)
    {
        return ATCA_BAD_PARAM;
    }

    return munmap(pMutex, sizeof(pthread_mutex_t)) ? ATCA_GEN_FAIL : ATCA_SUCCESS;
}


/*
 * \brief Application callback for locking a mutex
 * \param[IN] pMutex pointer to mutex
 */
ATCA_STATUS hal_os_lock_mutex(void* pMutex)
{
    int rv;

    if (!pMutex)
    {
        return ATCA_BAD_PARAM;
    }

    rv = pthread_mutex_lock(pMutex);

    if (!rv)
    {
        return ATCA_SUCCESS;
    }
    else if (EDEADLK == rv)
    {
        /* This thread already owns the mutex: a lock-state tracking bug in
           the caller. Treating it as success would mask the imbalance,
           and the matching unlock would release a lock the session still
           needs; fail loudly instead. */
        return ATCA_GEN_FAIL;
    }
    else if (EOWNERDEAD == rv)
    {
        /* Lock was obtained but its because another process terminated so the
        state is indeterminate and will probably need to be fixed */
        ATCA_DIAG("event=mutex_eownerdead");
        pthread_mutex_consistent(pMutex);
        return ATCA_FUNC_FAIL;
    }
    else
    {
        return ATCA_GEN_FAIL;
    }
}

/*
 * \brief Application callback for unlocking a mutex
 * \param[IN] pMutex pointer to mutex
 */
ATCA_STATUS hal_os_unlock_mutex(void* pMutex)
{
    if (!pMutex)
    {
        return ATCA_BAD_PARAM;
    }

    return pthread_mutex_unlock(pMutex) ? ATCA_GEN_FAIL : ATCA_SUCCESS;
}

/** @} */
