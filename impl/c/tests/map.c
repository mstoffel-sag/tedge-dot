/* Value mapping (contract §4.3) through the shared vectors
 * (doc/contract/test-vectors/map/vectors.json), which the Rust SDK's map.rs
 * runs too.
 *
 * Maps are configuration, so each vector's map goes through the TOML loader:
 * the test renders it as a TOML inline table. Reads run tdot_map_apply after
 * the transform; writes go through tdot_connector_write, the one path every
 * write verb and the CLI take, so the inverse transform composes as in the
 * runtime; invalid maps go through tdot_map_parse and tdot_map_check_point.
 * Plus the loader (library replacement, resolved checks) and the envelope's
 * `source_value`.
 */
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/map.h"
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

/* An integer literal JSON cannot carry exactly through a double (the vectors
 * use 2^53 + 1) is tagged as a string before cJSON parses the file, and
 * rendered back as a bare TOML integer. */
#define BIG_TAG "#int#"

static char *slurp_tagged(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    fread(text, 1, (size_t)n, fp);
    fclose(fp);
    text[n] = '\0';
    char *out = malloc((size_t)n * 2 + 1);
    size_t o = 0;
    bool in_string = false;
    for (long i = 0; i < n; i++) {
        char c = text[i];
        if (in_string) {
            out[o++] = c;
            if (c == '\\' && i + 1 < n)
                out[o++] = text[++i];
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out[o++] = c;
            continue;
        }
        if (c == '-' || isdigit((unsigned char)c)) {
            /* a whole number token: tag it when it is an integer too long for
             * a double to carry exactly */
            long j = i;
            while (j < n && strchr("0123456789.eE+-", text[j]))
                j++;
            long digits = 0;
            bool integer = true;
            for (long k = i; k < j; k++) {
                if (isdigit((unsigned char)text[k]))
                    digits++;
                else if (!(k == i && text[k] == '-'))
                    integer = false;
            }
            if (integer && digits >= 16)
                o += (size_t)sprintf(out + o, "\"" BIG_TAG "%.*s\"", (int)(j - i), text + i);
            else
                o += (size_t)sprintf(out + o, "%.*s", (int)(j - i), text + i);
            i = j - 1;
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    free(text);
    return out;
}

/* A cJSON value as TOML (inline tables, arrays, scalars). */
static void toml_of(const cJSON *v, char *buf, size_t len, size_t *o) {
#define OUT(...) (*o += (size_t)snprintf(buf + *o, *o < len ? len - *o : 0, __VA_ARGS__))
    if (cJSON_IsObject(v)) {
        OUT("{ ");
        const cJSON *x;
        bool first = true;
        cJSON_ArrayForEach(x, v) {
            OUT("%s%s = ", first ? "" : ", ", x->string);
            toml_of(x, buf, len, o);
            first = false;
        }
        OUT(" }");
    } else if (cJSON_IsArray(v)) {
        OUT("[");
        const cJSON *x;
        bool first = true;
        cJSON_ArrayForEach(x, v) {
            OUT("%s", first ? "" : ", ");
            toml_of(x, buf, len, o);
            first = false;
        }
        OUT("]");
    } else if (cJSON_IsString(v)) {
        if (!strncmp(v->valuestring, BIG_TAG, strlen(BIG_TAG))) {
            OUT("%s", v->valuestring + strlen(BIG_TAG));
        } else {
            char *q = cJSON_PrintUnformatted(v);
            OUT("%s", q);
            free(q);
        }
    } else if (cJSON_IsBool(v)) {
        OUT("%s", cJSON_IsTrue(v) ? "true" : "false");
    } else if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == floor(d) && fabs(d) < 1e15)
            OUT("%lld", (long long)d);
        else
            OUT("%.17g", d);
    }
#undef OUT
}

static const cJSON *named_map(const cJSON *vectors, const cJSON *v) {
    const cJSON *m = cJSON_GetObjectItem(v, "map");
    if (cJSON_IsString(m))
        return cJSON_GetObjectItem(cJSON_GetObjectItem(vectors, "maps"), m->valuestring);
    return m;
}

/* Parse `map` (a cJSON object) through the TOML front end. */
static int parse_map(const cJSON *map, tdot_map_t **out, char *err, size_t errlen) {
    char text[4096];
    size_t o = 0;
    o += (size_t)snprintf(text, sizeof text, "map = ");
    toml_of(map, text, sizeof text, &o);
    char terr[256];
    toml_table_t *root = toml_parse(text, terr, sizeof terr);
    if (!root) {
        snprintf(err, errlen, "TOML: %s in %s", terr, text);
        return -2;
    }
    int rc = tdot_map_parse(toml_table_in(root, "map"), out, err, errlen);
    toml_free(root);
    return rc;
}

/* A vector value: {"num": n}, {"nan": true}, {"bool": b} or {"str": s}. */
static tdot_value_t vector_value(const cJSON *v) {
    tdot_value_t x;
    memset(&x, 0, sizeof x);
    const cJSON *f;
    if ((f = cJSON_GetObjectItem(v, "num"))) {
        x.kind = TDOT_VAL_NUM;
        x.num = f->valuedouble;
    } else if (cJSON_GetObjectItem(v, "nan")) {
        x.kind = TDOT_VAL_NUM;
        x.num = NAN;
    } else if ((f = cJSON_GetObjectItem(v, "bool"))) {
        x.kind = TDOT_VAL_BOOL;
        x.b = cJSON_IsTrue(f);
    } else {
        x.kind = TDOT_VAL_STR;
        snprintf(x.str, sizeof x.str, "%s", cJSON_GetObjectItem(v, "str")->valuestring);
    }
    return x;
}

static bool same_value(const tdot_value_t *a, const tdot_value_t *b) {
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case TDOT_VAL_NUM: return a->num == b->num;
    case TDOT_VAL_BOOL: return a->b == b->b;
    case TDOT_VAL_STR: return !strcmp(a->str, b->str);
    default: return true;
    }
}

static void run_reads(const cJSON *vectors) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(vectors, "read")) {
        const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
        tdot_map_t *map = NULL;
        char err[512];
        if (parse_map(named_map(vectors, v), &map, err, sizeof err) != 0 || !map) {
            CHECK(0, "%s: map did not parse: %s", name, err);
            continue;
        }
        tdot_datatype_t dt = tdot_datatype_parse(cJSON_GetObjectItem(v, "datatype")->valuestring);
        tdot_value_t in = vector_value(cJSON_GetObjectItem(v, "in"));
        const cJSON *tr = cJSON_GetObjectItem(v, "transform");
        if (tr) {
            tdot_transform_t t;
            tdot_transform_init(&t);
            const cJSON *m = cJSON_GetObjectItem(tr, "multiplier");
            if (m)
                t.multiplier = m->valuedouble;
            in.num = tdot_transform_apply(&t, in.num);
        }
        tdot_value_t out;
        int rc = tdot_map_apply(map, &in, dt, &out, err, sizeof err);
        const cJSON *want_err = cJSON_GetObjectItem(v, "error");
        if (want_err) {
            CHECK(rc != 0 && !strcmp(err, want_err->valuestring), "%s: got error '%s'", name,
                  rc ? err : "(none)");
        } else {
            tdot_value_t want = vector_value(cJSON_GetObjectItem(v, "out"));
            CHECK(rc == 0 && same_value(&out, &want), "%s: rc %d, got kind %d num %.17g str '%s'",
                  name, rc, out.kind, out.num, out.str);
        }
        tdot_map_free(map);
    }
}

/* ---- writes, through a connector that records what it got ---------------- */

static tdot_value_t g_written;
static int g_writes;

static int rec_write(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                     const tdot_value_t *value, char *err, size_t errlen) {
    (void)self, (void)dev, (void)pt, (void)err, (void)errlen;
    g_written = *value;
    g_writes++;
    return 0;
}

static tdot_config_t *load_text(const char *body, char *err, size_t errlen) {
    char tmpl[] = "/tmp/tdot-map-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        exit(2);
    }
    char path[256];
    snprintf(path, sizeof path, "%s/modbus.toml", dir);
    FILE *fp = fopen(path, "w");
    fputs(body, fp);
    fclose(fp);
    tdot_config_t *cfg = tdot_config_load(path, err, errlen);
    unlink(path);
    rmdir(dir);
    return cfg;
}

#define HEADER                                                                 \
    "[connector]\nprotocol = \"modbus\"\n\n[[device]]\nname = \"d\"\n"        \
    "protocol_address = { transport = \"tcp\", host = \"127.0.0.1\", port = 502, unit_id = 1 }\n"

static void run_writes(const cJSON *vectors) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(vectors, "write")) {
        const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
        if (cJSON_GetObjectItem(v, "raw"))
            continue; /* the C runtime has no raw (hex) writes */
        char text[8192];
        size_t o = (size_t)snprintf(text, sizeof text,
                                    HEADER "  [[device.point]]\n  id = \"p\"\n"
                                           "  datatype = \"%s\"\n  access = \"read_write\"\n"
                                           "  address = { table = \"holding\", address = 0 }\n"
                                           "  map = ",
                                    cJSON_GetObjectItem(v, "datatype")->valuestring);
        toml_of(named_map(vectors, v), text, sizeof text, &o);
        const cJSON *tr = cJSON_GetObjectItem(v, "transform");
        if (tr) {
            o += (size_t)snprintf(text + o, sizeof text - o, "\n  transform = ");
            toml_of(tr, text, sizeof text, &o);
        }
        snprintf(text + o, sizeof text - o, "\n");
        char err[512];
        tdot_config_t *cfg = load_text(text, err, sizeof err);
        if (!cfg) {
            CHECK(0, "%s: config did not load: %s", name, err);
            continue;
        }
        tdot_device_t *dev = &cfg->devices[0];
        tdot_point_t *pt = tdot_device_point(dev, "p");
        tdot_value_t in;
        memset(&in, 0, sizeof in);
        const cJSON *jv = cJSON_GetObjectItem(v, "value");
        if (cJSON_IsNumber(jv)) {
            in.kind = TDOT_VAL_NUM;
            in.num = jv->valuedouble;
        } else if (cJSON_IsBool(jv)) {
            in.kind = TDOT_VAL_BOOL;
            in.b = cJSON_IsTrue(jv);
        } else {
            in.kind = TDOT_VAL_STR;
            snprintf(in.str, sizeof in.str, "%s", jv->valuestring);
        }
        tdot_connector_t conn = {.protocol = "rec", .write_point = rec_write};
        memset(&g_written, 0, sizeof g_written);
        int before = g_writes;
        err[0] = '\0';
        int rc = tdot_connector_write(&conn, dev, pt, &in, err, sizeof err);
        const cJSON *want_err = cJSON_GetObjectItem(v, "error");
        if (want_err) {
            char want[512];
            snprintf(want, sizeof want, "point p: %s", want_err->valuestring);
            CHECK(rc != 0 && g_writes == before && !strcmp(err, want), "%s: got '%s'", name, err);
        } else {
            const cJSON *dv = cJSON_GetObjectItem(v, "device");
            tdot_value_t want;
            memset(&want, 0, sizeof want);
            if (cJSON_IsNumber(dv)) {
                want.kind = TDOT_VAL_NUM;
                want.num = dv->valuedouble;
            } else if (cJSON_IsBool(dv)) {
                want.kind = TDOT_VAL_BOOL;
                want.b = cJSON_IsTrue(dv);
            } else {
                want.kind = TDOT_VAL_STR;
                snprintf(want.str, sizeof want.str, "%s", dv->valuestring);
            }
            CHECK(rc == 0 && same_value(&g_written, &want),
                  "%s: rc %d err '%s', wrote kind %d num %.17g str '%s'", name, rc, err,
                  g_written.kind, g_written.num, g_written.str);
        }
        tdot_config_free(cfg);
    }
}

static void run_invalid(const cJSON *vectors) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(vectors, "invalid")) {
        const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
        const char *want = cJSON_GetObjectItem(v, "error")->valuestring;
        const cJSON *mode = cJSON_GetObjectItem(v, "mode");
        bool raw = cJSON_IsString(mode) && !strcmp(mode->valuestring, "raw");
        tdot_map_t *map = NULL;
        char err[512] = "";
        int rc = parse_map(cJSON_GetObjectItem(v, "map"), &map, err, sizeof err);
        if (rc == 0 && map)
            rc = tdot_map_check_point(map, raw, cJSON_GetObjectItem(v, "datatype")->valuestring,
                                      err, sizeof err);
        CHECK(rc == -1 && !strcmp(err, want), "%s: got rc %d '%s'", name, rc, err);
        tdot_map_free(map);
    }
}

/* ---- the loader and the envelope ------------------------------------------ */

static void check_loader(void) {
    char err[512];
    /* the resolved checks name the device and point */
    tdot_config_t *cfg = load_text(HEADER "  [[device.point]]\n  id = \"p\"\n"
                                          "  datatype = \"uint16\"\n  address = {}\n"
                                          "  map = { cases = [{ eq = \"a\", to = 1 }] }\n",
                                   err, sizeof err);
    CHECK(!cfg && strstr(err, "device 'd': point 'p': map.cases[0].eq \"a\" can never match a "
                              "uint16 value"),
          "resolved check: %s", err);
    tdot_config_free(cfg);
    cfg = load_text(HEADER "  [[device.point]]\n  id = \"p\"\n  datatype = \"uint16\"\n"
                           "  address = {}\n  map = { cases = [{ eqs = 0, to = \"off\" }] }\n",
                    err, sizeof err);
    CHECK(!cfg && strstr(err, "unknown key 'eqs' in map.cases[0] of point 'p' (did you mean 'eq'?)"),
          "case keys: %s", err);
    tdot_config_free(cfg);
    cfg = load_text(HEADER "  [[device.point]]\n  id = \"p\"\n  datatype = \"uint16\"\n"
                           "  address = {}\n  map = 5\n",
                    err, sizeof err);
    CHECK(!cfg && strstr(err, "point 'p': map must be a table"), "not a table: %s", err);
    tdot_config_free(cfg);

    /* a later definition replaces the map whole; `{}` removes it */
    cfg = load_text(HEADER "  [[device.point]]\n  id = \"p\"\n  datatype = \"uint16\"\n"
                           "  address = {}\n  map = { cases = [{ eq = 0, to = \"off\" }] }\n"
                           "  [[device.point]]\n  id = \"p\"\n  map = { as = \"bool\" }\n"
                           "  [[device.point]]\n  id = \"q\"\n  datatype = \"uint16\"\n"
                           "  address = {}\n  map = { as = \"string\" }\n"
                           "  [[device.point]]\n  id = \"q\"\n  map = {}\n",
                    err, sizeof err);
    CHECK(cfg != NULL, "replacement config: %s", err);
    if (!cfg)
        return;
    tdot_point_t *p = tdot_device_point(&cfg->devices[0], "p");
    CHECK(p->map && p->map->has_as && p->map->as == TDOT_MAP_BOOL && p->map->ncases == 0,
          "map replaced whole");
    CHECK(tdot_device_point(&cfg->devices[0], "q")->map == NULL, "map = {} removes the map");

    /* the envelope: mapped value, source_value; unmatched turns bad */
    tdot_config_free(cfg);
    cfg = load_text(HEADER "  [[device.point]]\n  id = \"state\"\n  datatype = \"uint16\"\n"
                           "  address = {}\n"
                           "  map = { cases = [{ eq = 0, to = \"stopped\" }, { eq = 1, to = \"running\" }] }\n",
                    err, sizeof err);
    CHECK(cfg != NULL, "envelope config: %s", err);
    if (!cfg)
        return;
    tdot_device_t *dev = &cfg->devices[0];
    tdot_point_t *pt = tdot_device_point(dev, "state");
    tdot_sample_t s;
    tdot_sample_init(&s);
    s.value.kind = TDOT_VAL_NUM;
    s.value.num = 1;
    tdot_sample_apply_map(pt, &s);
    char *json = tdot_envelope_sample(cfg, dev, pt, &s);
    cJSON *env = cJSON_Parse(json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "value")->valuestring, "running"), "%s", json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "value_repr")->valuestring, "string"), "%s", json);
    CHECK(cJSON_GetObjectItem(env, "source_value")->valuedouble == 1, "%s", json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "source_value_repr")->valuestring, "number"), "%s",
          json);
    cJSON_Delete(env);
    free(json);

    tdot_sample_init(&s);
    s.value.kind = TDOT_VAL_NUM;
    s.value.num = 3;
    tdot_sample_apply_map(pt, &s);
    json = tdot_envelope_sample(cfg, dev, pt, &s);
    env = cJSON_Parse(json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "quality")->valuestring, "bad"), "%s", json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "error")->valuestring,
                  "point state: no mapping for value 3"),
          "%s", json);
    CHECK(!cJSON_GetObjectItem(env, "value"), "%s", json);
    CHECK(cJSON_GetObjectItem(env, "source_value")->valuedouble == 3, "%s", json);
    cJSON_Delete(env);
    free(json);

    /* a failed read is untouched */
    tdot_sample_init(&s);
    tdot_sample_bad(&s, "timeout");
    tdot_sample_apply_map(pt, &s);
    json = tdot_envelope_sample(cfg, dev, pt, &s);
    env = cJSON_Parse(json);
    CHECK(!strcmp(cJSON_GetObjectItem(env, "error")->valuestring, "timeout"), "%s", json);
    CHECK(!cJSON_GetObjectItem(env, "source_value"), "%s", json);
    cJSON_Delete(env);
    free(json);
    tdot_config_free(cfg);
}

static void check_number_text(void) {
    char buf[400];
    const struct {
        double n;
        const char *text;
    } cases[] = {
        {0.1, "0.1"},          {-0.0, "0"},       {12, "12"},
        {1e21, "1000000000000000000000"},         {1.5e-7, "0.00000015"},
        {-2.5, "-2.5"},        {123456.789, "123456.789"},
        {5e-324, NULL},        {1.7976931348623157e308, NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(tdot_map_format_number(cases[i].n, buf, sizeof buf), "format %.17g", cases[i].n);
        if (cases[i].text)
            CHECK(!strcmp(buf, cases[i].text), "format %.17g: '%s'", cases[i].n, buf);
        double back = 0;
        CHECK(tdot_map_parse_number(buf, &back) && back == cases[i].n,
              "%.17g does not read back from '%s'", cases[i].n, buf);
    }
    /* every float near powers of two and ten reads back, with no exponent */
    for (int e = -60; e <= 60; e++) {
        double bases[] = {ldexp(1.0, e), pow(10.0, e / 3), 0.1 * e, 1.0 / (e ? e : 7)};
        for (size_t i = 0; i < 4; i++) {
            double n[3] = {bases[i], nextafter(bases[i], INFINITY), nextafter(bases[i], -INFINITY)};
            for (size_t k = 0; k < 3; k++) {
                double back = 0;
                CHECK(tdot_map_format_number(n[k], buf, sizeof buf) && !strchr(buf, 'e') &&
                          tdot_map_parse_number(buf, &back) && back == n[k],
                      "%.17g -> '%s'", n[k], buf);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vectors.json>\n", argv[0]);
        return 2;
    }
    char *text = slurp_tagged(argv[1]);
    cJSON *vectors = text ? cJSON_Parse(text) : NULL;
    if (!vectors) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    run_reads(vectors);
    run_writes(vectors);
    run_invalid(vectors);
    check_loader();
    check_number_text();
    cJSON_Delete(vectors);
    free(text);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("map: all vectors pass\n");
    return 0;
}
