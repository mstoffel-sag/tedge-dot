/* AVEVA PI: the decoder over the shared golden vectors
 * (connectors/aveva-pi/conformance/decode-vectors.json), which the Rust crate's value.rs runs
 * too, plus the canonical number text, 64-bit integers and the write encoding.
 *
 * Usage: tedge-dot-aveva-pi-vectors <decode-vectors.json>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pi_value.h"

static int failures;

#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    if (buf) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

static void check_flags(const char *name, unsigned flags, const cJSON *expected) {
    unsigned want = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, pi_array(expected)) {
        if (strcmp(f->valuestring, "questionable") == 0)
            want |= PI_FLAG_QUESTIONABLE;
        else if (strcmp(f->valuestring, "substituted") == 0)
            want |= PI_FLAG_SUBSTITUTED;
        else if (strcmp(f->valuestring, "annotated") == 0)
            want |= PI_FLAG_ANNOTATED;
    }
    CHECK(flags == want, "%s: flags %#x, want %#x", name, flags, want);
}

static void check_value(const char *name, const pi_decoded_t *d, const cJSON *want) {
    CHECK(d->has_value, "%s: no value", name);
    if (!d->has_value)
        return;
    if (cJSON_IsBool(want))
        CHECK(d->value.kind == TDOT_VAL_BOOL && d->value.b == cJSON_IsTrue(want),
              "%s: value is not %s", name, cJSON_IsTrue(want) ? "true" : "false");
    else if (cJSON_IsNumber(want))
        CHECK(d->value.kind == TDOT_VAL_NUM && d->value.num == want->valuedouble,
              "%s: value %.17g, want %.17g", name, d->value.num, want->valuedouble);
    else if (cJSON_IsString(want))
        CHECK(d->value.kind == TDOT_VAL_STR && strcmp(d->value.str, want->valuestring) == 0,
              "%s: value '%s', want '%s'", name, d->value.str, want->valuestring);
}

static int run_vectors(const char *path) {
    size_t len = 0;
    char *text = slurp(path, &len);
    if (!text) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    cJSON *doc = pi_json_parse(text, len);
    free(text);
    const cJSON *vectors = cJSON_GetObjectItemCaseSensitive(doc, "vectors");
    int count = cJSON_GetArraySize(vectors);
    CHECK(count > 40, "only %d vectors", count);
    const cJSON *v;
    cJSON_ArrayForEach(v, pi_array(vectors)) {
        const char *name = cJSON_GetObjectItemCaseSensitive(v, "name")->valuestring;
        const char *dt_name = cJSON_GetObjectItemCaseSensitive(v, "datatype")->valuestring;
        bool raw = strcmp(cJSON_GetObjectItemCaseSensitive(v, "mode")->valuestring, "raw") == 0;
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(v, "expect");
        pi_decoded_t d;
        pi_decode(cJSON_GetObjectItemCaseSensitive(v, "value"), tdot_datatype_parse(dt_name),
                  raw ? TDOT_MODE_RAW : TDOT_MODE_TYPED, &d);
        const char *quality = cJSON_GetObjectItemCaseSensitive(e, "quality")->valuestring;
        CHECK(strcmp(quality, d.quality == TDOT_Q_GOOD ? "good" : "bad") == 0,
              "%s: quality %s, want %s (error '%s')", name,
              d.quality == TDOT_Q_GOOD ? "good" : "bad", quality, d.error);
        const cJSON *want = cJSON_GetObjectItemCaseSensitive(e, "value");
        if (want)
            check_value(name, &d, want);
        else
            CHECK(!d.has_value || raw, "%s: unexpected value", name);
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(e, "error");
        if (cJSON_IsString(err))
            CHECK(strcmp(d.error, err->valuestring) == 0, "%s: error '%s', want '%s'", name,
                  d.error, err->valuestring);
        else
            CHECK(d.error[0] == '\0', "%s: unexpected error '%s'", name, d.error);
        const cJSON *rawv = cJSON_GetObjectItemCaseSensitive(e, "raw");
        if (cJSON_IsString(rawv))
            CHECK(d.raw && strcmp(d.raw, rawv->valuestring) == 0, "%s: raw '%s', want '%s'", name,
                  d.raw ? d.raw : "(null)", rawv->valuestring);
        check_flags(name, d.flags, cJSON_GetObjectItemCaseSensitive(e, "flags"));
        pi_decoded_free(&d);
    }
    cJSON_Delete(doc);
    printf("aveva-pi-vectors: %d vectors\n", count);
    return 0;
}

/* value.rs float_repr_matches_python */
static void float_text(void) {
    static const struct {
        double f;
        const char *text;
    } cases[] = {
        {0.1, "0.1"},
        {2.5, "2.5"},
        {-0.5, "-0.5"},
        {1e300, "1e+300"},
        {1.5e-5, "1.5e-05"},
        {0.0001, "0.0001"},
        {1e16, "1e+16"},
        {1.2345e16, "1.2345e+16"},
        {9007199254740992.0, "9007199254740992.0"},
        {123456.789, "123456.789"},
        {0.10000000149011612, "0.10000000149011612"},
        {7.0, "7"},
        {-0.0, "0"},
        {-3.0, "-3"},
        {5e-324, "5e-324"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {123.0e-7, "1.23e-05"},
        {0.00012, "0.00012"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char text[64];
        pi_float_text(cases[i].f, text, sizeof text);
        CHECK(strcmp(text, cases[i].text) == 0, "float %.17g: '%s', want '%s'", cases[i].f,
              text, cases[i].text);
    }
}

static cJSON *parse(const char *text) { return pi_json_parse(text, strlen(text)); }

static void canonical(void) {
    static const struct {
        const char *in, *out;
    } cases[] = {
        {"{\"b\":1,\"a\":[true,null,\"x\"]}", "{\"a\":[true,null,\"x\"],\"b\":1}"},
        {"7.0", "7"},
        {"9007199254740993", "9007199254740993"},
        {"-9223372036854775808", "-9223372036854775808"},
        {"18446744073709551615", "18446744073709551615"},
        {"100000000000000000000", "1e+20"},
        {"\"a\\u00e9\\t\\u0001/\\ud83d\\ude00\"", "\"a\xc3\xa9\\t\\u0001/\xf0\x9f\x98\x80\""},
        {" { \"k\" : 1 , \"k\" : 2 } ", "{\"k\":2}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cJSON *v = parse(cases[i].in);
        char *text = v ? pi_canonical(v) : NULL;
        CHECK(text && strcmp(text, cases[i].out) == 0, "canonical(%s) = %s, want %s",
              cases[i].in, text ? text : "(parse error)", cases[i].out);
        free(text);
        cJSON_Delete(v);
    }
    static const char *bad[] = {"", "{", "[1,]", "01", "1.", "\"\\x\"", "tru", "1 2",
                                "\"\\udc00\"", "1e999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cJSON *v = parse(bad[i]);
        CHECK(!v, "'%s' parsed", bad[i]);
        cJSON_Delete(v);
    }
}

static void big_integers(void) {
    char err[TDOT_ERR_MAX] = "";
    tdot_value_t out;
    cJSON *n = parse("9007199254740993");
    CHECK(pi_to_datatype(n, TDOT_DT_INT64, &out, err, sizeof err) == 0 &&
              out.kind == TDOT_VAL_STR && strcmp(out.str, "9007199254740993") == 0,
          "int64 beyond 2^53 is carried as text (%s)", err);
    CHECK(pi_to_datatype(n, TDOT_DT_INT32, &out, err, sizeof err) != 0 &&
              strcmp(err, "9007199254740993 does not fit int32") == 0,
          "int32: %s", err);
    cJSON_Delete(n);
    n = parse("9223372036854775808");
    CHECK(pi_to_datatype(n, TDOT_DT_INT64, &out, err, sizeof err) != 0,
          "2^63 does not fit int64");
    CHECK(pi_to_datatype(n, TDOT_DT_UINT64, &out, err, sizeof err) == 0 &&
              strcmp(out.str, "9223372036854775808") == 0,
          "2^63 fits uint64");
    cJSON_Delete(n);
    n = parse("1e16");
    CHECK(pi_to_datatype(n, TDOT_DT_INT64, &out, err, sizeof err) == 0 &&
              out.kind == TDOT_VAL_STR && strcmp(out.str, "10000000000000000") == 0,
          "1e16 as int64: %s", out.str);
    cJSON_Delete(n);
    n = parse("-0.0");
    CHECK(pi_to_datatype(n, TDOT_DT_INT16, &out, err, sizeof err) == 0 &&
              out.kind == TDOT_VAL_NUM && out.num == 0 && !signbit(out.num),
          "-0.0 as int16");
    cJSON_Delete(n);
    n = parse("3.4028235677973366e38");
    CHECK(pi_to_datatype(n, TDOT_DT_FLOAT32, &out, err, sizeof err) != 0,
          "the float32 rounding midpoint overflows");
    cJSON_Delete(n);
    n = parse("3.4028235677973362e38");
    CHECK(pi_to_datatype(n, TDOT_DT_FLOAT32, &out, err, sizeof err) == 0,
          "just below the midpoint rounds to FLT_MAX");
    cJSON_Delete(n);
}

static char *encoded(tdot_value_t v, tdot_datatype_t dt, char *err, size_t errlen) {
    cJSON *j = pi_encode_write(&v, dt, err, errlen);
    char *text = j ? pi_canonical(j) : NULL;
    cJSON_Delete(j);
    return text;
}

/* value.rs writes_are_checked_like_reads */
static void writes(void) {
    char err[TDOT_ERR_MAX];
    tdot_value_t num = {.kind = TDOT_VAL_NUM, .num = 42.5};
    char *t = encoded(num, TDOT_DT_FLOAT64, err, sizeof err);
    CHECK(t && strcmp(t, "42.5") == 0, "42.5 as float64: %s", t ? t : err);
    free(t);
    t = encoded(num, TDOT_DT_INT32, err, sizeof err);
    CHECK(!t && strcmp(err, "42.5 does not fit int32") == 0, "42.5 as int32: %s", err);
    free(t);
    num.num = 70000;
    t = encoded(num, TDOT_DT_INT16, err, sizeof err);
    CHECK(!t, "70000 as int16");
    free(t);
    tdot_value_t str = {.kind = TDOT_VAL_STR};
    snprintf(str.str, sizeof str.str, "12");
    t = encoded(str, TDOT_DT_INT16, err, sizeof err);
    CHECK(t && strcmp(t, "12") == 0, "\"12\" as int16: %s", t ? t : err);
    free(t);
    snprintf(str.str, sizeof str.str, "abc");
    t = encoded(str, TDOT_DT_INT16, err, sizeof err);
    CHECK(!t && strcmp(err, "expected a number for int16, got \"abc\"") == 0, "abc: %s", err);
    free(t);
    num.num = 1;
    t = encoded(num, TDOT_DT_BOOL, err, sizeof err);
    CHECK(t && strcmp(t, "true") == 0, "1 as bool: %s", t ? t : err);
    free(t);
    num.num = 5;
    t = encoded(num, TDOT_DT_STRING, err, sizeof err);
    CHECK(!t && strcmp(err, "expected a string for string, got 5") == 0, "5 as string: %s",
          err);
    free(t);
    snprintf(str.str, sizeof str.str, "18446744073709551615");
    t = encoded(str, TDOT_DT_UINT64, err, sizeof err);
    CHECK(t && strcmp(t, "18446744073709551615") == 0, "uint64 max: %s", t ? t : err);
    free(t);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <decode-vectors.json>\n", argv[0]);
        return 2;
    }
    if (run_vectors(argv[1]) != 0)
        return 1;
    float_text();
    canonical();
    big_integers();
    writes();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
