/**
 * \file
 * \brief Diagnostic tracing of exceptional chip/bus/lock events. Always
 * compiled in, but silent unless the process environment has ATECC_DIAG
 * set to a non-empty value other than "0" - so a single production build
 * can be switched per-process without recompilation. One event per line
 * on stderr, machine-parseable:
 *   <prefix>: DIAG event=<name> [key=value ...]
 */
#ifndef ATCA_DIAG_H
#define ATCA_DIAG_H

#include <stdio.h>
#include <stdlib.h>

/* Cached per translation unit: getenv runs once before the first event. */
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

#define ATCA_DIAG(f, ...)    ATCA_DIAG_LINE("atca", f, ##__VA_ARGS__)

#endif /* ATCA_DIAG_H */
