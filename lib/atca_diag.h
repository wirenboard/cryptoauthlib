/**
 * \file
 * \brief Diagnostic tracing of exceptional chip/bus/lock events. Compiled
 * in by default, but silent unless the process environment has ATECC_DIAG
 * set to a non-empty value other than "0" - so a single production build
 * can be switched per-process without recompilation. One event per line
 * on stderr, machine-parseable:
 *   <prefix>: DIAG event=<name> [key=value ...]
 *
 * Defining ATCA_DIAG_DISABLE at compile time removes the facility entirely:
 * every event site expands to nothing, so neither the getenv() gate nor any
 * of the diagnostic string literals (the "atca" prefix, "ATECC_DIAG", the
 * event format strings) are emitted into the object - a strings/objdump
 * sweep of the resulting binary surfaces none of them.
 */
#ifndef ATCA_DIAG_H
#define ATCA_DIAG_H

/* The facility needs a hosted environment (stderr, getenv); on other
 * targets, and when explicitly disabled via ATCA_DIAG_DISABLE, the macros
 * compile to nothing, so core files may include this header
 * unconditionally. */
#if !defined(ATCA_DIAG_DISABLE) && (defined(__linux__) || defined(__unix__) || defined(__APPLE__))

#include <stdio.h>
#include <stdlib.h>

/* Cached per translation unit: getenv runs once before the first event.
   Concurrent first calls race benignly - every writer stores the same
   value. */
static inline int atca_diag_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
    {
        const char *v = getenv("ATECC_DIAG");
        enabled = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return enabled;
}

#define ATCA_DIAG_LINE(prefix, f, ...)                                  \
    do {                                                                \
        if (atca_diag_enabled())                                        \
        {                                                               \
            fprintf(stderr, prefix ": DIAG " f "\n", ##__VA_ARGS__);    \
        }                                                               \
    } while (0)

#else

#define ATCA_DIAG_LINE(prefix, f, ...)    do { } while (0)

#endif

#define ATCA_DIAG(f, ...)    ATCA_DIAG_LINE("atca", f, ##__VA_ARGS__)

#endif /* ATCA_DIAG_H */
