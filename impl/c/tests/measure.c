/* Measurement output runner: drives the C batcher (sdk/src/measure.c) through
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
}

/* The case's configuration, written to a file for the loader, with the points
 * `generate` asks for appended. */
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
        const char *at = strstr(pattern, "{i:04}");
        int count = cJSON_GetObjectItem(g, "count")->valueint;
        for (int i = 0; i < count; i++)
            fprintf(fp, "[[device.point]]\nid = \"%.*s%04d%s\"\ndatatype = \"%s\"\naddress = {}\n",
                    (int)(at - pattern), pattern, i, at + 6,
                    cJSON_GetObjectItem(g, "datatype")->valuestring);
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
    collect_t out = {.messages = cJSON_CreateArray()};
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
                                   cJSON_IsTrue(cJSON_GetObjectItem(r, "stale")));
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
            const cJSON *payload = cJSON_GetObjectItem(m, "payload"), *group = NULL, *x;
            cJSON_ArrayForEach(x, payload) if (strcmp(x->string, "time") != 0) group = x;
            int n = group ? cJSON_GetArraySize(group) : 0;
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
    cJSON_Delete(routing);
    tdot_batcher_free(b);
    tdot_config_free(cfg);
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
    cJSON_Delete(doc);
    free(text);
    printf("%d measurement vectors, %d failure(s)\n", n, failures);
    return failures ? 1 : 0;
}
