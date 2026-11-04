/* Reporting-policy runner: drives the C state machine (sdk/src/report.c)
 * through the shared vectors (doc/contract/test-vectors/report/vectors.json)
 * the Rust SDK's report.rs runs too, so the two publish the same readings at
 * the same times. A published item is identified by its `ts`, which is where
 * the runner puts the sample's id: that also checks a held reading keeps its
 * own ts.
 *
 *   tedge-dot-report <vectors.json>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/report.h"
#include "tedge_dot/runtime.h"

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

/* The comparable part of a vector's sample, as report.rs's test builds it. */
static void obs_of(const cJSON *sample, tdot_report_obs_t *obs) {
    memset(obs, 0, sizeof *obs);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(sample, "num"))) {
        obs->is_num = true;
        obs->num = v->valuedouble;
    } else if (cJSON_GetObjectItem(sample, "nan")) {
        obs->is_num = true;
        obs->num = NAN;
    } else if (cJSON_IsBool(v = cJSON_GetObjectItem(sample, "bool"))) {
        snprintf(obs->other, sizeof obs->other, "b:%s", cJSON_IsTrue(v) ? "true" : "false");
    } else if (cJSON_IsString(v = cJSON_GetObjectItem(sample, "str"))) {
        snprintf(obs->other, sizeof obs->other, "s:%s", v->valuestring);
    } else {
        v = cJSON_GetObjectItem(sample, "raw");
        snprintf(obs->other, sizeof obs->other, "r:%s",
                 cJSON_IsString(v) ? v->valuestring : "");
    }
}

static tdot_quality_t quality_of(const cJSON *sample) {
    const cJSON *q = cJSON_GetObjectItem(sample, "quality");
    if (cJSON_IsString(q) && strcmp(q->valuestring, "bad") == 0)
        return TDOT_Q_BAD;
    if (cJSON_IsString(q) && strcmp(q->valuestring, "stale") == 0)
        return TDOT_Q_STALE;
    return TDOT_Q_GOOD;
}

/* The ids a step expects published, joined with ",". */
static void expected_of(const cJSON *step, char *dst, size_t len) {
    dst[0] = '\0';
    const cJSON *id;
    size_t used = 0;
    cJSON_ArrayForEach(id, cJSON_GetObjectItem(step, "publish")) {
        used += (size_t)snprintf(dst + used, len - used, "%s%s", used ? "," : "",
                                 cJSON_IsString(id) ? id->valuestring : "?");
        if (used >= len)
            break;
    }
}

static void run_vectors(const cJSON *doc) {
    const cJSON *vectors = cJSON_GetObjectItem(doc, "vectors");
    CHECK(cJSON_GetArraySize(vectors) > 0, "the vector file holds no vectors");
    int ran = 0;
    const cJSON *vector;
    cJSON_ArrayForEach(vector, vectors) {
        const char *name = cJSON_GetObjectItem(vector, "name")->valuestring;
        tdot_report_policy_t policy;
        tdot_report_policy_from_json(cJSON_GetObjectItem(vector, "policy"), &policy);
        bool pushed = cJSON_IsTrue(cJSON_GetObjectItem(vector, "pushed"));
        tdot_report_state_t st;
        tdot_report_init(&st, &policy, 0);
        int index = 0;
        const cJSON *step;
        cJSON_ArrayForEach(step, cJSON_GetObjectItem(vector, "steps")) {
            double at_ms = cJSON_GetObjectItem(step, "at")->valuedouble;
            int64_t now = (int64_t)at_ms * 1000000;
            char published[256] = "", expected[256];
            bool read = false;
            const cJSON *sample = cJSON_GetObjectItem(step, "sample");
            if (sample) {
                tdot_report_item_t item;
                memset(&item, 0, sizeof item);
                tdot_sample_init(&item.sample);
                item.sample.quality = quality_of(sample);
                snprintf(item.ts, sizeof item.ts, "%s",
                         cJSON_GetObjectItem(sample, "id")->valuestring);
                tdot_report_obs_t obs;
                obs_of(sample, &obs);
                const cJSON *src = cJSON_GetObjectItem(sample, "src");
                if (cJSON_IsNumber(src)) {
                    tdot_report_item_t released;
                    bool has_released = false;
                    bool now_too = tdot_report_offer_replayed(
                        &st, &item, &obs, item.sample.quality,
                        (int64_t)src->valuedouble * 1000000, now, &released, &has_released);
                    if (has_released) {
                        snprintf(published, sizeof published, "%s", released.ts);
                        tdot_report_item_release(&released);
                    }
                    if (now_too)
                        snprintf(published + strlen(published), sizeof published - strlen(published),
                                 "%s%s", published[0] ? "," : "", item.ts);
                } else if (tdot_report_offer(&st, &item, &obs, item.sample.quality, now)) {
                    snprintf(published, sizeof published, "%s", item.ts);
                }
            } else if (cJSON_GetObjectItem(step, "tick")) {
                tdot_report_item_t out;
                if (tdot_report_due(&st, now, pushed, &out, &read)) {
                    snprintf(published, sizeof published, "%s", out.ts);
                    tdot_report_item_release(&out);
                }
            } else if (cJSON_GetObjectItem(step, "no_data")) {
                tdot_report_no_data(&st);
            } else if (cJSON_GetObjectItem(step, "reset")) {
                tdot_report_reset(&st, now);
            } else {
                CHECK(false, "%s: step %d has no kind", name, index);
            }
            expected_of(step, expected, sizeof expected);
            CHECK(strcmp(published, expected) == 0,
                  "%s: step %d (at %.0fms) published [%s], expected [%s]", name, index,
                  at_ms, published, expected);
            bool expected_read = cJSON_IsTrue(cJSON_GetObjectItem(step, "read"));
            CHECK(read == expected_read, "%s: step %d (at %.0fms) read %d, expected %d", name,
                  index, at_ms, read, expected_read);
            index++;
        }
        tdot_report_free(&st);
        ran++;
    }
    printf("report: %d vector(s) run\n", ran);
}

static const int64_t S = 1000000000;

/* A heartbeat at or under the rate limit arises only through inheritance
 * (a single table with it is refused): it is raised to twice the rate limit,
 * with a warning. Mirrors report.rs::inherited_heartbeat_under_the_rate_limit_is_raised. */
static void check_inherited_heartbeat_is_raised(void) {
    cJSON *merged = cJSON_Parse("{\"min_interval\":\"1h\",\"max_interval\":\"30m\"}");
    tdot_report_policy_t p;
    char warning[256] = "";
    CHECK(tdot_report_effective(merged, &p, warning, sizeof warning),
          "a heartbeat under the rate limit must warn");
    CHECK(p.max_interval == 7200 * S, "the heartbeat must be raised to 2h, got %lld",
          (long long)p.max_interval);
    CHECK(strcmp(warning, "the inherited report.max_interval (1800s) is not longer than "
                          "report.min_interval (3600s); using 7200s") == 0,
          "warning: %s", warning);
    cJSON_Delete(merged);
    merged = cJSON_Parse("{\"min_interval\":\"10s\",\"max_interval\":\"30m\"}");
    CHECK(!tdot_report_effective(merged, &p, warning, sizeof warning),
          "a heartbeat over the rate limit is fine");
    cJSON_Delete(merged);
}

/* Mirrors report.rs::empty_table_is_passthrough. */
static void check_passthrough(void) {
    tdot_report_policy_t p;
    cJSON *table = cJSON_CreateObject();
    tdot_report_policy_from_json(table, &p);
    CHECK(tdot_report_is_passthrough(&p), "an empty table must be the passthrough");
    cJSON_Delete(table);
    table = cJSON_Parse("{\"on_change\":false,\"deadband\":0,\"max_interval\":\"0\"}");
    tdot_report_policy_from_json(table, &p);
    CHECK(tdot_report_is_passthrough(&p), "switched-off settings must be the passthrough");
    cJSON_Delete(table);
    /* The loader keeps numbers as written, as raw text. */
    table = cJSON_CreateObject();
    cJSON_AddItemToObject(table, "deadband", cJSON_CreateRaw("0.5"));
    tdot_report_policy_from_json(table, &p);
    CHECK(p.deadband_kind == TDOT_DEADBAND_ABSOLUTE && p.deadband == 0.5,
          "a raw-text deadband must be read");
    cJSON_Delete(table);
}

static void check_percent_grammar(void) {
    static const char *good[] = {"2%", "2.5%", "0%", "100%"};
    static const char *bad[] = {"%", "-1%", "2", ".5%", "5.%", "1.2.3%", "2 %", "x%"};
    for (size_t i = 0; i < sizeof good / sizeof *good; i++)
        CHECK(tdot_report_parse_percent(good[i], NULL), "'%s' must be a percentage", good[i]);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(!tdot_report_parse_percent(bad[i], NULL), "'%s' must be refused", bad[i]);
}

/* A held reading keeps its own copy of a module's per-sample address echo,
 * which is only borrowed for the call that offered it. */
static void check_held_item_owns_its_address(void) {
    tdot_report_policy_t p = {.min_interval = 10 * S};
    tdot_report_state_t st;
    tdot_report_init(&st, &p, 0);
    tdot_report_item_t item;
    memset(&item, 0, sizeof item);
    tdot_sample_init(&item.sample);
    tdot_report_obs_t obs = {.is_num = true, .num = 1};
    CHECK(tdot_report_offer(&st, &item, &obs, TDOT_Q_GOOD, 0), "the first reading publishes");
    char addr[] = "{\"source\":\"10.0.0.1\"}";
    item.sample.addr_json = addr;
    snprintf(item.ts, sizeof item.ts, "held");
    obs.num = 2;
    CHECK(!tdot_report_offer(&st, &item, &obs, TDOT_Q_GOOD, 2 * S), "a reading is held");
    addr[2] = 'X'; /* the module's buffer is gone */
    tdot_report_item_t out;
    bool read;
    CHECK(tdot_report_due(&st, 10 * S, false, &out, &read), "the held reading publishes");
    CHECK(strcmp(out.ts, "held") == 0 && out.sample.addr_json &&
              strcmp(out.sample.addr_json, "{\"source\":\"10.0.0.1\"}") == 0,
          "a held reading keeps its ts and address, got %s %s", out.ts,
          out.sample.addr_json ? out.sample.addr_json : "(null)");
    tdot_report_item_release(&out);
    tdot_report_free(&st);
}

/* ---- the policy in the runtime ------------------------------------------------
 * A fake module under the real runtime loop, in stdout mode: a flat polled
 * point, a pushed point nothing is pushed for, and a trap-like pushed point
 * that has nothing to read on demand. */

static int trap_reads = 0;

static int fake_configure(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                          size_t errlen) {
    (void)self, (void)cfg, (void)err, (void)errlen;
    return 0;
}

static int fake_connect(tdot_connector_t *self, tdot_device_t *dev, char *err,
                        size_t errlen) {
    (void)self, (void)dev, (void)err, (void)errlen;
    return 0;
}

static int fake_read(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                     tdot_sample_t *out) {
    (void)self, (void)dev;
    if (strcmp(pt->id, "trap") == 0) {
        trap_reads++;
        return TDOT_READ_NO_DATA;
    }
    out->value.kind = TDOT_VAL_NUM;
    out->value.num = strcmp(pt->id, "flat") == 0 ? 42 : 7;
    return 0;
}

static int fake_write(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      const tdot_value_t *value, char *err, size_t errlen) {
    (void)self, (void)dev, (void)pt, (void)value;
    snprintf(err, errlen, "read-only");
    return -1;
}

static int fake_subscribe(tdot_connector_t *self, tdot_device_t *dev, char *err,
                          size_t errlen) {
    (void)self, (void)err, (void)errlen;
    for (size_t j = 0; j < dev->npoints; j++)
        dev->points[j].subscribed = strcmp(dev->points[j].id, "flat") != 0;
    return 0;
}

static int fake_drain(tdot_connector_t *self, tdot_device_t *dev, tdot_sample_sink_t sink,
                      void *ctx) {
    (void)self, (void)dev, (void)sink, (void)ctx;
    return 0; /* nothing is ever pushed */
}

static void fake_disconnect(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self, (void)dev;
}

static void fake_destroy(tdot_connector_t *self) { free(self); }

static void check_policy_in_the_runtime(void) {
    char dir[] = "/tmp/tdot-report-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        failures++;
        return;
    }
    char cfg_path[256], out_path[256];
    snprintf(cfg_path, sizeof cfg_path, "%s/fake.toml", dir);
    snprintf(out_path, sizeof out_path, "%s/out.jsonl", dir);
    FILE *fp = fopen(cfg_path, "w");
    fputs("[connector]\nprotocol = \"fake\"\npoll_interval = \"100ms\"\n"
          "report = { max_interval = \"1s\" }\n"
          "[[device]]\nname = \"d\"\nprotocol_address = {}\n"
          "  [[device.point]]\n  id = \"flat\"\n  datatype = \"uint16\"\n  address = {}\n"
          "  report = { on_change = true }\n"
          "  [[device.point]]\n  id = \"pushed\"\n  datatype = \"uint16\"\n  address = {}\n"
          "  [[device.point]]\n  id = \"trap\"\n  datatype = \"uint16\"\n  address = {}\n",
          fp);
    fclose(fp);

    fflush(stdout); /* or the child writes what is buffered a second time */
    pid_t child = fork();
    if (child == 0) {
        if (!freopen(out_path, "w", stdout))
            _exit(3);
        char err[256];
        tdot_config_t *cfg = tdot_config_load(cfg_path, err, sizeof err);
        if (!cfg)
            _exit(4);
        tdot_connector_t *conn = calloc(1, sizeof *conn);
        conn->protocol = "fake";
        conn->configure = fake_configure;
        conn->connect_device = fake_connect;
        conn->read_point = fake_read;
        conn->write_point = fake_write;
        conn->subscribe_device = fake_subscribe;
        conn->drain_subscriptions = fake_drain;
        conn->disconnect_device = fake_disconnect;
        conn->destroy = fake_destroy;
        tdot_run_opts_t opts = {.output = TDOT_OUTPUT_STDOUT, .duration_s = 2.6};
        int rc = tdot_runtime_run(conn, cfg, &opts);
        fflush(stdout);
        /* The trap point is read once, then left alone until a reset. */
        _exit(rc != 0 ? 5 : trap_reads == 1 ? 0 : 10 + trap_reads);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the runtime run failed or read the trap point more than once (exit %d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    int flat = 0, pushed = 0, trap = 0;
    double last_seq = 0;
    fp = fopen(out_path, "r");
    char line[4096];
    while (fp && fgets(line, sizeof line, fp)) {
        cJSON *sample = cJSON_Parse(line);
        const cJSON *point = cJSON_GetObjectItem(sample, "point");
        const char *id = cJSON_IsString(point) ? point->valuestring : "";
        const cJSON *jseq = cJSON_GetObjectItem(sample, "seq");
        double seq = cJSON_IsNumber(jseq) ? jseq->valuedouble : 0;
        if (strcmp(id, "flat") == 0) {
            flat++;
            /* Suppressed readings consume no sequence number. */
            CHECK(seq == last_seq + 1, "flat's seq must be gap-free: %.0f after %.0f", seq,
                  last_seq);
            last_seq = seq;
        } else if (strcmp(id, "pushed") == 0) {
            pushed++;
        } else if (strcmp(id, "trap") == 0) {
            trap++;
        }
        cJSON_Delete(sample);
    }
    if (fp)
        fclose(fp);
    /* ~26 polls of an unchanging value: the first, then one heartbeat per
     * second. */
    CHECK(flat >= 2 && flat <= 4, "flat: expected its first reading and 1-2 heartbeats, got %d",
          flat);
    /* Nothing pushed: only the on-demand heartbeat reads publish it. */
    CHECK(pushed >= 1 && pushed <= 3, "pushed: expected 1-3 heartbeat reads, got %d", pushed);
    CHECK(trap == 0, "trap: nothing to read must publish nothing, got %d", trap);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        printf("warn: could not clean %s\n", dir);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: tedge-dot-report <vectors.json>\n", stderr);
        return 2;
    }
    char *text = slurp(argv[1]);
    cJSON *doc = text ? cJSON_Parse(text) : NULL;
    if (!doc) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    run_vectors(doc);
    check_inherited_heartbeat_is_raised();
    check_passthrough();
    check_percent_grammar();
    check_held_item_owns_its_address();
    check_policy_in_the_runtime();
    cJSON_Delete(doc);
    free(text);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("report: all checks passed\n");
    return 0;
}
