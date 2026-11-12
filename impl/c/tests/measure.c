/* Grouped output runner: drives the C batcher (sdk/src/measure.c) through
 * the shared vectors (doc/contract/test-vectors/measurements/vectors.json) the
 * Rust SDK's measure.rs runs too, so the two publish the same measurements, in
 * the same messages, with the same units metadata. Payloads are compared as
 * JSON, not as text.
 *
 *   tedge-dot-measure <vectors.json>
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/config.h"
#include "tedge_dot/measure.h"

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

/* RFC 3339 with milliseconds and a Z, as the vectors write times. */
static double parse_ms(const char *s) {
    struct tm tm = {0};
    int ms = 0;
    sscanf(s, "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
           &tm.tm_min, &tm.tm_sec, &ms);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (double)timegm(&tm) * 1000.0 + ms;
}

typedef struct {
    cJSON *messages; /* array of {topic, retain, payload} */
    cJSON *lengths;  /* the payload text length of each message */
    char *last;      /* the last payload as text */
} collect_t;

static void collect(void *ctx, const char *topic, const char *payload, bool retain) {
    collect_t *c = ctx;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "topic", topic);
    cJSON_AddBoolToObject(m, "retain", retain);
    cJSON *body = payload[0] ? cJSON_Parse(payload) : cJSON_CreateString("");
    if (!body) {
        printf("FAIL: not JSON: %s\n", payload);
        failures++;
        body = cJSON_CreateString(payload);
    }
    cJSON_AddItemToObject(m, "payload", body);
    cJSON_AddItemToArray(c->messages, m);
    if (c->lengths)
        cJSON_AddItemToArray(c->lengths, cJSON_CreateNumber((double)strlen(payload)));
    free(c->last);
    c->last = strdup(payload);
}

/* The case's configuration, written to a file for the loader, with the points
 * `generate` asks for appended: every `{i:04}` of its id pattern is the
 * point's index. */
static tdot_config_t *config_of(const cJSON *c, char *err, size_t errlen) {
    char path[] = "/tmp/tedge-dot-measure-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
    fputs(cJSON_GetObjectItem(c, "config")->valuestring, fp);
    const cJSON *g = cJSON_GetObjectItem(c, "generate");
    if (g) {
        const char *pattern = cJSON_GetObjectItem(g, "id")->valuestring;
        int count = cJSON_GetObjectItem(g, "count")->valueint;
        for (int i = 0; i < count; i++) {
            fputs("[[device.point]]\nid = \"", fp);
            for (const char *p = pattern; *p;) {
                if (strncmp(p, "{i:04}", 6) == 0) {
                    fprintf(fp, "%04d", i);
                    p += 6;
                } else {
                    fputc(*p++, fp);
                }
            }
            fprintf(fp, "\"\ndatatype = \"%s\"\naddress = {}\n",
                    cJSON_GetObjectItem(g, "datatype")->valuestring);
        }
    }
    fclose(fp);
    tdot_config_t *cfg = tdot_config_load(path, err, errlen);
    unlink(path);
    return cfg;
}

static size_t device_index(const tdot_config_t *cfg, const char *name) {
    for (size_t d = 0; d < cfg->ndevices; d++)
        if (strcmp(cfg->devices[d].name, name) == 0)
            return d;
    return (size_t)-1;
}

/* Offer the reading `r` describes for point `k` of device `d`, as a module
 * would return it and the runtime would hand it over. */
static tdot_routing_t offer(tdot_batcher_t *b, tdot_config_t *cfg, size_t d, size_t k,
                            const cJSON *r, long long now_ns, collect_t *out) {
    tdot_point_t *pt = &cfg->devices[d].points[k];
    tdot_sample_t s;
    tdot_sample_init(&s);
    const cJSON *v = cJSON_GetObjectItem(r, "value");
    if (cJSON_IsBool(v)) {
        s.value.kind = TDOT_VAL_BOOL;
        s.value.b = cJSON_IsTrue(v);
    } else if (cJSON_IsNumber(v)) {
        s.value.kind = TDOT_VAL_NUM;
        s.value.num = v->valuedouble;
    } else if (cJSON_IsString(v)) {
        s.value.kind = TDOT_VAL_STR;
        snprintf(s.value.str, sizeof s.value.str, "%s", v->valuestring);
    }
    if (pt->mode == TDOT_MODE_RAW)
        s.value.kind = TDOT_VAL_NONE;
    const cJSON *q = cJSON_GetObjectItem(r, "quality");
    if (q && strcmp(q->valuestring, "bad") == 0)
        tdot_sample_bad(&s, "bad");
    else if (q && strcmp(q->valuestring, "stale") == 0)
        s.quality = TDOT_Q_STALE;
    double ts = parse_ms(cJSON_GetObjectItem(r, "ts")->valuestring);
    return tdot_batcher_offer(b, d, k, pt, &s, ts, now_ns, collect, out);
}

static void run_case(const cJSON *c) {
    const char *name = cJSON_GetObjectItem(c, "name")->valuestring;
    char err[512];
    tdot_config_t *cfg = config_of(c, err, sizeof err);
    if (!cfg) {
        CHECK(0, "%s: config: %s", name, err);
        return;
    }
    double wall0 = parse_ms("2026-01-01T00:00:00.000Z");
    tdot_batcher_t *b = tdot_batcher_new(cfg, 0, NULL);
    collect_t out = {.messages = cJSON_CreateArray(), .lengths = cJSON_CreateArray()};
    cJSON *routing = cJSON_CreateArray();
    const cJSON *step;
    cJSON_ArrayForEach(step, cJSON_GetObjectItem(c, "steps")) {
        long long at = (long long)cJSON_GetObjectItem(step, "at")->valuedouble;
        long long now_ns = at * 1000000LL;
        const cJSON *r;
        if ((r = cJSON_GetObjectItem(step, "read"))) {
            size_t d = device_index(cfg, cJSON_GetObjectItem(r, "device")->valuestring);
            tdot_point_t *pt =
                tdot_device_point(&cfg->devices[d], cJSON_GetObjectItem(r, "point")->valuestring);
            tdot_routing_t rt = offer(b, cfg, d, (size_t)(pt - cfg->devices[d].points), r,
                                      now_ns, &out);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "sample", rt.sample);
            cJSON_AddBoolToObject(e, "measured", rt.measured);
            cJSON_AddItemToArray(routing, e);
        } else if ((r = cJSON_GetObjectItem(step, "read_all"))) {
            size_t d = device_index(cfg, cJSON_GetObjectItem(r, "device")->valuestring);
            for (size_t k = 0; k < cfg->devices[d].npoints; k++)
                offer(b, cfg, d, k, r, now_ns, &out);
        } else if (cJSON_GetObjectItem(step, "pass")) {
            tdot_batcher_end_of_pass(b, collect, &out);
        } else if (cJSON_GetObjectItem(step, "due")) {
            tdot_batcher_due(b, now_ns, wall0 + (double)at, collect, &out);
        } else if ((r = cJSON_GetObjectItem(step, "stale"))) {
            tdot_batcher_set_stale(b, device_index(cfg, cJSON_GetObjectItem(r, "device")->valuestring),
                                   cJSON_IsTrue(cJSON_GetObjectItem(r, "stale")), now_ns);
        } else {
            CHECK(0, "%s: unknown step", name);
        }
    }
    const cJSON *split = cJSON_GetObjectItem(c, "split");
    if (split) {
        const cJSON *want = cJSON_GetObjectItem(split, "series");
        CHECK(cJSON_GetArraySize(out.messages) == cJSON_GetArraySize(want),
              "%s: %d messages, want %d", name, cJSON_GetArraySize(out.messages),
              cJSON_GetArraySize(want));
        for (int i = 0; i < cJSON_GetArraySize(out.messages) && i < cJSON_GetArraySize(want); i++) {
            const cJSON *m = cJSON_GetArrayItem(out.messages, i);
            CHECK(strcmp(cJSON_GetObjectItem(m, "topic")->valuestring,
                         cJSON_GetObjectItem(split, "topic")->valuestring) == 0,
                  "%s: topic", name);
            /* Every fragment's series count, and the estimate stays within
             * the budget. */
            const cJSON *payload = cJSON_GetObjectItem(m, "payload"), *x;
            int n = 0;
            cJSON_ArrayForEach(x, payload) if (strcmp(x->string, "time") != 0) n +=
                cJSON_GetArraySize(x);
            int len = cJSON_GetArrayItem(out.lengths, i)->valueint;
            CHECK(len + TDOT_SERIES_OVERHEAD * n <= TDOT_SIZE_BUDGET, "%s: message %d is %d bytes",
                  name, i, len);
            CHECK(n == cJSON_GetArrayItem(want, i)->valueint, "%s: message %d has %d series, want %d",
                  name, i, n, cJSON_GetArrayItem(want, i)->valueint);
        }
    } else {
        const cJSON *want = cJSON_GetObjectItem(c, "messages");
        if (!cJSON_Compare(out.messages, want, true)) {
            char *got = cJSON_PrintUnformatted(out.messages), *exp = cJSON_PrintUnformatted(want);
            CHECK(0, "%s:\n  got  %s\n  want %s", name, got, exp);
            free(got);
            free(exp);
        }
        const cJSON *want_routing = cJSON_GetObjectItem(c, "routing");
        if (!cJSON_Compare(routing, want_routing, true)) {
            char *got = cJSON_PrintUnformatted(routing), *exp = cJSON_PrintUnformatted(want_routing);
            CHECK(0, "%s: routing\n  got  %s\n  want %s", name, got, exp);
            free(got);
            free(exp);
        }
    }
    cJSON_Delete(out.messages);
    cJSON_Delete(out.lengths);
    free(out.last);
    cJSON_Delete(routing);
    tdot_batcher_free(b);
    tdot_config_free(cfg);
}

/* The parts of a split frame layer share its time, and the next layer is
 * ordered after that time, not after each part (the vectors' `split` form does
 * not carry times). As split_frame_layer_parts_share_its_time in measure.rs. */
static void split_frame_layer_parts_share_its_time(void) {
    cJSON *c = cJSON_Parse(
        "{\"config\":\"[connector]\\nprotocol = \\\"opcua\\\"\\noutput = \\\"measurements\\\"\\n\\n"
        "[[device]]\\nname = \\\"plc1\\\"\\nprotocol_address = {}\\n"
        "batch = { group_by = \\\"frame\\\", window = \\\"100ms\\\" }\\n\","
        "\"generate\":{\"count\":300,\"datatype\":\"float64\",\"id\":"
        "\"a_long_point_name_that_makes_each_series_take_about_a_hundred_bytes_of_the_message_{i:04}\"}}");
    char err[512];
    tdot_config_t *cfg = c ? config_of(c, err, sizeof err) : NULL;
    CHECK(cfg, "split frame layer: config: %s", c ? err : "not JSON");
    if (!cfg) {
        cJSON_Delete(c);
        return;
    }
    tdot_batcher_t *b = tdot_batcher_new(cfg, 0, NULL);
    collect_t out = {.messages = cJSON_CreateArray()};
    cJSON *r = cJSON_Parse("{\"value\":1,\"ts\":\"2026-01-01T00:00:00.040Z\"}");
    for (size_t k = 0; k < cfg->devices[0].npoints; k++)
        offer(b, cfg, 0, k, r, 0, &out);
    cJSON_ReplaceItemInObject(r, "ts", cJSON_CreateString("2026-01-01T00:00:00.030Z"));
    offer(b, cfg, 0, 0, r, 10 * 1000000LL, &out);
    tdot_batcher_due(b, 100 * 1000000LL, parse_ms("2026-01-01T00:00:00.100Z"), collect, &out);
    int n = 0, parts = 0;
    const cJSON *m;
    const char *last = NULL;
    cJSON_ArrayForEach(m, out.messages) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(m, "retain")))
            continue;
        const char *t = cJSON_GetObjectItem(cJSON_GetObjectItem(m, "payload"), "time")->valuestring;
        if (last)
            parts += strcmp(last, "2026-01-01T00:00:00.040Z") == 0;
        last = t;
        n++;
    }
    CHECK(n >= 3 && parts == n - 1, "split frame layer: %d messages, %d parts at .040", n, parts);
    CHECK(last && strcmp(last, "2026-01-01T00:00:00.041Z") == 0, "split frame layer: last at %s",
          last ? last : "(none)");
    cJSON_Delete(r);
    cJSON_Delete(out.messages);
    free(out.last);
    tdot_batcher_free(b);
    tdot_config_free(cfg);
    cJSON_Delete(c);
}

/* Two fragments of one type and time share one message, as text: the
 * fragments in the order their first point is configured, each fragment's
 * series in the order they were added. As several_fragments_share_a_message in
 * measure.rs. */
static void several_fragments_share_a_message(void) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(
        c, "config",
        "[connector]\nprotocol = \"opcua\"\noutput = \"group\"\n"
        "measurement = { type = \"Combined\", point_separator = \".\" }\n\n"
        "[[device]]\nname = \"plc1\"\nprotocol_address = {}\n"
        "[[device.point]]\nid = \"c8y_Environmental.temperature\"\ndatatype = \"float64\"\naddress = {}\n"
        "[[device.point]]\nid = \"c8y_Battery.level\"\ndatatype = \"float64\"\naddress = {}\n"
        "[[device.point]]\nid = \"c8y_Battery.voltage\"\ndatatype = \"float64\"\naddress = {}\n");
    char err[512];
    tdot_config_t *cfg = c ? config_of(c, err, sizeof err) : NULL;
    CHECK(cfg, "several fragments: config: %s", c ? err : "not JSON");
    if (!cfg) {
        cJSON_Delete(c);
        return;
    }
    tdot_batcher_t *b = tdot_batcher_new(cfg, 0, NULL);
    collect_t out = {.messages = cJSON_CreateArray()};
    static const struct {
        size_t k;
        const char *value;
    } reads[] = {{1, "85"}, {0, "23.4"}, {2, "3.7"}};
    for (size_t i = 0; i < 3; i++) {
        char text[128];
        snprintf(text, sizeof text, "{\"value\":%s,\"ts\":\"2026-11-12T12:23:00.000Z\"}",
                 reads[i].value);
        cJSON *r = cJSON_Parse(text);
        offer(b, cfg, 0, reads[i].k, r, 0, &out);
        cJSON_Delete(r);
    }
    tdot_batcher_end_of_pass(b, collect, &out);
    const char *want = "{\"time\":\"2026-11-12T12:23:00.000Z\",\"c8y_Environmental\":{"
                       "\"temperature\":23.4},\"c8y_Battery\":{\"level\":85,\"voltage\":3.7}}";
    CHECK(cJSON_GetArraySize(out.messages) == 1 && out.last && strcmp(out.last, want) == 0,
          "several fragments: %d messages, last %s", cJSON_GetArraySize(out.messages),
          out.last ? out.last : "(none)");
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(out.messages, 0), "topic")->valuestring,
                 "te/device/plc1///m/Combined") == 0,
          "several fragments: topic");
    cJSON_Delete(out.messages);
    free(out.last);
    tdot_batcher_free(b);
    tdot_config_free(cfg);
    cJSON_Delete(c);
}

/* The type, fragment and series one reading of a point is published under:
 * the topic's last level, the body's fragment and its series. */
static void name_of(const char *connector, const char *device, const char *point_id,
                    const char *meta, char *out, size_t outlen) {
    char config[2048];
    snprintf(config, sizeof config,
             "[connector]\nprotocol = \"opcua\"\noutput = \"group\"\n%s\n[[device]]\n"
             "name = \"plc\"\nprotocol_address = {}\n%s\n[[device.point]]\nid = \"%s\"\n"
             "datatype = \"float64\"\naddress = {}\n%s\n",
             connector, device, point_id, meta);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "config", config);
    char err[512];
    tdot_config_t *cfg = config_of(c, err, sizeof err);
    cJSON_Delete(c);
    snprintf(out, outlen, "config: %s", cfg ? "" : err);
    if (!cfg)
        return;
    tdot_batcher_t *b = tdot_batcher_new(cfg, 0, NULL);
    collect_t got = {.messages = cJSON_CreateArray()};
    cJSON *r = cJSON_Parse("{\"value\":1,\"ts\":\"2026-01-01T00:00:00.000Z\"}");
    offer(b, cfg, 0, 0, r, 0, &got);
    tdot_batcher_end_of_pass(b, collect, &got);
    const cJSON *m;
    cJSON_ArrayForEach(m, got.messages) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(m, "retain")))
            continue;
        const char *topic = cJSON_GetObjectItem(m, "topic")->valuestring;
        const char *type = strstr(topic, "///m/");
        const cJSON *f = NULL, *x;
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(m, "payload")) if (strcmp(x->string, "time") != 0) f = x;
        snprintf(out, outlen, "%s|%s|%s", type ? type + 5 : topic, f ? f->string : "",
                 f && f->child ? f->child->string : "");
    }
    cJSON_Delete(r);
    cJSON_Delete(got.messages);
    free(got.last);
    tdot_batcher_free(b);
    tdot_config_free(cfg);
}

/* The names a point gets (§5.5), key by key: point meta, device, connector, the
 * separator, then the defaults; `group` names the type and the fragment of its
 * level. As names_resolve_key_by_key in measure.rs. */
static void names_resolve_key_by_key(void) {
    static const struct {
        const char *connector, *device, *point, *meta, *want;
    } cases[] = {
        {"", "", "flow", "", "opcua|opcua|flow"},
        /* The fragment defaults to the type. */
        {"", "measurement = { type = \"c8y_Sensor\" }", "flow", "", "c8y_Sensor|c8y_Sensor|flow"},
        {"measurement = { type = \"Combined\" }", "", "b",
         "meta = { measurement = { fragment = \"c8y_Battery\", series = \"level\" } }",
         "Combined|c8y_Battery|level"},
        /* The separator names the fragment under a type, and the type too
         * without one. */
        {"measurement = { type = \"Combined\", point_separator = \".\" }", "", "c8y_Battery.level",
         "", "Combined|c8y_Battery|level"},
        {"measurement = { point_separator = \".\" }", "", "Environment.Temperature", "",
         "Environment|Environment|Temperature"},
        /* `group` sets both at its level and wins over a lower level's type. */
        {"measurement = { type = \"Combined\" }", "", "p",
         "meta = { measurement = { group = \"Env\" } }", "Env|Env|p"},
        {"measurement = { group = \"G\", fragment = \"F\" }", "", "p", "", "G|F|p"},
        /* The device sits between the point and the connector. */
        {"measurement = { type = \"C\", series = \"s\" }", "measurement = { type = \"D\" }", "p",
         "meta = { measurement = { series = \"m\" } }", "D|D|m"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char got[512];
        name_of(cases[i].connector, cases[i].device, cases[i].point, cases[i].meta, got,
                sizeof got);
        CHECK(strcmp(got, cases[i].want) == 0, "names case %zu: %s, want %s", i, got,
              cases[i].want);
    }
}

/* The significant digits of a number's text, without sign, point, exponent,
 * leading or trailing zeros. */
static void digits_of(const char *text, char *out, size_t len) {
    size_t n = 0;
    for (const char *p = text; *p && *p != 'e' && n + 1 < len; p++)
        if (*p >= '0' && *p <= '9' && (n > 0 || *p != '0'))
            out[n++] = *p;
    while (n > 1 && out[n - 1] == '0')
        n--;
    out[n] = '\0';
}

static bool same_digits(const char *a, const char *b) {
    char x[40], y[40];
    digits_of(a, x, sizeof x);
    digits_of(b, y, sizeof y);
    return strcmp(x, y) == 0;
}

/* Numbers as the Rust build prints them (ryu), so the size estimate agrees. */
static void numbers(void) {
    static const struct {
        double n;
        const char *text;
    } cases[] = {
        {118, "118"},       {-3, "-3"},         {118.6, "118.6"},
        {0.1 + 0.2, "0.30000000000000004"},     {1e20, "1e20"},
        {1.5e-7, "1.5e-7"}, {0.0001, "0.0001"}, {1e15, "1000000000000000.0"},
        {123.456, "123.456"}, {-0.5, "-0.5"},   {1.2345678901234568e20, "1.2345678901234568e20"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char buf[64];
        tdot_format_number(cases[i].n, buf, sizeof buf);
        CHECK(strcmp(buf, cases[i].text) == 0, "number %.17g: %s, want %s", cases[i].n, buf,
              cases[i].text);
    }
    /* The search for the shortest precision gives the digits a linear search
     * gives: random doubles across the exponent range, means of the kind an
     * aggregate publishes, and powers of two. */
    unsigned long long x = 0x9e3779b97f4a7c15ULL;
    int differ = 0;
    for (int i = 0; i < 200000 && differ < 5; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        double d;
        if (i % 3 == 0) {
            memcpy(&d, &x, sizeof d);
            if (!isfinite(d))
                continue;
        } else if (i % 3 == 1) {
            d = (double)(x % 1000000) / 7.0 * (i % 2 ? 1e-3 : 1e3);
        } else {
            d = ldexp(1.0, (int)(x % 2000) - 1000) * (x & 1 ? -1 : 1);
        }
        char want[40];
        for (int prec = 0; prec <= 16; prec++) {
            snprintf(want, sizeof want, "%.*e", prec, d);
            if (strtod(want, NULL) == d)
                break;
        }
        /* The formatter's significant digits are the linear search's. */
        char got[64];
        tdot_format_number(d, got, sizeof got);
        if (!same_digits(got, want) || strtod(got, NULL) != d) {
            printf("FAIL number %.17g: %s, linear search %s\n", d, got, want);
            differ++;
            failures++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vectors.json>\n", argv[0]);
        return 2;
    }
    char *text = slurp(argv[1]);
    cJSON *doc = text ? cJSON_Parse(text) : NULL;
    if (!doc) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(doc, "cases")) {
        run_case(c);
        n++;
    }
    numbers();
    split_frame_layer_parts_share_its_time();
    several_fragments_share_a_message();
    names_resolve_key_by_key();
    cJSON_Delete(doc);
    free(text);
    printf("%d measurement vectors, %d failure(s)\n", n, failures);
    return failures ? 1 : 0;
}
