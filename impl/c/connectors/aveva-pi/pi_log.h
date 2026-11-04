/* AVEVA PI connector -- log lines in the runtime's format (stderr, one line each). Never pass a
 * secret: not the Authorization header, not a secret file's content (spec §10). */
#ifndef TDOT_PI_LOG_H
#define TDOT_PI_LOG_H

#include <stdarg.h>
#include <stdio.h>

#include "tedge_dot/runtime.h"

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static inline void pi_log(const char *level, const char *fmt, ...) {
    char ts[40];
    char line[1024];
    tdot_now_rfc3339(ts, sizeof ts);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* One write per line: the push workers log from their own threads. */
    fprintf(stderr, "%s %-5s %s\n", ts, level, line);
}

#endif
