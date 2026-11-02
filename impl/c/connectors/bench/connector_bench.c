/* tedge-dot — synthetic `bench` connector module for the performance harness
 * (bench/, doc/performance.md). Mirrors impl/rust/crates/connector-bench: it
 * talks to no device, and readings come from per-point value streams
 * (bench_stream.h), so the harness drives the real runtime at any rate.
 * Built only with -DTDOT_BENCH=ON, which no release sets.
 *
 * - Polled: one good sample per point, after a blocking `latency` per block of
 *   `batch` points, the way a blocking protocol call holds up the serial loop.
 * - Pushed: on a device with `push_rate` above 0, every readable point not set
 *   `subscribe = false` is pushed. The C runtime drains push once per loop
 *   tick, so drain_subscriptions() delivers what is owed since the previous
 *   drain (push_rate x elapsed), carrying the fraction over. With a
 *   `publish_interval` it delivers only once that much has passed, the way an
 *   OPC UA subscription delivers one notification per publishing interval.
 * - Source times: a pushed sample's source time is its generation time, spread
 *   evenly across the gap since the last delivery, moved back by up to
 *   `ts_jitter` and down to a multiple of `ts_resolution`, as the Rust module's.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cjson/cJSON.h"

#include "bench_stream.h"
#include "tedge_dot/connector.h"

static const char CAPABILITIES[] =
    "{\"protocol\":\"bench\",\"version\":\"" TDOT_VERSION "\","
    "\"modes\":[\"typed\"],"
    "\"datatypes\":[\"float64\",\"int32\",\"bool\",\"string\"],"
    "\"point_kinds\":[\"synthetic\"],"
    "\"command_verbs\":[],"
    "\"features\":[\"subscribe\"],\"subscribe\":true}";

static const char *const DEVICE_KEYS[] = {"latency",   "batch",         "push_rate",
                                          "ts_jitter", "ts_resolution", "publish_interval",
                                          NULL};
static const char *const POINT_KEYS[] = {"model", "step", "change", NULL};

/* Per point (pt->proto, flat, freed by the config). */
typedef struct {
    bench_stream_t stream;
} bench_point_t;

/* Per device (dev->proto, flat, freed by the config): the spec, the push
 * generator's state and the indexes of the pushed points. */
typedef struct {
    double latency_s;
    size_t batch;
    double push_rate;
    double ts_jitter_s;
    double ts_resolution_ms; /* default 1 */
    double publish_interval_s;
    bench_rng_t pick;
    bench_rng_t jitter;
    double last_drain;
    double carry;
    size_t npushed;
    size_t pushed[];
} bench_device_t;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool in_list(const char *key, const char *const *list) {
    for (; *list; list++)
        if (strcmp(key, *list) == 0)
            return true;
    return false;
}

/* The first unknown key in sort order, so both builds name the same one. */
static const char *unknown_key(const toml_table_t *tab, const char *const *known) {
    const char *first = NULL;
    for (int i = 0; tab; i++) {
        const char *k = toml_key_in(tab, i);
        if (!k)
            break;
        if (!in_list(k, known) && (!first || strcmp(k, first) < 0))
            first = k;
    }
    return first;
}

/* A number written as a TOML integer or float. */
static bool number_in(const toml_table_t *tab, const char *key, double *out) {
    toml_datum_t d = toml_double_in(tab, key);
    if (d.ok) {
        *out = d.u.d;
        return true;
    }
    d = toml_int_in(tab, key);
    if (d.ok) {
        *out = (double)d.u.i;
        return true;
    }
    return false;
}

static int device_spec(tdot_device_t *dev, bench_device_t *bd, char *err, size_t errlen) {
    const toml_table_t *tab = dev->protocol_address;
    char place[256];
    snprintf(place, sizeof place, "device '%s' protocol_address", dev->name);
    const char *unknown = unknown_key(tab, DEVICE_KEYS);
    if (unknown) {
        snprintf(err, errlen, "%s: unknown key '%s'", place, unknown);
        return -1;
    }
    if (tab && toml_key_exists(tab, "latency")) {
        toml_datum_t d = toml_string_in(tab, "latency");
        double s = d.ok ? tdot_duration_parse(d.u.s) : -1.0;
        if (d.ok)
            free(d.u.s);
        if (s < 0) {
            snprintf(err, errlen, "%s: 'latency' must be a duration such as \"2ms\"", place);
            return -1;
        }
        bd->latency_s = s;
    }
    if (tab && toml_key_exists(tab, "batch")) {
        toml_datum_t d = toml_int_in(tab, "batch");
        if (!d.ok || d.u.i < 1) {
            snprintf(err, errlen, "%s: 'batch' must be a positive integer", place);
            return -1;
        }
        bd->batch = (size_t)d.u.i;
    }
    if (tab && toml_key_exists(tab, "push_rate")) {
        double r;
        if (!number_in(tab, "push_rate", &r) || !isfinite(r) || r < 0) {
            snprintf(err, errlen, "%s: 'push_rate' must be a number of 0 or more", place);
            return -1;
        }
        bd->push_rate = r;
    }
    double *durations[] = {&bd->ts_jitter_s, NULL, &bd->publish_interval_s};
    static const char *const SOURCE_TIME_KEYS[] = {"ts_jitter", "ts_resolution",
                                                   "publish_interval"};
    bd->ts_resolution_ms = 1.0;
    for (size_t i = 0; i < 3; i++) {
        const char *k = SOURCE_TIME_KEYS[i];
        if (!tab || !toml_key_exists(tab, k))
            continue;
        toml_datum_t d = toml_string_in(tab, k);
        double secs = d.ok ? tdot_duration_parse(d.u.s) : -1.0;
        if (d.ok)
            free(d.u.s);
        if (secs < 0) {
            snprintf(err, errlen, "%s: '%s' must be a duration such as \"2ms\"", place, k);
            return -1;
        }
        if (durations[i]) {
            *durations[i] = secs;
        } else if (secs < 0.001) {
            snprintf(err, errlen, "%s: 'ts_resolution' must be at least 1ms", place);
            return -1;
        } else {
            bd->ts_resolution_ms = secs * 1000.0;
        }
    }
    return 0;
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Parses the point's address into its stream, and its JSON echo. */
static int point_spec(tdot_device_t *dev, tdot_point_t *pt, bench_point_t *bp, char *err,
                      size_t errlen) {
    const toml_table_t *tab = pt->address;
    char place[320];
    snprintf(place, sizeof place, "point '%s' on device '%s' address", pt->id, dev->name);
    bench_model_t model = BENCH_WALK;
    double step = 1.0, change = 1.0;
    cJSON *addr = cJSON_CreateObject();
    const char *unknown = unknown_key(tab, POINT_KEYS);
    if (unknown) {
        snprintf(err, errlen, "%s: unknown key '%s'", place, unknown);
        goto fail;
    }
    if (tab && toml_key_exists(tab, "model")) {
        toml_datum_t d = toml_string_in(tab, "model");
        int m = -1;
        if (d.ok) {
            m = strcmp(d.u.s, "walk") == 0       ? BENCH_WALK
                : strcmp(d.u.s, "constant") == 0 ? BENCH_CONSTANT
                : strcmp(d.u.s, "counter") == 0  ? BENCH_COUNTER
                                                 : -1;
            free(d.u.s);
        }
        if (m < 0) {
            snprintf(err, errlen, "%s: 'model' must be \"walk\", \"constant\" or \"counter\"",
                     place);
            goto fail;
        }
        model = (bench_model_t)m;
    }
    if (tab && toml_key_exists(tab, "step")) {
        if (!number_in(tab, "step", &step) || !isfinite(step) || step < 0) {
            snprintf(err, errlen, "%s: 'step' must be a finite number of 0 or more", place);
            goto fail;
        }
    }
    if (tab && toml_key_exists(tab, "change")) {
        if (!number_in(tab, "change", &change) || !(change >= 0 && change <= 1)) {
            snprintf(err, errlen, "%s: 'change' must be a number from 0 to 1", place);
            goto fail;
        }
    }
    /* Sorted keys, like the Rust module's serde_json echo. */
    if (tab && toml_key_exists(tab, "change"))
        cJSON_AddNumberToObject(addr, "change", change);
    if (tab && toml_key_exists(tab, "model"))
        cJSON_AddStringToObject(addr, "model",
                                model == BENCH_WALK       ? "walk"
                                : model == BENCH_CONSTANT ? "constant"
                                                          : "counter");
    if (tab && toml_key_exists(tab, "step"))
        cJSON_AddNumberToObject(addr, "step", step);
    pt->addr_json = cJSON_PrintUnformatted(addr);
    cJSON_Delete(addr);
    bench_stream_init(&bp->stream, dev->name, pt->id, model, step, change);
    return 0;
fail:
    cJSON_Delete(addr);
    return -1;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err, size_t errlen) {
    (void)self;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_device_t *dev = &cfg->devices[i];
        bench_device_t *bd = calloc(1, sizeof *bd + dev->npoints * sizeof bd->pushed[0]);
        if (!bd) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        dev->proto = bd;
        bd->batch = (size_t)-1;
        if (device_spec(dev, bd, err, errlen) != 0)
            return -1;
        for (size_t j = 0; j < dev->npoints; j++) {
            tdot_point_t *pt = &dev->points[j];
            if (pt->mode != TDOT_MODE_TYPED) {
                snprintf(err, errlen,
                         "point '%s' on device '%s': the bench module supports typed points only",
                         pt->id, dev->name);
                return -1;
            }
            if (pt->datatype != TDOT_DT_FLOAT64 && pt->datatype != TDOT_DT_INT32 &&
                pt->datatype != TDOT_DT_BOOL && pt->datatype != TDOT_DT_STRING) {
                snprintf(err, errlen,
                         "point '%s' on device '%s': datatype must be float64, int32, bool or "
                         "string",
                         pt->id, dev->name);
                return -1;
            }
            bench_point_t *bp = calloc(1, sizeof *bp);
            if (!bp) {
                snprintf(err, errlen, "out of memory");
                return -1;
            }
            pt->proto = bp;
            if (point_spec(dev, pt, bp, err, errlen) != 0)
                return -1;
        }
    }
    return 0;
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev, char *err, size_t errlen) {
    (void)self;
    (void)dev;
    (void)err;
    (void)errlen;
    return 0;
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self;
    (void)dev;
}

/* The value's bytes, big endian, so `raw` is always present (contract §5). */
static void fill(tdot_point_t *pt, tdot_sample_t *out) {
    bench_point_t *bp = pt->proto;
    bench_stream_advance(&bp->stream);
    bench_stream_value(&bp->stream, pt->datatype, &out->value);
    out->quality = TDOT_Q_GOOD;
    out->raw_group = 1;
    const tdot_value_t *v = &out->value;
    if (pt->datatype == TDOT_DT_INT32) {
        uint32_t u = (uint32_t)(int32_t)v->num;
        for (int k = 0; k < 4; k++)
            out->raw[k] = (uint8_t)(u >> (24 - 8 * k));
        out->raw_len = 4;
    } else if (v->kind == TDOT_VAL_NUM) {
        uint64_t u;
        memcpy(&u, &v->num, sizeof u);
        for (int k = 0; k < 8; k++)
            out->raw[k] = (uint8_t)(u >> (56 - 8 * k));
        out->raw_len = 8;
    } else if (v->kind == TDOT_VAL_BOOL) {
        out->raw[0] = v->b ? 1 : 0;
        out->raw_len = 1;
    } else {
        size_t n = strlen(v->str);
        memcpy(out->raw, v->str, n);
        out->raw_len = n;
    }
}

/* Blocks for `latency` per block of `batch` points, as a blocking protocol
 * call holds up the serial C loop. */
static void wait_blocks(const bench_device_t *bd, size_t n) {
    if (bd->latency_s <= 0 || n == 0)
        return;
    size_t blocks = (n + bd->batch - 1) / bd->batch;
    if (bd->batch == (size_t)-1)
        blocks = 1;
    double s = bd->latency_s * (double)blocks;
    struct timespec ts = {(time_t)s, (long)((s - floor(s)) * 1e9)};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      tdot_sample_t *out) {
    (void)self;
    wait_blocks(dev->proto, 1);
    fill(pt, out);
    return 0;
}

static size_t read_points(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *const *pts,
                          size_t n, tdot_sample_t *out, int *rc) {
    (void)self;
    wait_blocks(dev->proto, n);
    for (size_t k = 0; k < n; k++) {
        fill(pts[k], &out[k]);
        rc[k] = 0;
    }
    return n;
}

static int subscribe_device(tdot_connector_t *self, tdot_device_t *dev, char *err,
                            size_t errlen) {
    (void)err;
    (void)errlen;
    bench_device_t *bd = dev->proto;
    bd->npushed = 0;
    if (self->no_push || bd->push_rate <= 0)
        return 0;
    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        pt->subscribed = pt->subscribe && (pt->access & TDOT_ACCESS_READ);
        if (pt->subscribed)
            bd->pushed[bd->npushed++] = j;
    }
    bench_rng_init(&bd->pick, dev->name, "#push");
    bench_rng_init(&bd->jitter, dev->name, "#ts");
    bd->last_drain = now_s();
    bd->carry = 0.0;
    return 0;
}

static int drain_subscriptions(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_sample_sink_t sink, void *sink_ctx) {
    (void)self;
    bench_device_t *bd = dev->proto;
    if (bd->npushed == 0)
        return 0;
    double now = now_s();
    double elapsed = now - bd->last_drain;
    if (elapsed < bd->publish_interval_s)
        return 0;
    double owed = bd->push_rate * elapsed + bd->carry;
    double whole = floor(owed);
    bd->carry = owed - whole;
    bd->last_drain = now;
    double wall = wall_ms(), gap_ms = elapsed * 1000.0;
    size_t n = (size_t)whole;
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)(bench_rng_draw(&bd->pick) * (double)bd->npushed);
        if (k >= bd->npushed)
            k = bd->npushed - 1;
        tdot_point_t *pt = &dev->points[bd->pushed[k]];
        tdot_sample_t s;
        tdot_sample_init(&s);
        fill(pt, &s);
        /* The generation time is what a protocol would report as the source
         * time (contract §5). */
        double ts = wall - gap_ms + gap_ms * (double)(i + 1) / (double)n;
        if (bd->ts_jitter_s > 0)
            ts -= bd->ts_jitter_s * 1000.0 * bench_rng_draw(&bd->jitter);
        s.source_ts_ms = floor(ts / bd->ts_resolution_ms) * bd->ts_resolution_ms;
        sink(sink_ctx, dev, pt, &s);
    }
    return 0;
}

static void destroy(tdot_connector_t *self) {
    free(self);
}

tdot_connector_t *tdot_connector_bench_new(void) {
    tdot_connector_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->protocol = "bench";
    c->capabilities_json = CAPABILITIES;
    c->configure = configure;
    c->connect_device = connect_device;
    c->read_point = read_point;
    c->read_points = read_points;
    c->subscribe_device = subscribe_device;
    c->drain_subscriptions = drain_subscriptions;
    c->disconnect_device = disconnect_device;
    c->destroy = destroy;
    return c;
}
