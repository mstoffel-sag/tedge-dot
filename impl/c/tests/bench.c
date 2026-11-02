/* The `bench` module (connectors/bench): the shared value vectors and config
 * errors (doc/contract/test-vectors/bench/, which the Rust module runs too),
 * the blocking latency per block, and the push rate at two tick lengths.
 *
 *   tedge-dot-bench-module <doc/contract/test-vectors/bench>
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench_stream.h"
#include "cjson/cJSON.h"
#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"

static int failures;

#define CHECK(cond, ...)                                                                       \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                               \
            fprintf(stderr, __VA_ARGS__);                                                      \
            fputc('\n', stderr);                                                               \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_s(double s) {
    struct timespec ts = {(time_t)s, (long)((s - floor(s)) * 1e9)};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static tdot_datatype_t datatype(const char *s) {
    if (strcmp(s, "int32") == 0)
        return TDOT_DT_INT32;
    if (strcmp(s, "bool") == 0)
        return TDOT_DT_BOOL;
    if (strcmp(s, "string") == 0)
        return TDOT_DT_STRING;
    return TDOT_DT_FLOAT64;
}

static void value_vectors(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/values.json", dir);
    char *text = slurp(path);
    CHECK(text, "cannot read %s", path);
    if (!text)
        return;
    cJSON *doc = cJSON_Parse(text);
    cJSON *cases = cJSON_GetObjectItem(doc, "cases");
    int ncases = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, cases) {
        ncases++;
        const char *name = cJSON_GetObjectItem(c, "name")->valuestring;
        const char *m = cJSON_GetObjectItem(c, "model")->valuestring;
        bench_model_t model = strcmp(m, "constant") == 0  ? BENCH_CONSTANT
                              : strcmp(m, "counter") == 0 ? BENCH_COUNTER
                                                          : BENCH_WALK;
        tdot_datatype_t dt = datatype(cJSON_GetObjectItem(c, "datatype")->valuestring);
        bench_stream_t s;
        bench_stream_init(&s, cJSON_GetObjectItem(c, "device")->valuestring,
                          cJSON_GetObjectItem(c, "point")->valuestring, model,
                          cJSON_GetObjectItem(c, "step")->valuedouble,
                          cJSON_GetObjectItem(c, "change")->valuedouble);
        int n = 0;
        cJSON *want;
        cJSON_ArrayForEach(want, cJSON_GetObjectItem(c, "values")) {
            bench_stream_advance(&s);
            tdot_value_t v;
            memset(&v, 0, sizeof v);
            bench_stream_value(&s, dt, &v);
            int ok;
            if (dt == TDOT_DT_FLOAT64) {
                uint64_t bits;
                memcpy(&bits, &v.num, sizeof bits);
                char hex[32];
                snprintf(hex, sizeof hex, "0x%016" PRIx64, bits);
                ok = strcmp(hex, want->valuestring) == 0;
            } else if (dt == TDOT_DT_INT32) {
                ok = v.num == want->valuedouble;
            } else if (dt == TDOT_DT_BOOL) {
                ok = v.b == cJSON_IsTrue(want);
            } else {
                ok = strcmp(v.str, want->valuestring) == 0;
            }
            CHECK(ok, "case '%s' reading %d differs", name, n);
            if (!ok)
                break;
            n++;
        }
    }
    CHECK(ncases > 0, "no value cases");
    cJSON_Delete(doc);
    free(text);
}

/* Load `toml` as a config and configure the bench module with it. */
static tdot_config_t *load(const char *toml, tdot_connector_t **conn, char *err, size_t errlen) {
    char path[] = "/tmp/tdot-bench-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w");
    fputs(toml, f);
    fclose(f);
    tdot_config_t *cfg = tdot_config_load(path, err, errlen);
    unlink(path);
    if (!cfg)
        return NULL;
    *conn = tdot_connector_factory("bench");
    if (tdot_connector_configure(*conn, cfg, err, errlen) != 0) {
        (*conn)->destroy(*conn);
        *conn = NULL;
    }
    return cfg;
}

static void config_errors(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/config-errors.json", dir);
    char *text = slurp(path);
    CHECK(text, "cannot read %s", path);
    if (!text)
        return;
    cJSON *doc = cJSON_Parse(text);
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(doc, "cases")) {
        const char *name = cJSON_GetObjectItem(c, "name")->valuestring;
        char toml[2048];
        snprintf(toml, sizeof toml,
                 "[connector]\nprotocol = \"bench\"\n[[device]]\nname = \"plc0\"\n%s\n%s\n",
                 cJSON_GetObjectItem(c, "device")->valuestring,
                 cJSON_GetObjectItem(c, "points")->valuestring);
        char err[512] = "";
        tdot_connector_t *conn = NULL;
        tdot_config_t *cfg = load(toml, &conn, err, sizeof err);
        CHECK(cfg, "case '%s': the loader refused it: %s", name, err);
        cJSON *want = cJSON_GetObjectItem(c, "c");
        if (cJSON_IsNull(want))
            CHECK(conn, "case '%s': refused with '%s'", name, err);
        else
            CHECK(!conn && strcmp(err, want->valuestring) == 0, "case '%s': got '%s', want '%s'",
                  name, conn ? "(accepted)" : err, want->valuestring);
        if (conn)
            conn->destroy(conn);
        if (cfg)
            tdot_config_free(cfg);
    }
    cJSON_Delete(doc);
    free(text);
}

/* A config with one device of `n` float64 points and `device` as its
 * protocol_address body. */
static tdot_config_t *device_of(size_t n, const char *address, tdot_connector_t **conn) {
    size_t cap = 256 + n * 64;
    char *toml = malloc(cap);
    size_t len = (size_t)snprintf(toml, cap,
                                  "[connector]\nprotocol = \"bench\"\n[[device]]\nname = "
                                  "\"plc0\"\nprotocol_address = { %s }\n",
                                  address);
    for (size_t i = 0; i < n; i++)
        len += (size_t)snprintf(toml + len, cap - len,
                                "[[device.point]]\nid = \"p%zu\"\ndatatype = \"float64\"\n"
                                "address = {}\n",
                                i);
    char err[512];
    tdot_config_t *cfg = load(toml, conn, err, sizeof err);
    free(toml);
    CHECK(cfg && *conn, "config refused: %s", err);
    return cfg;
}

static void latency_per_block(void) {
    tdot_connector_t *conn = NULL;
    tdot_config_t *cfg = device_of(2000, "latency = \"2ms\", batch = 500", &conn);
    if (!cfg || !conn)
        return;
    tdot_device_t *dev = &cfg->devices[0];
    tdot_point_t *pts[500];
    tdot_sample_t *out = calloc(500, sizeof *out);
    int rc[500];
    double t0 = now_s();
    for (size_t call = 0; call < 4; call++) {
        for (size_t k = 0; k < 500; k++) {
            pts[k] = &dev->points[call * 500 + k];
            tdot_sample_init(&out[k]);
        }
        size_t got = conn->read_points(conn, dev, pts, 500, out, rc);
        CHECK(got == 500 && out[0].quality == TDOT_Q_GOOD, "read %zu points", got);
    }
    double took = now_s() - t0;
    CHECK(took >= 0.008, "2000 points took %.4f s, want at least 8 ms", took);
    free(out);
    conn->destroy(conn);
    tdot_config_free(cfg);
}

static size_t pushed;

static void count(void *ctx, tdot_device_t *dev, tdot_point_t *pt, const tdot_sample_t *s) {
    (void)ctx;
    (void)dev;
    (void)pt;
    if (s->quality == TDOT_Q_GOOD)
        pushed++;
}

/* push_rate 10000 over one second of drains every `tick` seconds. */
static void push_rate(double tick) {
    tdot_connector_t *conn = NULL;
    tdot_config_t *cfg = device_of(1000, "push_rate = 10000", &conn);
    if (!cfg || !conn)
        return;
    tdot_device_t *dev = &cfg->devices[0];
    char err[256];
    CHECK(conn->subscribe_device(conn, dev, err, sizeof err) == 0, "subscribe: %s", err);
    CHECK(dev->points[0].subscribed && dev->points[999].subscribed, "points not subscribed");
    pushed = 0;
    double start = now_s();
    size_t drains = 0, smallest = (size_t)-1, largest = 0;
    while (now_s() - start < 1.0) {
        sleep_s(tick);
        size_t before = pushed;
        conn->drain_subscriptions(conn, dev, count, NULL);
        size_t got = pushed - before;
        drains++;
        if (got < smallest)
            smallest = got;
        if (got > largest)
            largest = got;
    }
    double elapsed = now_s() - start;
    double rate = (double)pushed / elapsed;
    CHECK(rate >= 9500 && rate <= 10500, "tick %.3f s: %.0f samples/s", tick, rate);
    if (tick >= 0.2)
        CHECK(smallest >= 1700 && largest <= 2600,
              "tick %.3f s: %zu..%zu samples per drain, want about 2000", tick, smallest,
              largest);
    printf("push at %.0f ms ticks: %zu drains, %.0f samples/s\n", tick * 1000, drains, rate);
    conn->destroy(conn);
    tdot_config_free(cfg);
}

/* What source_times saw: per sample its source time against the drain's
 * wall clock. */
static size_t timed, off_grid, too_old, in_future;
static double drain_wall_ms, previous_delivery_ms;

static void check_time(void *ctx, tdot_device_t *dev, tdot_point_t *pt, const tdot_sample_t *s) {
    (void)ctx;
    (void)dev;
    (void)pt;
    timed++;
    if (fmod(s->source_ts_ms, 10.0) != 0)
        off_grid++;
    /* Generated since the previous delivery, then moved back by up to the
     * 50 ms jitter and down to the 10 ms grid. A late drain (a loaded host)
     * widens the gap, so the bound follows the gap rather than the interval. */
    double lag = drain_wall_ms - s->source_ts_ms;
    if (lag > drain_wall_ms - previous_delivery_ms + 50 + 10 + 1)
        too_old++;
    if (lag < -1)
        in_future++;
}

/* Source times as the Rust module's (connector-bench, push_ts_jitter_and_grouping):
 * delivered once per publish_interval, at most ts_jitter before generation and
 * so at most publish_interval + ts_jitter before delivery, on the
 * ts_resolution grid. */
static void source_times(void) {
    tdot_connector_t *conn = NULL;
    tdot_config_t *cfg = device_of(
        100, "push_rate = 1000, ts_jitter = \"50ms\", ts_resolution = \"10ms\", "
             "publish_interval = \"100ms\"",
        &conn);
    if (!cfg || !conn)
        return;
    tdot_device_t *dev = &cfg->devices[0];
    char err[256];
    CHECK(conn->subscribe_device(conn, dev, err, sizeof err) == 0, "subscribe: %s", err);
    timed = off_grid = too_old = in_future = 0;
    size_t deliveries = 0;
    struct timespec t0;
    clock_gettime(CLOCK_REALTIME, &t0);
    previous_delivery_ms = (double)t0.tv_sec * 1000.0 + (double)t0.tv_nsec / 1e6;
    double start = now_s();
    while (now_s() - start < 0.55) {
        sleep_s(0.010);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        drain_wall_ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
        size_t before = timed;
        conn->drain_subscriptions(conn, dev, check_time, NULL);
        if (timed > before) {
            deliveries++;
            previous_delivery_ms = drain_wall_ms;
        }
    }
    CHECK(deliveries >= 3 && deliveries <= 6, "%zu deliveries in 0.55 s, want one per 100 ms",
          deliveries);
    CHECK(timed >= 400, "%zu samples", timed);
    CHECK(off_grid == 0 && too_old == 0 && in_future == 0,
          "%zu off the 10 ms grid, %zu older than their delivery gap allows, %zu in the future", off_grid,
          too_old, in_future);
    conn->destroy(conn);
    tdot_config_free(cfg);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <doc/contract/test-vectors/bench>\n", argv[0]);
        return 2;
    }
    value_vectors(argv[1]);
    config_errors(argv[1]);
    latency_per_block();
    push_rate(0.010);
    push_rate(0.200);
    source_times();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("bench module: ok\n");
    return 0;
}
