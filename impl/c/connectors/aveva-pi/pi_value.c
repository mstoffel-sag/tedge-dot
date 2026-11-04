/* AVEVA PI connector -- decoding a PI value (spec §4). See pi_value.h. */
#include "pi_value.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Integers up to this magnitude are carried as JSON numbers; larger ones as text (contract
 * §4.1), and written exactly in canonical text. */
#define PI_SAFE 9007199254740992.0 /* 2^53 */
/* serde_json's recursion limit. */
#define PI_JSON_DEPTH 128

/* ---- growable string ---------------------------------------------------------------------- */

typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
} pi_sb_t;

static void sb_put(pi_sb_t *b, const char *s, size_t n) {
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) {
            b->oom = true;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_puts(pi_sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }

static char *sb_take(pi_sb_t *b) {
    if (b->oom) {
        free(b->s);
        return NULL;
    }
    if (!b->s)
        return strdup("");
    return b->s;
}

/* ---- JSON parsing --------------------------------------------------------------------------- */

typedef struct {
    const char *p, *end;
    int depth;
} pi_jp_t;

static void jp_ws(pi_jp_t *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static bool jp_lit(pi_jp_t *j, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0)
        return false;
    j->p += n;
    return true;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static void put_utf8(pi_sb_t *b, unsigned cp) {
    char u[4];
    size_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_put(b, u, n);
}

/* A string after its opening quote; NULL on a syntax error. Caller frees. */
static char *jp_string(pi_jp_t *j) {
    pi_sb_t b = {0};
    sb_put(&b, "", 0);
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"')
            return sb_take(&b);
        if (c < 0x20)
            break; /* a raw control character */
        if (c != '\\') {
            sb_put(&b, (const char *)&c, 1);
            continue;
        }
        if (j->p >= j->end)
            break;
        char e = *j->p++;
        const char *s = NULL;
        switch (e) {
        case '"': s = "\""; break;
        case '\\': s = "\\"; break;
        case '/': s = "/"; break;
        case 'b': s = "\b"; break;
        case 'f': s = "\f"; break;
        case 'n': s = "\n"; break;
        case 'r': s = "\r"; break;
        case 't': s = "\t"; break;
        case 'u': {
            if (j->end - j->p < 4)
                goto fail;
            int cp = hex4(j->p);
            if (cp < 0)
                goto fail;
            j->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u')
                    goto fail;
                int lo = hex4(j->p + 2);
                if (lo < 0xDC00 || lo > 0xDFFF)
                    goto fail;
                j->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                goto fail; /* a lone low surrogate */
            }
            put_utf8(&b, (unsigned)cp);
            continue;
        }
        default:
            goto fail;
        }
        sb_puts(&b, s);
    }
fail:
    free(b.s);
    return NULL;
}

static cJSON *jp_number(pi_jp_t *j) {
    const char *start = j->p;
    bool integer = true;
    if (j->p < j->end && *j->p == '-')
        j->p++;
    if (j->p >= j->end)
        return NULL;
    if (*j->p == '0')
        j->p++;
    else if (*j->p >= '1' && *j->p <= '9')
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
    else
        return NULL;
    if (j->p < j->end && *j->p == '.') {
        integer = false;
        j->p++;
        const char *d = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
        if (j->p == d)
            return NULL;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        integer = false;
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
            j->p++;
        const char *d = j->p;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
        if (j->p == d)
            return NULL;
    }
    size_t n = (size_t)(j->p - start);
    char text[512];
    if (n >= sizeof text)
        return NULL;
    memcpy(text, start, n);
    text[n] = '\0';
    if (integer) {
        /* Exact when it fits 64 bits, as serde_json's i64/u64. */
        char exact[32] = "";
        double d = 0;
        bool fits = false;
        errno = 0;
        if (text[0] == '-') {
            long long v = strtoll(text, NULL, 10);
            if (errno != ERANGE) {
                fits = true;
                d = (double)v;
                if (fabs(d) >= PI_SAFE)
                    snprintf(exact, sizeof exact, "%lld", v);
            }
        } else {
            unsigned long long v = strtoull(text, NULL, 10);
            if (errno != ERANGE) {
                fits = true;
                d = (double)v;
                if (d >= PI_SAFE)
                    snprintf(exact, sizeof exact, "%llu", v);
            }
        }
        if (fits) {
            cJSON *item = cJSON_CreateNumber(d);
            if (item && exact[0]) {
                item->valuestring = strdup(exact);
                if (!item->valuestring) {
                    cJSON_Delete(item);
                    return NULL;
                }
            }
            return item;
        }
    }
    double d = strtod(text, NULL);
    if (!isfinite(d))
        return NULL; /* serde_json: "number out of range" */
    return cJSON_CreateNumber(d);
}

static cJSON *jp_value(pi_jp_t *j);

static cJSON *jp_array(pi_jp_t *j) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    jp_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return arr;
    }
    for (;;) {
        cJSON *item = jp_value(j);
        if (!item)
            break;
        cJSON_AddItemToArray(arr, item);
        jp_ws(j);
        if (j->p >= j->end)
            break;
        char c = *j->p++;
        if (c == ']')
            return arr;
        if (c != ',')
            break;
    }
    cJSON_Delete(arr);
    return NULL;
}

static cJSON *jp_object(pi_jp_t *j) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;
    jp_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return obj;
    }
    for (;;) {
        jp_ws(j);
        if (j->p >= j->end || *j->p != '"')
            break;
        j->p++;
        char *key = jp_string(j);
        if (!key)
            break;
        jp_ws(j);
        if (j->p >= j->end || *j->p != ':') {
            free(key);
            break;
        }
        j->p++;
        cJSON *item = jp_value(j);
        if (!item) {
            free(key);
            break;
        }
        /* A repeated key keeps its last value, as serde_json's map does. */
        if (cJSON_GetObjectItemCaseSensitive(obj, key))
            cJSON_ReplaceItemInObjectCaseSensitive(obj, key, item);
        else
            cJSON_AddItemToObject(obj, key, item);
        free(key);
        jp_ws(j);
        if (j->p >= j->end)
            break;
        char c = *j->p++;
        if (c == '}')
            return obj;
        if (c != ',')
            break;
    }
    cJSON_Delete(obj);
    return NULL;
}

static cJSON *jp_value(pi_jp_t *j) {
    jp_ws(j);
    if (j->p >= j->end || ++j->depth > PI_JSON_DEPTH)
        return NULL;
    cJSON *out = NULL;
    char c = *j->p;
    if (c == '{') {
        j->p++;
        out = jp_object(j);
    } else if (c == '[') {
        j->p++;
        out = jp_array(j);
    } else if (c == '"') {
        j->p++;
        char *s = jp_string(j);
        if (s) {
            out = cJSON_CreateString(s);
            free(s);
        }
    } else if (jp_lit(j, "true")) {
        out = cJSON_CreateTrue();
    } else if (jp_lit(j, "false")) {
        out = cJSON_CreateFalse();
    } else if (jp_lit(j, "null")) {
        out = cJSON_CreateNull();
    } else {
        out = jp_number(j);
    }
    j->depth--;
    return out;
}

cJSON *pi_json_parse(const char *text, size_t len) {
    if (!text)
        return NULL;
    pi_jp_t j = {.p = text, .end = text + len};
    cJSON *v = jp_value(&j);
    if (!v)
        return NULL;
    jp_ws(&j);
    if (j.p != j.end) {
        cJSON_Delete(v);
        return NULL;
    }
    return v;
}

/* ---- canonical text (spec §4, "Canonical text") ----------------------------------------- */

/* A float in its shortest round-trip digits, as Python's repr writes it: fixed notation with
 * at least one fractional digit when the decimal exponent is in [-4, 16), else exponent form
 * with a sign and at least two exponent digits (`1e+300`, `5e-05`). */
static void float_repr(double f, char *dst, size_t cap) {
    if (!isfinite(f)) {
        snprintf(dst, cap, "%s", isnan(f) ? "NaN" : (f < 0 ? "-inf" : "inf"));
        return;
    }
    double a = fabs(f);
    char sci[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(sci, sizeof sci, "%.*e", p - 1, a);
        if (strtod(sci, NULL) == a)
            break;
    }
    /* sci: d[.ddd]e[+-]xx */
    char digits[24];
    size_t nd = 0;
    const char *e = strchr(sci, 'e');
    for (const char *q = sci; q < e && nd < sizeof digits - 1; q++)
        if (*q != '.')
            digits[nd++] = *q;
    digits[nd] = '\0';
    int exp = atoi(e + 1);
    const char *sign = signbit(f) ? "-" : "";
    char body[64];
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            size_t point = (size_t)exp + 1;
            if (nd <= point) {
                char zeros[24];
                memset(zeros, '0', point - nd);
                zeros[point - nd] = '\0';
                snprintf(body, sizeof body, "%s%s.0", digits, zeros);
            } else {
                snprintf(body, sizeof body, "%.*s.%s", (int)point, digits, digits + point);
            }
        } else {
            char zeros[8];
            memset(zeros, '0', (size_t)(-exp - 1));
            zeros[-exp - 1] = '\0';
            snprintf(body, sizeof body, "0.%s%s", zeros, digits);
        }
        snprintf(dst, cap, "%s%s", sign, body);
    } else {
        char m[32];
        if (nd == 1)
            snprintf(m, sizeof m, "%s", digits);
        else
            snprintf(m, sizeof m, "%c.%s", digits[0], digits + 1);
        snprintf(dst, cap, "%s%se%c%02d", sign, m, exp < 0 ? '-' : '+', abs(exp));
    }
}

void pi_float_text(double f, char *dst, size_t cap) {
    if (isfinite(f) && f == trunc(f) && fabs(f) < PI_SAFE)
        snprintf(dst, cap, "%lld", (long long)f);
    else
        float_repr(f, dst, cap);
}

void pi_number_text(const cJSON *n, char *dst, size_t cap) {
    if (n->valuestring)
        snprintf(dst, cap, "%s", n->valuestring);
    else
        pi_float_text(n->valuedouble, dst, cap);
}

/* A string as serde_json writes it. */
static void put_string(pi_sb_t *b, const char *s) {
    sb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"': sb_put(b, "\\\"", 2); break;
        case '\\': sb_put(b, "\\\\", 2); break;
        case '\b': sb_put(b, "\\b", 2); break;
        case '\f': sb_put(b, "\\f", 2); break;
        case '\n': sb_put(b, "\\n", 2); break;
        case '\r': sb_put(b, "\\r", 2); break;
        case '\t': sb_put(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                sb_puts(b, esc);
            } else {
                sb_put(b, (const char *)p, 1);
            }
        }
    }
    sb_put(b, "\"", 1);
}

static int key_cmp(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    return strcmp(x->string, y->string);
}

static void put_canonical(pi_sb_t *b, const cJSON *v) {
    if (!v || cJSON_IsNull(v)) {
        sb_puts(b, "null");
    } else if (cJSON_IsTrue(v)) {
        sb_puts(b, "true");
    } else if (cJSON_IsFalse(v)) {
        sb_puts(b, "false");
    } else if (cJSON_IsNumber(v)) {
        char num[64];
        pi_number_text(v, num, sizeof num);
        sb_puts(b, num);
    } else if (cJSON_IsString(v)) {
        put_string(b, v->valuestring ? v->valuestring : "");
    } else if (cJSON_IsArray(v)) {
        sb_put(b, "[", 1);
        int i = 0;
        for (const cJSON *c = v->child; c; c = c->next, i++) {
            if (i)
                sb_put(b, ",", 1);
            put_canonical(b, c);
        }
        sb_put(b, "]", 1);
    } else if (cJSON_IsObject(v)) {
        size_t n = 0;
        for (const cJSON *c = v->child; c; c = c->next)
            n++;
        const cJSON **keys = n ? malloc(n * sizeof *keys) : NULL;
        if (n && !keys) {
            b->oom = true;
            return;
        }
        n = 0;
        for (const cJSON *c = v->child; c; c = c->next)
            keys[n++] = c;
        qsort(keys, n, sizeof *keys, key_cmp);
        sb_put(b, "{", 1);
        for (size_t i = 0; i < n; i++) {
            if (i)
                sb_put(b, ",", 1);
            put_string(b, keys[i]->string);
            sb_put(b, ":", 1);
            put_canonical(b, keys[i]);
        }
        sb_put(b, "}", 1);
        free(keys);
    } else if (cJSON_IsRaw(v)) {
        sb_puts(b, v->valuestring ? v->valuestring : "null");
    } else {
        sb_puts(b, "null");
    }
}

char *pi_canonical(const cJSON *v) {
    pi_sb_t b = {0};
    put_canonical(&b, v);
    return sb_take(&b);
}

/* ---- decoding ------------------------------------------------------------------------------ */

static const cJSON *get(const cJSON *obj, const char *key) {
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

bool pi_flag(const cJSON *pi, const char *key) {
    const cJSON *f = get(pi, key);
    if (cJSON_IsBool(f))
        return cJSON_IsTrue(f);
    if (cJSON_IsString(f))
        return strcasecmp(f->valuestring, "true") == 0;
    return false;
}

static void join(pi_sb_t *b, const char *msg) {
    if (b->len)
        sb_puts(b, "; ");
    sb_puts(b, msg);
}

/* A non-string JSON message, as its text. */
static void join_json(pi_sb_t *b, const cJSON *v) {
    char *text = pi_canonical(v);
    join(b, text ? text : "");
    free(text);
}

bool pi_errors_of(const cJSON *v, char *dst, size_t cap) {
    const cJSON *list = get(v, "Errors");
    if (!cJSON_IsArray(list))
        return false;
    pi_sb_t b = {0};
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        if (cJSON_IsString(e)) {
            join(&b, e->valuestring);
        } else if (cJSON_IsObject(e)) {
            const cJSON *m = get(e, "Message");
            if (cJSON_IsArray(m)) {
                const cJSON *x;
                cJSON_ArrayForEach(x, m) {
                    if (cJSON_IsString(x))
                        join(&b, x->valuestring);
                    else
                        join_json(&b, x);
                }
            } else if (cJSON_IsString(m)) {
                join(&b, m->valuestring);
            } else if (m) {
                join_json(&b, m);
            }
        }
    }
    snprintf(dst, cap, "%s", b.len && !b.oom ? b.s : "error");
    free(b.s);
    return true;
}

bool pi_is_digital(const cJSON *v) {
    return cJSON_IsObject(v) && get(v, "Name") && get(v, "Value") && !get(v, "Errors");
}

static void set_text(tdot_value_t *out, const char *s) {
    memset(out, 0, sizeof *out);
    out->kind = TDOT_VAL_STR;
    snprintf(out->str, sizeof out->str, "%s", s);
}

static void set_num(tdot_value_t *out, double n) {
    memset(out, 0, sizeof *out);
    out->kind = TDOT_VAL_NUM;
    out->num = n;
}

static void set_bool(tdot_value_t *out, bool b) {
    memset(out, 0, sizeof *out);
    out->kind = TDOT_VAL_BOOL;
    out->b = b;
}

/* A number item converted to an integer datatype: 0 when it is integral and in range, else
 * -1. Beyond 2^53 the value is carried as its decimal text (model.h). */
static int to_integer(const cJSON *n, tdot_datatype_t dt, tdot_value_t *out) {
    double f = n->valuedouble;
    if (n->valuestring) {
        /* An exact literal beyond 2^53: compare the integer itself. */
        bool neg = n->valuestring[0] == '-';
        if (dt == TDOT_DT_INT64 || (dt == TDOT_DT_UINT64 && !neg)) {
            if (dt == TDOT_DT_INT64 && !neg) {
                errno = 0;
                unsigned long long u = strtoull(n->valuestring, NULL, 10);
                if (errno == ERANGE || u > (unsigned long long)INT64_MAX)
                    return -1;
            }
            set_text(out, n->valuestring);
            return 0;
        }
        return -1;
    }
    if (!isfinite(f) || f != trunc(f) || fabs(f) >= 1e39)
        return -1;
    double lo, hi; /* inclusive, except the 64-bit upper bounds (exclusive) */
    switch (dt) {
    case TDOT_DT_INT8: lo = INT8_MIN; hi = INT8_MAX; break;
    case TDOT_DT_UINT8: lo = 0; hi = UINT8_MAX; break;
    case TDOT_DT_INT16: lo = INT16_MIN; hi = INT16_MAX; break;
    case TDOT_DT_UINT16: lo = 0; hi = UINT16_MAX; break;
    case TDOT_DT_INT32: lo = INT32_MIN; hi = INT32_MAX; break;
    case TDOT_DT_UINT32: lo = 0; hi = UINT32_MAX; break;
    case TDOT_DT_INT64:
        if (f < -9223372036854775808.0 || f >= 9223372036854775808.0)
            return -1;
        lo = f;
        hi = f;
        break;
    case TDOT_DT_UINT64:
        if (f < 0 || f >= 18446744073709551616.0)
            return -1;
        lo = f;
        hi = f;
        break;
    default:
        return -1;
    }
    if (f < lo || f > hi)
        return -1;
    if (fabs(f) < PI_SAFE) {
        set_num(out, (double)(long long)f); /* also turns -0 into 0 */
    } else {
        char text[48];
        snprintf(text, sizeof text, "%.0f", f);
        set_text(out, text);
    }
    return 0;
}

int pi_to_datatype(const cJSON *v, tdot_datatype_t dt, tdot_value_t *out, char *err,
                   size_t errlen) {
    if (pi_is_digital(v)) {
        if (dt == TDOT_DT_STRING) {
            const cJSON *name = get(v, "Name");
            if (!cJSON_IsString(name)) {
                snprintf(err, errlen, "unsupported PI value");
                return -1;
            }
            set_text(out, name->valuestring);
            return 0;
        }
        v = get(v, "Value");
    }
    const char *name = tdot_datatype_str(dt);
    if (!name)
        name = "none";
    if (cJSON_IsBool(v)) {
        if (dt == TDOT_DT_BOOL) {
            set_bool(out, cJSON_IsTrue(v));
            return 0;
        }
        if (dt == TDOT_DT_STRING) {
            set_text(out, cJSON_IsTrue(v) ? "true" : "false");
            return 0;
        }
        snprintf(err, errlen, "expected %s, got a boolean", name);
        return -1;
    }
    if (cJSON_IsNumber(v)) {
        char text[64];
        pi_number_text(v, text, sizeof text);
        double f = v->valuedouble;
        switch (dt) {
        case TDOT_DT_STRING:
            set_text(out, text);
            return 0;
        case TDOT_DT_BOOL:
            if (f == 0.0 || f == 1.0) {
                set_bool(out, f == 1.0);
                return 0;
            }
            break;
        case TDOT_DT_FLOAT64:
            set_num(out, f);
            return 0;
        case TDOT_DT_FLOAT32:
            /* Below the midpoint between FLT_MAX and 2^128 the conversion rounds to a
             * finite float; at or above it, to infinity (Rust's `as f32`). */
            if (fabs(f) < 0x1.ffffffp+127) {
                set_num(out, (double)(float)f);
                return 0;
            }
            break;
        case TDOT_DT_INT8:
        case TDOT_DT_UINT8:
        case TDOT_DT_INT16:
        case TDOT_DT_UINT16:
        case TDOT_DT_INT32:
        case TDOT_DT_UINT32:
        case TDOT_DT_INT64:
        case TDOT_DT_UINT64:
            if (to_integer(v, dt, out) == 0)
                return 0;
            break;
        default:
            snprintf(err, errlen, "unsupported datatype %s", name);
            return -1;
        }
        snprintf(err, errlen, "%s does not fit %s", text, name);
        return -1;
    }
    if (cJSON_IsString(v)) {
        if (dt == TDOT_DT_STRING) {
            set_text(out, v->valuestring);
            return 0;
        }
        snprintf(err, errlen, "expected %s, got a string", name);
        return -1;
    }
    snprintf(err, errlen, "unsupported PI value");
    return -1;
}

void pi_decode(const cJSON *pi, tdot_datatype_t dt, tdot_mode_t mode, pi_decoded_t *out) {
    memset(out, 0, sizeof *out);
    const cJSON *v = get(pi, "Value");
    bool has_error = pi_errors_of(v, out->error, sizeof out->error) ||
                     pi_errors_of(pi, out->error, sizeof out->error);
    if (!has_error && pi_is_digital(v) && cJSON_IsTrue(get(v, "IsSystem"))) {
        const cJSON *name = get(v, "Name");
        snprintf(out->error, sizeof out->error, "%s",
                 cJSON_IsString(name) ? name->valuestring : "not good");
        has_error = true;
    }
    if (!has_error && get(pi, "Good") && !pi_flag(pi, "Good")) {
        const cJSON *name = get(v, "Name");
        snprintf(out->error, sizeof out->error, "%s",
                 cJSON_IsString(name) && pi_is_digital(v) ? name->valuestring : "not good");
        has_error = true;
    }
    if (!has_error && (!v || cJSON_IsNull(v))) {
        snprintf(out->error, sizeof out->error, "unsupported PI value");
        has_error = true;
    }
    if (!has_error && mode == TDOT_MODE_TYPED) {
        if (dt == TDOT_DT_NONE) {
            snprintf(out->error, sizeof out->error, "no datatype");
            has_error = true;
        } else if (pi_to_datatype(v, dt, &out->value, out->error, sizeof out->error) == 0) {
            out->has_value = true;
        } else {
            has_error = true;
        }
    }
    if (pi_flag(pi, "Questionable"))
        out->flags |= PI_FLAG_QUESTIONABLE;
    if (pi_flag(pi, "Substituted"))
        out->flags |= PI_FLAG_SUBSTITUTED;
    if (pi_flag(pi, "Annotated"))
        out->flags |= PI_FLAG_ANNOTATED;
    out->quality = has_error ? TDOT_Q_BAD : TDOT_Q_GOOD;
    if (!has_error)
        out->error[0] = '\0';
    out->raw = pi_canonical(v);
    const cJSON *ts = get(pi, "Timestamp");
    out->timestamp = cJSON_IsString(ts) ? ts->valuestring : NULL;
    const cJSON *units = get(pi, "UnitsAbbreviation");
    out->units = cJSON_IsString(units) && units->valuestring[0] ? units->valuestring : NULL;
}

void pi_decoded_free(pi_decoded_t *d) {
    free(d->raw);
    d->raw = NULL;
}

/* ---- writes -------------------------------------------------------------------------------- */

cJSON *pi_value_json(const tdot_value_t *value) {
    switch (value->kind) {
    case TDOT_VAL_BOOL:
        return cJSON_CreateBool(value->b);
    case TDOT_VAL_NUM:
        return cJSON_CreateNumber(value->num);
    case TDOT_VAL_STR:
        return cJSON_CreateString(value->str);
    default:
        return cJSON_CreateNull();
    }
}

cJSON *pi_encode_write(const tdot_value_t *value, tdot_datatype_t dt, char *err,
                       size_t errlen) {
    cJSON *v = pi_value_json(value);
    if (!v) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (dt == TDOT_DT_NONE)
        return v;
    const char *name = tdot_datatype_str(dt);
    char *shown = pi_canonical(v);
    cJSON *out = NULL;
    if (dt == TDOT_DT_STRING) {
        if (cJSON_IsString(v)) {
            out = v;
            v = NULL;
        } else {
            snprintf(err, errlen, "expected a string for %s, got %s", name, shown ? shown : "?");
        }
    } else if (dt == TDOT_DT_BOOL) {
        if (cJSON_IsBool(v)) {
            out = v;
            v = NULL;
        } else if (cJSON_IsNumber(v) && (v->valuedouble == 0.0 || v->valuedouble == 1.0)) {
            out = cJSON_CreateBool(v->valuedouble == 1.0);
        } else {
            snprintf(err, errlen, "expected a boolean for %s, got %s", name,
                     shown ? shown : "?");
        }
    } else {
        cJSON *n = NULL;
        if (cJSON_IsNumber(v)) {
            n = v;
            v = NULL;
        } else if (cJSON_IsString(v)) {
            n = pi_json_parse(v->valuestring, strlen(v->valuestring));
            if (!cJSON_IsNumber(n)) {
                cJSON_Delete(n);
                n = NULL;
                snprintf(err, errlen, "expected a number for %s, got \"%s\"", name,
                         v->valuestring);
            }
        } else {
            snprintf(err, errlen, "expected a number for %s, got %s", name, shown ? shown : "?");
        }
        if (n) {
            /* The same range and type check as a read: a value that would not decode back is
             * refused. */
            tdot_value_t check;
            if (pi_to_datatype(n, dt, &check, err, errlen) == 0)
                out = n;
            else
                cJSON_Delete(n);
        }
    }
    free(shown);
    cJSON_Delete(v);
    return out;
}
