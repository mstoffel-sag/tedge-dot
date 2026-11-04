/* AVEVA PI connector -- push delivery on a worker thread per device. See pi_push.h. */
#include "pi_push.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pi_cursor.h"
#include "pi_log.h"
#include "pi_ring.h"
#include "pi_value.h"

/* The most events a `recorded` request may ask for (the server's default
 * MaxReturnedItemsPerCall). */
#define PI_MAX_COUNT 150000
/* How long the worker waits on its channels before it looks at the stop flag again. */
#define PI_WAIT_MS 200

struct pi_push {
    pthread_t thread;
    bool started;
    atomic_bool stop;
    pi_ring_t ring;
    pi_store_t store; /* the persisted cursors, advanced on drain; locked */

    /* What only the worker touches once it runs. */
    pi_client_t client;
    const pi_settings_t *s;
    char device[256];
    bool recorded;
    double interval_s;
    size_t ntargets;
    size_t *point_of;  /* target -> index in dev->points */
    char **web_ids;    /* target -> WebId */
    char **ids;        /* target -> point id */
    int *target_of;    /* index in dev->points -> target, or -1 */
    size_t npoints;
    pi_cursor_t *sent; /* target -> what has been queued (dedup) */
    pi_ws_t **ws;
    size_t nws;
    /* Live channel messages read while the worker is busy elsewhere (a catch-up, a full
     * ring), at most `live_limit` (backfill.live_buffer). */
    cJSON **live;
    size_t live_head, live_n, live_cap, live_limit;
    bool overflow;
    bool closed;
    char closed_reason[TDOT_REASON_MAX];

    /* What the drain reads. */
    _Atomic long long alive_ms; /* monotonic ms of the last message, heartbeat or answer */
    atomic_bool failed;
    char reason[TDOT_REASON_MAX]; /* written before `failed` is set */
    double period_s;              /* the longest normal silence / 3; 0: unbounded */
};

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void alive(pi_push_t *p) { atomic_store(&p->alive_ms, mono_ms()); }

static bool stopping(pi_push_t *p) { return atomic_load(&p->stop); }

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void fail(pi_push_t *p, const char *fmt, ...) {
    if (stopping(p))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->reason, sizeof p->reason, fmt, ap);
    va_end(ap);
    pi_log("warn", "device %s: push delivery stopped: %s", p->device, p->reason);
    atomic_store(&p->failed, true);
}

static void maybe_flush(pi_push_t *p) {
    pthread_mutex_lock(&p->store.mu);
    pi_store_maybe_flush(&p->store);
    pthread_mutex_unlock(&p->store.mu);
}

static void close_channels(pi_push_t *p) {
    for (size_t i = 0; i < p->nws; i++)
        pi_ws_close(p->ws[i]);
    free(p->ws);
    p->ws = NULL;
    p->nws = 0;
}

/* ---- the live buffer ------------------------------------------------------------------------ */

static bool live_push(pi_push_t *p, cJSON *msg) {
    if (p->live_head + p->live_n == p->live_cap) {
        if (p->live_head > 0) {
            memmove(p->live, p->live + p->live_head, p->live_n * sizeof *p->live);
            p->live_head = 0;
        } else {
            size_t cap = p->live_cap ? 2 * p->live_cap : 64;
            cJSON **grown = realloc(p->live, cap * sizeof *grown);
            if (!grown)
                return false;
            p->live = grown;
            p->live_cap = cap;
        }
    }
    p->live[p->live_head + p->live_n++] = msg;
    return true;
}

static cJSON *live_pop(pi_push_t *p) {
    if (!p->live_n)
        return NULL;
    cJSON *msg = p->live[p->live_head++];
    if (--p->live_n == 0)
        p->live_head = 0;
    return msg;
}

/* Read whatever the channels have into the live buffer. Every frame proves the server alive
 * (a heartbeat too); data and error messages are buffered. When the buffer is full the
 * channels are closed and `overflow` raised (spec §5.4, live buffer). */
static void service(pi_push_t *p) {
    for (size_t i = 0; i < p->nws && !p->overflow; i++) {
        for (;;) {
            const char *text = NULL;
            size_t len = 0;
            char why[TDOT_REASON_MAX];
            pi_ws_event_t ev = pi_ws_recv(p->ws[i], &text, &len, why, sizeof why);
            if (ev == PI_WS_NONE)
                break;
            if (ev == PI_WS_CLOSED) {
                if (!p->closed) {
                    p->closed = true;
                    snprintf(p->closed_reason, sizeof p->closed_reason, "%s", why);
                }
                break;
            }
            alive(p);
            if (ev == PI_WS_FRAME)
                continue;
            cJSON *json = pi_json_parse(text, len);
            if (!json)
                continue;
            const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "Items");
            if (cJSON_IsArray(items) && cJSON_GetArraySize(items) == 0 &&
                !cJSON_GetObjectItemCaseSensitive(json, "Errors")) {
                cJSON_Delete(json); /* a heartbeat */
                continue;
            }
            if (p->live_n >= p->live_limit || !live_push(p, json)) {
                cJSON_Delete(json);
                p->overflow = true;
                close_channels(p);
                return;
            }
        }
    }
}

/* The ring's idle callback: read the channels while the runtime catches up. */
static bool idle(void *ctx) {
    pi_push_t *p = ctx;
    if (p->nws)
        service(p);
    return !stopping(p);
}

/* Wait up to PI_WAIT_MS for a channel to have data. */
static void wait_channels(pi_push_t *p) {
    struct pollfd fds[64];
    nfds_t n = 0;
    for (size_t i = 0; i < p->nws && n < 64; i++)
        if (p->ws[i]->fd != CURL_SOCKET_BAD)
            fds[n++] = (struct pollfd){.fd = (int)p->ws[i]->fd, .events = POLLIN};
    if (n)
        (void)poll(fds, n, PI_WAIT_MS);
    else {
        struct timespec ts = {0, PI_WAIT_MS * 1000000L};
        nanosleep(&ts, NULL);
    }
}

/* ---- delivery ------------------------------------------------------------------------------- */

/* Hand one PI event of target `t` to the runtime, unless it is already queued or delivered.
 * 1 queued, 0 skipped, -1 the worker is stopping. */
static int deliver(pi_push_t *p, size_t t, const cJSON *ev, bool replayed) {
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(ev, "Timestamp");
    pi_time_t at;
    if (!cJSON_IsString(ts) || !pi_time_parse(ts->valuestring, &at))
        return 0; /* an event without a valid Timestamp is skipped */
    char *value = pi_canonical(cJSON_GetObjectItemCaseSensitive(ev, "Value"));
    if (!value)
        return 0;
    if (!pi_cursor_admits(&p->sent[t], at, value)) {
        /* Already delivered -- or older than the cursor (a history edit), which is dropped. */
        free(value);
        return 0;
    }
    pi_event_t e = {.point = (uint32_t)p->point_of[t], .replayed = replayed,
                    .event = pi_canonical(ev)};
    if (!e.event || pi_ring_push(&p->ring, &e, idle, p) != 0) {
        free(e.event);
        free(value);
        return stopping(p) ? -1 : 0;
    }
    (void)pi_cursor_advance(&p->sent[t], ts->valuestring, at, value);
    free(value);
    return 1;
}

typedef struct {
    pi_time_t at;
    size_t order;
    const cJSON *ev;
    const char *ts;
} pi_stamped_t;

static int stamped_cmp(const void *a, const void *b) {
    const pi_stamped_t *x = a, *y = b;
    int c = pi_time_cmp(x->at, y->at);
    if (c)
        return c;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* The events of an `Items` array that carry a valid Timestamp, in time order (stable). */
static pi_stamped_t *stamped(const cJSON *items, size_t *n) {
    *n = 0;
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (count <= 0)
        return NULL;
    pi_stamped_t *out = calloc((size_t)count, sizeof *out);
    if (!out)
        return NULL;
    const cJSON *ev;
    size_t i = 0;
    cJSON_ArrayForEach(ev, items) {
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(ev, "Timestamp");
        if (cJSON_IsString(ts) && pi_time_parse(ts->valuestring, &out[*n].at)) {
            out[*n].order = i;
            out[*n].ev = ev;
            out[*n].ts = ts->valuestring;
            (*n)++;
        }
        i++;
    }
    qsort(out, *n, sizeof *out, stamped_cmp);
    return out;
}

/* Deliver the events of one stream item (a channel message's or a recorded answer's), in
 * time order: a channel does not guarantee it. -1 when stopping. */
static int deliver_item(pi_push_t *p, const cJSON *item, bool replayed) {
    const cJSON *web_id = cJSON_GetObjectItemCaseSensitive(item, "WebId");
    if (!cJSON_IsString(web_id))
        return 0;
    char errors[TDOT_ERR_MAX];
    if (pi_errors_of(item, errors, sizeof errors)) {
        pi_log("warn", "device %s: PI reported an error for the stream %s: %s", p->device,
               web_id->valuestring, errors);
        return 0;
    }
    size_t n = 0;
    pi_stamped_t *events = stamped(cJSON_GetObjectItemCaseSensitive(item, "Items"), &n);
    int rc = 0;
    for (size_t t = 0; t < p->ntargets && rc == 0; t++) {
        if (strcmp(p->web_ids[t], web_id->valuestring) != 0)
            continue;
        for (size_t i = 0; i < n; i++)
            if (deliver(p, t, events[i].ev, replayed) < 0) {
                rc = -1;
                break;
            }
    }
    free(events);
    return rc;
}

/* Outcome of a worker step. */
enum { STEP_OK = 0, STEP_ERROR = -1, STEP_STOP = -2 };

static int step_error(pi_push_t *p, const pi_error_t *e, char *err, size_t errlen) {
    if (e->kind == PI_E_STOPPED || stopping(p))
        return STEP_STOP;
    pi_error_format(e, err, errlen);
    return STEP_ERROR;
}

/* Read target `t`'s archive from `from` in pages (spec §5.3, §5.4) and deliver it, at most
 * `max_events` events (0: no limit). */
static int page(pi_push_t *p, size_t t, const char *from_text, size_t max_events,
                bool replayed, char *err, size_t errlen) {
    size_t page_size = p->s->backfill ? p->s->page_size : 1000;
    size_t max_count = page_size;
    char from[64];
    snprintf(from, sizeof from, "%s", from_text);
    size_t delivered = 0;
    for (;;) {
        if (stopping(p))
            return STEP_STOP;
        if (p->nws)
            service(p);
        char count[24];
        snprintf(count, sizeof count, "%zu", max_count);
        pi_buf_t q = {0};
        pi_buf_pair(&q, "webId", p->web_ids[t]);
        pi_buf_pair(&q, "startTime", from);
        pi_buf_pair(&q, "endTime", "*");
        pi_buf_pair(&q, "maxCount", count);
        pi_buf_pair(&q, "boundaryType", "Inside");
        pi_buf_pair(&q, "selectedFields", "Items.WebId;Items.Items;Items.Errors");
        cJSON *doc = NULL;
        pi_error_t e;
        int rc = pi_http_get(&p->client, "streamsets/recorded", q.s ? q.s : "", &doc, &e);
        pi_buf_free(&q);
        if (rc != 0)
            return step_error(p, &e, err, errlen);
        alive(p);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "Items");
        const cJSON *item = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : NULL;
        char errors[TDOT_ERR_MAX];
        if (!item) {
            cJSON_Delete(doc);
            return STEP_OK;
        }
        if (pi_errors_of(item, errors, sizeof errors)) {
            pi_log("warn", "device %s: point %s: cannot read the archive: %s", p->device,
                   p->ids[t], errors);
            cJSON_Delete(doc);
            return STEP_OK;
        }
        const cJSON *evs = cJSON_GetObjectItemCaseSensitive(item, "Items");
        size_t n = cJSON_IsArray(evs) ? (size_t)cJSON_GetArraySize(evs) : 0;
        size_t ns = 0;
        pi_stamped_t *events = stamped(evs, &ns);
        int out = -3; /* continue */
        for (size_t i = 0; i < ns; i++) {
            int d = deliver(p, t, events[i].ev, replayed);
            if (d < 0) {
                out = STEP_STOP;
                break;
            }
            if (d > 0 && max_events && ++delivered >= max_events) {
                pi_log("warn",
                       "device %s: point %s: backfill.max_events (%zu) reached: events after %s "
                       "are not replayed",
                       p->device, p->ids[t], delivered, events[i].ts);
                out = STEP_OK;
                break;
            }
        }
        if (out == -3 && (n < max_count || ns == 0))
            out = STEP_OK;
        if (out == -3) {
            if (pi_time_cmp(events[0].at, events[ns - 1].at) == 0) {
                /* A whole page at one timestamp: ask for more at once, or paging would
                 * stall. */
                if (max_count >= PI_MAX_COUNT) {
                    pi_log("warn", "device %s: point %s: more than %d events at %s: the rest are "
                                   "skipped",
                           p->device, p->ids[t], PI_MAX_COUNT, events[ns - 1].ts);
                    out = STEP_OK;
                } else {
                    max_count = 2 * max_count > PI_MAX_COUNT ? PI_MAX_COUNT : 2 * max_count;
                }
            } else {
                snprintf(from, sizeof from, "%s", events[ns - 1].ts);
                max_count = page_size;
            }
        }
        free(events);
        cJSON_Delete(doc);
        if (out != -3)
            return out;
    }
}

/* Catch up every point that has a cursor (spec §5.4): from the cursor, or from
 * `now - backfill.max_age` for a longer gap. */
static int catch_up(pi_push_t *p, char *err, size_t errlen) {
    if (!p->s->backfill)
        return STEP_OK;
    pi_time_t floor = pi_time_minus(pi_time_now(), p->s->max_age_s);
    for (size_t t = 0; t < p->ntargets; t++) {
        const pi_cursor_t *c = &p->sent[t];
        if (!c->set)
            continue;
        char from[64];
        if (pi_time_cmp(c->at, floor) < 0) {
            pi_time_format(floor, from, sizeof from);
            pi_log("warn",
                   "device %s: point %s: events from %s to %s are older than backfill.max_age "
                   "and are not replayed",
                   p->device, p->ids[t], c->ts, from);
        } else {
            snprintf(from, sizeof from, "%s", c->ts);
        }
        pi_log("info", "device %s: point %s: catching up from the archive from %s", p->device,
               p->ids[t], from);
        int rc = page(p, t, from, p->s->max_events, true, err, errlen);
        if (rc != STEP_OK)
            return rc;
    }
    return STEP_OK;
}

/* The unique WebIds of the targets, in order. Returns the count; ids has room for ntargets. */
static size_t unique_web_ids(const pi_push_t *p, const char **ids) {
    size_t n = 0;
    for (size_t t = 0; t < p->ntargets; t++) {
        bool seen = false;
        for (size_t k = 0; k < n && !seen; k++)
            seen = strcmp(ids[k], p->web_ids[t]) == 0;
        if (!seen)
            ids[n++] = p->web_ids[t];
    }
    return n;
}

/* Open the device's channels, one per chunk of WebIds. */
static int open_channels(pi_push_t *p, bool include_initial, pi_error_t *e, bool *refused) {
    *refused = false;
    const char **ids = calloc(p->ntargets ? p->ntargets : 1, sizeof *ids);
    size_t *starts = calloc(p->ntargets ? p->ntargets : 1, sizeof *starts);
    if (!ids || !starts) {
        free(ids);
        free(starts);
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "out of memory");
        return -1;
    }
    size_t n = unique_web_ids(p, ids);
    size_t nchunks = pi_chunks(ids, n, atomic_load(p->client.chunk), p->s->max_url_bytes, 200,
                               starts);
    char heartbeat[16];
    snprintf(heartbeat, sizeof heartbeat, "%u", p->s->heartbeat_rate);
    int rc = 0;
    p->ws = calloc(nchunks ? nchunks : 1, sizeof *p->ws);
    for (size_t c = 0; p->ws && c < nchunks; c++) {
        size_t end = c + 1 < nchunks ? starts[c + 1] : n;
        pi_buf_t q = {0};
        for (size_t i = starts[c]; i < end; i++)
            pi_buf_pair(&q, "webId", ids[i]);
        pi_buf_pair(&q, "includeInitialValues", include_initial ? "true" : "false");
        pi_buf_pair(&q, "heartbeatRate", heartbeat);
        pi_ws_t *ws = NULL;
        rc = pi_ws_open(&p->client, q.s ? q.s : "", &ws, e);
        pi_buf_free(&q);
        if (rc != 0) {
            *refused = e->kind == PI_E_STATUS;
            break;
        }
        p->ws[p->nws++] = ws;
    }
    if (!p->ws) {
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "out of memory");
        rc = -1;
    }
    if (rc != 0)
        close_channels(p);
    free(ids);
    free(starts);
    return rc;
}

/* A channel message: data, or a data source's failure behind PI Web API. */
static int handle_message(pi_push_t *p, const cJSON *json) {
    char errors[TDOT_ERR_MAX];
    if (pi_errors_of(json, errors, sizeof errors)) {
        pi_log("warn", "device %s: PI Web API reported on the channel: %s", p->device, errors);
        return 0;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "Items");
    const cJSON *item;
    cJSON_ArrayForEach(item, pi_array(items)) {
        if (deliver_item(p, item, false) < 0)
            return -1;
    }
    return 0;
}

static void drop_live(pi_push_t *p) {
    cJSON *m;
    while ((m = live_pop(p)))
        cJSON_Delete(m);
}

static void channel_main(pi_push_t *p) {
    char err[TDOT_REASON_MAX];
    for (;;) {
        p->overflow = false;
        int rc = catch_up(p, err, sizeof err);
        if (rc == STEP_ERROR)
            fail(p, "%s", err);
        if (rc != STEP_OK)
            return;
        for (;;) {
            if (stopping(p))
                return;
            if (p->overflow)
                break;
            cJSON *msg = live_pop(p);
            if (msg) {
                rc = handle_message(p, msg);
                cJSON_Delete(msg);
                if (rc < 0)
                    return;
                continue;
            }
            if (p->closed) {
                fail(p, "%s", p->closed_reason);
                return;
            }
            wait_channels(p);
            service(p);
            maybe_flush(p);
        }
        /* More live events arrived than the buffer holds (spec §5.4, live buffer): deliver
         * what is buffered, catch up from the cursor with recorded reads, and open the channel
         * again. */
        pi_log("warn",
               "device %s: the live buffer is full; catching up from the archive before "
               "reopening the channel",
               p->device);
        close_channels(p);
        cJSON *msg;
        while ((msg = live_pop(p))) {
            rc = handle_message(p, msg);
            cJSON_Delete(msg);
            if (rc < 0)
                return;
        }
        rc = catch_up(p, err, sizeof err);
        if (rc == STEP_ERROR)
            fail(p, "%s", err);
        if (rc != STEP_OK)
            return;
        pi_error_t e;
        bool refused;
        p->closed = false;
        if (open_channels(p, false, &e, &refused) != 0) {
            if (stopping(p))
                return;
            if (refused) {
                fail(p, "reopening the channel: HTTP %ld", e.code);
            } else {
                pi_error_format(&e, err, sizeof err);
                fail(p, "reopening the channel: %s", err);
            }
            return;
        }
    }
}

/* One recorded poll of every pushed point (spec §5.3): from the oldest cursor of each chunk,
 * each point keeping only what is after its own cursor. Events read after the fact are
 * replayed samples. */
static int poll_once(pi_push_t *p, char *err, size_t errlen) {
    size_t page_size = p->s->backfill ? p->s->page_size : 1000;
    const char **ids = calloc(p->ntargets ? p->ntargets : 1, sizeof *ids);
    size_t *starts = calloc(p->ntargets ? p->ntargets : 1, sizeof *starts);
    if (!ids || !starts) {
        free(ids);
        free(starts);
        snprintf(err, errlen, "out of memory");
        return STEP_ERROR;
    }
    size_t n = unique_web_ids(p, ids);
    size_t chunk = atomic_load(p->client.chunk);
    size_t nchunks = pi_chunks(ids, n, chunk, p->s->max_url_bytes, 300, starts);
    int rc = STEP_OK;
    for (size_t c = 0; c < nchunks && rc == STEP_OK; c++) {
        size_t end = c + 1 < nchunks ? starts[c + 1] : n;
        /* The oldest cursor of the chunk's points. */
        const pi_cursor_t *oldest = NULL;
        for (size_t t = 0; t < p->ntargets; t++) {
            bool in_chunk = false;
            for (size_t i = starts[c]; i < end && !in_chunk; i++)
                in_chunk = strcmp(ids[i], p->web_ids[t]) == 0;
            if (in_chunk && p->sent[t].set &&
                (!oldest || pi_time_cmp(p->sent[t].at, oldest->at) < 0))
                oldest = &p->sent[t];
        }
        char start[64];
        if (oldest)
            snprintf(start, sizeof start, "%s", oldest->ts);
        else
            pi_time_format(pi_time_minus(pi_time_now(), p->interval_s), start, sizeof start);
        char count[24];
        snprintf(count, sizeof count, "%zu", page_size);
        pi_buf_t q = {0};
        for (size_t i = starts[c]; i < end; i++)
            pi_buf_pair(&q, "webId", ids[i]);
        pi_buf_pair(&q, "startTime", start);
        pi_buf_pair(&q, "endTime", "*");
        pi_buf_pair(&q, "maxCount", count);
        pi_buf_pair(&q, "boundaryType", "Inside");
        pi_buf_pair(&q, "selectedFields", "Items.WebId;Items.Items;Items.Errors");
        cJSON *doc = NULL;
        pi_error_t e;
        int got = pi_http_get(&p->client, "streamsets/recorded", q.s ? q.s : "", &doc, &e);
        pi_buf_free(&q);
        if (got != 0) {
            if (e.kind == PI_E_TOO_LONG && pi_chunk_halve(&p->client, end - starts[c], e.code))
                break; /* the next poll uses the smaller chunks */
            rc = step_error(p, &e, err, errlen);
            break;
        }
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "Items");
        const cJSON *item;
        cJSON_ArrayForEach(item, pi_array(items)) {
            if (deliver_item(p, item, true) < 0) {
                rc = STEP_STOP;
                break;
            }
            const cJSON *evs = cJSON_GetObjectItemCaseSensitive(item, "Items");
            if (!cJSON_IsArray(evs) || (size_t)cJSON_GetArraySize(evs) < page_size)
                continue;
            /* More than one page since the cursor: page on from where this one ended. */
            const cJSON *web_id = cJSON_GetObjectItemCaseSensitive(item, "WebId");
            for (size_t t = 0; t < p->ntargets && rc == STEP_OK; t++) {
                if (!cJSON_IsString(web_id) || strcmp(p->web_ids[t], web_id->valuestring) ||
                    !p->sent[t].set)
                    continue;
                char from[64];
                snprintf(from, sizeof from, "%s", p->sent[t].ts);
                rc = page(p, t, from, 0, true, err, errlen);
            }
            if (rc != STEP_OK)
                break;
        }
        cJSON_Delete(doc);
    }
    free(ids);
    free(starts);
    return rc;
}

/* A point without a cursor starts at its current value (as a channel's initial values do). */
static int deliver_current(pi_push_t *p, char *err, size_t errlen) {
    const char **ids = calloc(p->ntargets ? p->ntargets : 1, sizeof *ids);
    size_t *starts = calloc(p->ntargets ? p->ntargets : 1, sizeof *starts);
    int rc = STEP_OK;
    if (!ids || !starts) {
        snprintf(err, errlen, "out of memory");
        rc = STEP_ERROR;
    }
    size_t n = 0;
    for (size_t t = 0; rc == STEP_OK && t < p->ntargets; t++) {
        if (p->sent[t].set)
            continue;
        bool seen = false;
        for (size_t k = 0; k < n && !seen; k++)
            seen = strcmp(ids[k], p->web_ids[t]) == 0;
        if (!seen)
            ids[n++] = p->web_ids[t];
    }
    size_t nchunks =
        rc == STEP_OK ? pi_chunks(ids, n, atomic_load(p->client.chunk), p->s->max_url_bytes, 200,
                                  starts)
                      : 0;
    for (size_t c = 0; c < nchunks && rc == STEP_OK; c++) {
        size_t end = c + 1 < nchunks ? starts[c + 1] : n;
        pi_buf_t q = {0};
        for (size_t i = starts[c]; i < end; i++)
            pi_buf_pair(&q, "webId", ids[i]);
        pi_buf_pair(&q, "selectedFields", "Items.WebId;Items.Value;Items.Errors");
        cJSON *doc = NULL;
        pi_error_t e;
        int got = pi_http_get(&p->client, "streamsets/value", q.s ? q.s : "", &doc, &e);
        pi_buf_free(&q);
        if (got != 0) {
            rc = step_error(p, &e, err, errlen);
            break;
        }
        alive(p);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "Items");
        const cJSON *item;
        cJSON_ArrayForEach(item, pi_array(items)) {
            const cJSON *web_id = cJSON_GetObjectItemCaseSensitive(item, "WebId");
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "Value");
            if (!cJSON_IsString(web_id) || !value)
                continue;
            for (size_t t = 0; t < p->ntargets && rc == STEP_OK; t++) {
                if (p->sent[t].set || strcmp(p->web_ids[t], web_id->valuestring) != 0)
                    continue;
                if (deliver(p, t, value, false) < 0)
                    rc = STEP_STOP;
            }
        }
        cJSON_Delete(doc);
    }
    free(ids);
    free(starts);
    return rc;
}

static void recorded_main(pi_push_t *p) {
    char err[TDOT_REASON_MAX];
    /* Which points had no cursor when the worker started: they start at their current value. */
    int rc = catch_up(p, err, sizeof err);
    if (rc == STEP_OK)
        rc = deliver_current(p, err, sizeof err);
    long long period = (long long)(p->interval_s * 1000.0);
    if (period < 1)
        period = 1;
    long long next = mono_ms() + period;
    while (rc == STEP_OK) {
        while (!stopping(p) && mono_ms() < next) {
            long long left = next - mono_ms();
            if (left > 100)
                left = 100;
            struct timespec ts = {0, (left > 0 ? left : 0) * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (stopping(p))
            return;
        next += period;
        rc = poll_once(p, err, sizeof err);
        if (rc == STEP_OK) {
            alive(p);
            maybe_flush(p);
        }
    }
    if (rc == STEP_ERROR)
        fail(p, "%s", err);
}

static void *worker(void *arg) {
    pi_push_t *p = arg;
    if (p->recorded)
        recorded_main(p);
    else
        channel_main(p);
    return NULL;
}

/* ---- the runtime thread's side -------------------------------------------------------------- */

static void free_push(pi_push_t *p) {
    if (!p)
        return;
    close_channels(p);
    drop_live(p);
    free(p->live);
    pi_client_free(&p->client);
    for (size_t t = 0; t < p->ntargets; t++) {
        if (p->web_ids)
            free(p->web_ids[t]);
        if (p->ids)
            free(p->ids[t]);
        if (p->sent)
            pi_cursor_clear(&p->sent[t]);
    }
    free(p->web_ids);
    free(p->ids);
    free(p->sent);
    free(p->point_of);
    free(p->target_of);
    if (p->store.path)
        pi_store_free(&p->store);
    if (p->ring.slots)
        pi_ring_destroy(&p->ring);
    free(p);
}

int pi_push_start(const pi_push_args_t *a, pi_push_t **out, pi_error_t *e, bool *refused) {
    *refused = false;
    memset(e, 0, sizeof *e);
    pi_push_t *p = calloc(1, sizeof *p);
    if (!p)
        goto oom;
    const pi_settings_t *s = &a->pd->settings;
    p->s = s;
    snprintf(p->device, sizeof p->device, "%s", a->dev->name);
    p->recorded = a->recorded;
    p->interval_s = a->interval_s > 0 ? a->interval_s : 1;
    p->ntargets = a->n;
    p->npoints = a->dev->npoints;
    p->point_of = calloc(a->n ? a->n : 1, sizeof *p->point_of);
    p->web_ids = calloc(a->n ? a->n : 1, sizeof *p->web_ids);
    p->ids = calloc(a->n ? a->n : 1, sizeof *p->ids);
    p->sent = calloc(a->n ? a->n : 1, sizeof *p->sent);
    p->target_of = calloc(p->npoints ? p->npoints : 1, sizeof *p->target_of);
    if (!p->point_of || !p->web_ids || !p->ids || !p->sent || !p->target_of)
        goto oom;
    for (size_t k = 0; k < p->npoints; k++)
        p->target_of[k] = -1;
    for (size_t t = 0; t < a->n; t++) {
        size_t k = a->points[t];
        p->point_of[t] = k;
        p->target_of[k] = (int)t;
        p->web_ids[t] = strdup(a->live->streams[k].web_id);
        p->ids[t] = strdup(a->dev->points[k].id);
        if (!p->web_ids[t] || !p->ids[t])
            goto oom;
    }
    if (pi_ring_init(&p->ring, a->n) != 0)
        goto oom;
    if (pi_store_load(&p->store, a->state_file, (const char *const *)p->ids,
                      (const char *const *)p->web_ids, a->n,
                      s->backfill ? s->cursor_flush_s : 10.0) != 0)
        goto oom;
    for (size_t t = 0; t < a->n; t++)
        if (pi_cursor_copy(&p->sent[t], &p->store.cur[t]) != 0)
            goto oom;
    p->live_limit = s->backfill ? s->live_buffer : 10000;
    if (p->live_limit < 1)
        p->live_limit = 1;
    char err[TDOT_REASON_MAX];
    if (pi_client_init(&p->client, s, a->pd->base_url, &a->live->creds, &a->live->chunk,
                       &a->live->chunk_warned, p->device, err, sizeof err) != 0) {
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "%s", err);
        free_push(p);
        return -1;
    }
    p->client.stop = &p->stop;
    if (p->recorded)
        p->period_s = p->interval_s;
    else if (s->heartbeat_rate > 0)
        p->period_s = s->heartbeat_rate * s->polling_interval_s;
    alive(p);
    if (!p->recorded && open_channels(p, s->include_initial_values, e, refused) != 0) {
        free_push(p);
        return -1;
    }
    if (pthread_create(&p->thread, NULL, worker, p) != 0) {
        free_push(p);
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "cannot start the push worker");
        return -1;
    }
    p->started = true;
    *out = p;
    return 0;
oom:
    free_push(p);
    e->kind = PI_E_TRANSPORT;
    snprintf(e->text, sizeof e->text, "out of memory");
    return -1;
}

int pi_push_drain(pi_push_t *p, tdot_device_t *dev, const pi_live_t *live,
                  tdot_sample_sink_t sink, void *ctx) {
    pi_event_t *events = NULL;
    size_t n = pi_ring_take(&p->ring, &events);
    for (size_t i = 0; i < n; i++) {
        pi_event_t *ev = &events[i];
        cJSON *json = pi_json_parse(ev->event, strlen(ev->event));
        int t = ev->point < p->npoints ? p->target_of[ev->point] : -1;
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(json, "Timestamp");
        pi_time_t at;
        if (json && t >= 0 && cJSON_IsString(ts) && pi_time_parse(ts->valuestring, &at)) {
            tdot_point_t *pt = &dev->points[ev->point];
            tdot_sample_t s;
            char *addr = NULL;
            pi_build_sample(pt, &live->streams[ev->point], json, pi_time_ms(at), ev->replayed,
                            &s, &addr);
            sink(ctx, dev, pt, &s);
            free(addr);
            char *value = pi_canonical(cJSON_GetObjectItemCaseSensitive(json, "Value"));
            if (value) {
                pthread_mutex_lock(&p->store.mu);
                (void)pi_cursor_advance(&p->store.cur[t], ts->valuestring, at, value);
                p->store.dirty = true;
                pthread_mutex_unlock(&p->store.mu);
                free(value);
            }
        }
        cJSON_Delete(json);
        free(ev->event);
    }
    free(events);
    if (atomic_load(&p->failed))
        return -1; /* the worker logged why */
    if (p->period_s > 0) {
        double silent = (double)(mono_ms() - atomic_load(&p->alive_ms)) / 1000.0;
        if (silent > 3 * p->period_s) {
            pi_log("warn", "device %s: no message or heartbeat for %.1fs", dev->name, silent);
            return -1;
        }
    }
    return 0;
}

void pi_push_stop(pi_push_t *p) {
    if (!p)
        return;
    atomic_store(&p->stop, true);
    pi_ring_close(&p->ring);
    if (p->started)
        pthread_join(p->thread, NULL);
    pthread_mutex_lock(&p->store.mu);
    pi_store_flush(&p->store);
    pthread_mutex_unlock(&p->store.mu);
    free_push(p);
}
