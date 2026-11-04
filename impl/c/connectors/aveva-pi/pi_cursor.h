/* AVEVA PI connector -- delivery cursors (spec §5.4). Mirrors
 * impl/rust/crates/connector-aveva-pi/src/cursor.rs: per pushed point, the PI timestamp of the
 * last event handed to the runtime and the values delivered at that timestamp, kept across
 * restarts in <state_dir>/<service_name>/<device>.json:
 *
 *   {"version":1,"points":{"<id>":{"web_id":"F1DP...","ts":"2026-11-02T08:00:00.1234567Z",
 *                                  "values":[81.5]}}}
 *
 * Values are compared by their canonical text (pi_value.h).
 */
#ifndef TDOT_PI_CURSOR_H
#define TDOT_PI_CURSOR_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An instant to the nanosecond. */
typedef struct {
    int64_t sec;
    int32_t nsec;
} pi_time_t;

/* RFC 3339 (`2026-11-02T08:00:00.1234567Z`, any offset). False when it is not one. */
bool pi_time_parse(const char *text, pi_time_t *out);
int pi_time_cmp(pi_time_t a, pi_time_t b);
pi_time_t pi_time_now(void);
pi_time_t pi_time_minus(pi_time_t t, double seconds);
/* RFC 3339 in UTC, to the millisecond (truncated), fraction without trailing zeros. */
void pi_time_format(pi_time_t t, char *dst, size_t cap);
double pi_time_ms(pi_time_t t);

/* One point's position. */
typedef struct {
    bool set;        /* false: no cursor (the point starts live) */
    char ts[64];     /* PI's text, kept verbatim for `startTime` */
    pi_time_t at;
    char **values;   /* canonical texts delivered at `at` */
    size_t nvalues;
} pi_cursor_t;

/* Whether an event at `at` with value `value` (canonical text) comes after the cursor: later,
 * or at the same time with a value not yet delivered at that time. Always true without one. */
bool pi_cursor_admits(const pi_cursor_t *c, pi_time_t at, const char *value);
/* An event was handed on: advance (never backwards). Returns -1 when out of memory. */
int pi_cursor_advance(pi_cursor_t *c, const char *ts, pi_time_t at, const char *value);
void pi_cursor_clear(pi_cursor_t *c);
int pi_cursor_copy(pi_cursor_t *dst, const pi_cursor_t *src);

/* A device's cursors and their file. The caller locks `mu` around every call but load. */
typedef struct {
    pthread_mutex_t mu;
    char *path;
    size_t n;
    char **ids;       /* point ids */
    char **web_ids;   /* the WebId each point resolved to */
    pi_cursor_t *cur; /* n cursors */
    bool dirty;
    double last_flush; /* monotonic seconds */
    double every;      /* backfill.cursor_flush */
} pi_store_t;

/* Load the device's file, keeping the cursors of the points given (ids[k] resolved to
 * web_ids[k]) whose WebId is unchanged. A missing file means no cursors; an unreadable one is
 * reported (warn) and replaced on the next flush. 0, or -1 when out of memory. */
int pi_store_load(pi_store_t *s, const char *path, const char *const *ids,
                  const char *const *web_ids, size_t n, double every);
void pi_store_free(pi_store_t *s);
/* Write the file when it changed and `every` has passed since the last write. */
void pi_store_maybe_flush(pi_store_t *s);
/* Write the file now (when it changed), by atomic rename. */
void pi_store_flush(pi_store_t *s);

#ifdef __cplusplus
}
#endif

#endif
