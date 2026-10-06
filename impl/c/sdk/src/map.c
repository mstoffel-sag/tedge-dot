/* Value mapping (contract §4.3); see include/tedge_dot/map.h.
 *
 * Every rule and message mirrors impl/rust/crates/sdk/src/map.rs. */
#include "tedge_dot/map.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- numbers as text ------------------------------------------------------ */

bool tdot_map_parse_number(const char *text, double *out) {
    const char *s = text;
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    size_t i = 0, digits = 0;
    if (i < n && (s[i] == '+' || s[i] == '-'))
        i++;
    while (i < n && isdigit((unsigned char)s[i]))
        i++, digits++;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && isdigit((unsigned char)s[i]))
            i++, digits++;
    }
    if (digits == 0)
        return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        size_t start = i;
        while (i < n && isdigit((unsigned char)s[i]))
            i++;
        if (i == start)
            return false;
    }
    if (i != n)
        return false;
    char buf[512];
    if (n >= sizeof buf)
        return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    errno = 0;
    double v = strtod(buf, NULL);
    if (!isfinite(v))
        return false;
    *out = v;
    return true;
}

/* M * 10^X reads back as n. */
static bool reads_back(uint64_t m, int x, double n) {
    char buf[64];
    snprintf(buf, sizeof buf, "%" PRIu64 "e%d", m, x);
    return strtod(buf, NULL) == n;
}

bool tdot_map_format_number(double n, char *buf, size_t len) {
    if (!isfinite(n) || len == 0)
        return false;
    if (n == 0.0) {
        snprintf(buf, len, "0");
        return true;
    }
    double a = fabs(n);
    /* The fewest significant digits that read back: the correctly rounded
     * p-digit decimal, or -- next to a power of two, where the gap below is
     * half the one above -- its neighbour in the last digit. */
    uint64_t m = 0;
    int x = 0;
    for (int p = 0; p <= 16; p++) {
        char e[64];
        snprintf(e, sizeof e, "%.*e", p, a);
        char *ep = strchr(e, 'e');
        int exp10 = atoi(ep + 1);
        uint64_t mant = 0;
        for (char *c = e; c < ep; c++)
            if (isdigit((unsigned char)*c))
                mant = mant * 10 + (uint64_t)(*c - '0');
        int scale = exp10 - p;
        if (reads_back(mant, scale, a)) {
            m = mant, x = scale;
            break;
        }
        if (reads_back(mant + 1, scale, a)) {
            m = mant + 1, x = scale;
            break;
        }
        if (mant > 1 && reads_back(mant - 1, scale, a)) {
            m = mant - 1, x = scale;
            break;
        }
    }
    if (m == 0)
        return false; /* unreachable: 17 digits always read back */
    while (m % 10 == 0) {
        m /= 10;
        x++;
    }
    char digits[32];
    int k = snprintf(digits, sizeof digits, "%" PRIu64, m);
    int pos = k + x; /* digits before the decimal point */
    size_t need = (size_t)(n < 0) + (size_t)k + (size_t)(pos <= 0 ? 2 - pos : (pos > k ? pos - k : 1)) + 1;
    if (need > len)
        return false;
    char *o = buf;
    if (n < 0)
        *o++ = '-';
    if (pos <= 0) {
        *o++ = '0';
        *o++ = '.';
        for (int i = 0; i < -pos; i++)
            *o++ = '0';
        memcpy(o, digits, (size_t)k);
        o += k;
    } else if (pos >= k) {
        memcpy(o, digits, (size_t)k);
        o += k;
        for (int i = 0; i < pos - k; i++)
            *o++ = '0';
    } else {
        memcpy(o, digits, (size_t)pos);
        o += pos;
        *o++ = '.';
        memcpy(o, digits + pos, (size_t)(k - pos));
        o += k - pos;
    }
    *o = '\0';
    return true;
}

/* ---- message text ------------------------------------------------------------ */

/* `s` as a JSON string literal, escaped as serde_json escapes it. */
static void quote(const char *s, char *buf, size_t len) {
    size_t o = 0;
#define PUT(c)                                                                         \
    do {                                                                               \
        if (o + 1 < len)                                                               \
            buf[o++] = (c);                                                            \
    } while (0)
    PUT('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': PUT('\\'); PUT('"'); break;
        case '\\': PUT('\\'); PUT('\\'); break;
        case '\n': PUT('\\'); PUT('n'); break;
        case '\r': PUT('\\'); PUT('r'); break;
        case '\t': PUT('\\'); PUT('t'); break;
        case '\b': PUT('\\'); PUT('b'); break;
        case '\f': PUT('\\'); PUT('f'); break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                for (char *e = esc; *e; e++)
                    PUT(*e);
            } else {
                PUT((char)*p);
            }
        }
    }
    PUT('"');
#undef PUT
    buf[o < len ? o : len - 1] = '\0';
}

static void number_text(double n, char *buf, size_t len) {
    if (!tdot_map_format_number(n, buf, len))
        snprintf(buf, len, "%s", isnan(n) ? "NaN" : n > 0 ? "inf" : "-inf");
}

/* A value as messages show it: numbers as tdot_map_format_number writes them,
 * strings quoted. */
static void value_text(const tdot_value_t *v, char *buf, size_t len) {
    switch (v->kind) {
    case TDOT_VAL_NUM: number_text(v->num, buf, len); break;
    case TDOT_VAL_STR: quote(v->str, buf, len); break;
    case TDOT_VAL_BOOL: snprintf(buf, len, "%s", v->b ? "true" : "false"); break;
    default: snprintf(buf, len, "null"); break;
    }
}

static void val_text(const tdot_map_val_t *v, char *buf, size_t len) {
    switch (v->kind) {
    case TDOT_MAP_NUMBER:
        if (v->is_int)
            snprintf(buf, len, "%" PRId64, v->i);
        else
            number_text(v->num, buf, len);
        break;
    case TDOT_MAP_STRING: quote(v->s, buf, len); break;
    case TDOT_MAP_BOOL: snprintf(buf, len, "%s", v->b ? "true" : "false"); break;
    }
}

static const char *noun(tdot_map_kind_t k) {
    return k == TDOT_MAP_NUMBER ? "a number" : k == TDOT_MAP_STRING ? "a string" : "a bool";
}

static const char *repr(tdot_map_kind_t k) {
    return k == TDOT_MAP_NUMBER ? "number" : k == TDOT_MAP_STRING ? "string" : "boolean";
}

/* ---- parsing ----------------------------------------------------------------- */

static bool present(toml_table_t *t, const char *key) {
    for (int i = 0;; i++) {
        const char *k = toml_key_in(t, i);
        if (!k)
            return false;
        if (strcmp(k, key) == 0)
            return true;
    }
}

static void val_free(tdot_map_val_t *v) {
    free(v->s);
    v->s = NULL;
}

/* A scalar at `key` / index: 1 when found, 0 when absent or not a scalar. */
static int scalar_in(toml_table_t *t, const char *key, tdot_map_val_t *out) {
    memset(out, 0, sizeof *out);
    toml_datum_t d;
    if ((d = toml_string_in(t, key)).ok) {
        out->kind = TDOT_MAP_STRING;
        out->s = d.u.s;
    } else if ((d = toml_bool_in(t, key)).ok) {
        out->kind = TDOT_MAP_BOOL;
        out->b = d.u.b;
    } else if ((d = toml_int_in(t, key)).ok) {
        out->kind = TDOT_MAP_NUMBER;
        out->is_int = true;
        out->i = d.u.i;
        out->num = (double)d.u.i;
    } else if ((d = toml_double_in(t, key)).ok) {
        out->kind = TDOT_MAP_NUMBER;
        out->num = d.u.d;
    } else {
        return 0;
    }
    return 1;
}

static int scalar_at(toml_array_t *a, int idx, tdot_map_val_t *out) {
    memset(out, 0, sizeof *out);
    toml_datum_t d;
    if ((d = toml_string_at(a, idx)).ok) {
        out->kind = TDOT_MAP_STRING;
        out->s = d.u.s;
    } else if ((d = toml_bool_at(a, idx)).ok) {
        out->kind = TDOT_MAP_BOOL;
        out->b = d.u.b;
    } else if ((d = toml_int_at(a, idx)).ok) {
        out->kind = TDOT_MAP_NUMBER;
        out->is_int = true;
        out->i = d.u.i;
        out->num = (double)d.u.i;
    } else if ((d = toml_double_at(a, idx)).ok) {
        out->kind = TDOT_MAP_NUMBER;
        out->num = d.u.d;
    } else {
        return 0;
    }
    return 1;
}

/* A finite number at `key`: 1 found, 0 absent, -1 present but not one. */
static int number_in(toml_table_t *t, const char *key, double *out) {
    if (!present(t, key))
        return 0;
    toml_datum_t d;
    if ((d = toml_int_in(t, key)).ok) {
        *out = (double)d.u.i;
        return 1;
    }
    if ((d = toml_double_in(t, key)).ok && isfinite(d.u.d)) {
        *out = d.u.d;
        return 1;
    }
    return -1;
}

static void case_free(tdot_map_case_t *c) {
    for (size_t i = 0; i < c->neq; i++)
        val_free(&c->eq[i]);
    free(c->eq);
    val_free(&c->write);
    val_free(&c->to);
}

void tdot_map_free(tdot_map_t *map) {
    if (!map)
        return;
    for (size_t i = 0; i < map->ncases; i++)
        case_free(&map->cases[i]);
    free(map->cases);
    val_free(&map->def);
    free(map);
}

/* One case as written; `err` gets the suffix after `map.cases[i]`. */
static int parse_case(toml_table_t *t, tdot_map_case_t *c, char *err, size_t errlen) {
    memset(c, 0, sizeof *c);
    if (!present(t, "to")) {
        snprintf(err, errlen, " needs a to");
        return -1;
    }
    if (!scalar_in(t, "to", &c->to)) {
        snprintf(err, errlen, ".to must be a number, a string or a bool");
        return -1;
    }
    int has_min = number_in(t, "min", &c->min);
    if (has_min < 0) {
        snprintf(err, errlen, ".min must be a number");
        return -1;
    }
    int has_max = number_in(t, "max", &c->max);
    if (has_max < 0) {
        snprintf(err, errlen, ".max must be a number");
        return -1;
    }
    c->has_min = has_min > 0;
    c->has_max = has_max > 0;
    bool range = c->has_min || c->has_max;
    bool has_eq = present(t, "eq");
    if (has_eq && range) {
        snprintf(err, errlen, " cannot have both eq and min/max");
        return -1;
    }
    if (!has_eq && !range) {
        snprintf(err, errlen, " needs eq, or min and/or max");
        return -1;
    }
    if (has_eq) {
        if (present(t, "write")) {
            snprintf(err, errlen, ".write is only for a range (the eq value is what is written)");
            return -1;
        }
        toml_array_t *list = toml_array_in(t, "eq");
        if (list) {
            int n = toml_array_nelem(list);
            if (n == 0) {
                snprintf(err, errlen, ".eq must not be an empty list");
                return -1;
            }
            c->eq = calloc((size_t)n, sizeof *c->eq);
            for (int i = 0; i < n; i++) {
                if (!scalar_at(list, i, &c->eq[c->neq])) {
                    snprintf(err, errlen,
                             ".eq must be a number, a string, a bool or a list of them");
                    return -1;
                }
                c->neq++;
            }
        } else {
            c->eq = calloc(1, sizeof *c->eq);
            if (!scalar_in(t, "eq", &c->eq[0])) {
                snprintf(err, errlen, ".eq must be a number, a string, a bool or a list of them");
                return -1;
            }
            c->neq = 1;
        }
        return 0;
    }
    c->range = true;
    if (c->has_min && c->has_max && c->min > c->max) {
        snprintf(err, errlen, ".min must not be greater than .max");
        return -1;
    }
    double w;
    int has_write = number_in(t, "write", &w);
    if (has_write < 0) {
        snprintf(err, errlen, ".write must be a number");
        return -1;
    }
    if (has_write > 0) {
        if ((c->has_min && w < c->min) || (c->has_max && w > c->max)) {
            snprintf(err, errlen, ".write must lie within the case's range");
            return -1;
        }
        c->has_write = true;
        scalar_in(t, "write", &c->write);
    }
    return 0;
}

tdot_map_kind_t tdot_map_output_kind(const tdot_map_t *map) {
    if (map->has_as)
        return map->as;
    if (map->ncases)
        return map->cases[0].to.kind;
    if (map->has_default)
        return map->def.kind;
    return TDOT_MAP_STRING;
}

static int parse_kind(const char *name, tdot_map_kind_t *out) {
    if (!strcmp(name, "number"))
        *out = TDOT_MAP_NUMBER;
    else if (!strcmp(name, "string"))
        *out = TDOT_MAP_STRING;
    else if (!strcmp(name, "bool"))
        *out = TDOT_MAP_BOOL;
    else
        return -1;
    return 0;
}

int tdot_map_parse(toml_table_t *table, tdot_map_t **out, char *err, size_t errlen) {
    *out = NULL;
    if (!toml_key_in(table, 0))
        return 0; /* map = {}: no map */
    tdot_map_t *map = calloc(1, sizeof *map);
    if (present(table, "as")) {
        toml_datum_t d = toml_string_in(table, "as");
        int bad = !d.ok || parse_kind(d.u.s, &map->as) != 0;
        if (d.ok)
            free(d.u.s);
        if (bad) {
            snprintf(err, errlen, "map.as must be one of \"number\", \"string\", \"bool\"");
            goto fail;
        }
        map->has_as = true;
    }
    if (present(table, "default")) {
        if (!scalar_in(table, "default", &map->def)) {
            snprintf(err, errlen, "map.default must be a number, a string or a bool");
            goto fail;
        }
        map->has_default = true;
    }
    if (map->has_default && map->has_as) {
        snprintf(err, errlen, "map.default and map.as cannot be combined");
        goto fail;
    }
    if (present(table, "cases")) {
        toml_array_t *cases = toml_array_in(table, "cases");
        if (!cases) {
            snprintf(err, errlen, "map.cases must be an array of tables");
            goto fail;
        }
        int n = toml_array_nelem(cases);
        map->cases = calloc(n > 0 ? (size_t)n : 1, sizeof *map->cases);
        for (int i = 0; i < n; i++) {
            toml_table_t *ct = toml_table_at(cases, i);
            char why[256];
            if (!ct) {
                snprintf(err, errlen, "map.cases[%d] must be a table", i);
                goto fail;
            }
            int rc = parse_case(ct, &map->cases[map->ncases], why, sizeof why);
            map->ncases++; /* counted either way, so tdot_map_free releases it */
            if (rc != 0) {
                snprintf(err, errlen, "map.cases[%d]%s", i, why);
                goto fail;
            }
        }
    }
    if (map->ncases == 0 && !map->has_default && !map->has_as) {
        snprintf(err, errlen, "map needs cases, a default or as");
        goto fail;
    }
    tdot_map_kind_t first = tdot_map_output_kind(map);
    for (size_t i = 0; i < map->ncases; i++) {
        if (map->cases[i].to.kind != first) {
            snprintf(err, errlen, "map.cases[%zu].to must be %s like the map's other outputs", i,
                     noun(first));
            goto fail;
        }
    }
    if (map->has_default && map->def.kind != first) {
        snprintf(err, errlen, "map.default must be %s like the map's other outputs", noun(first));
        goto fail;
    }
    *out = map;
    return 0;
fail:
    tdot_map_free(map);
    return -1;
}

/* ---- checks against the point --------------------------------------------- */

static tdot_map_kind_t native_kind(tdot_datatype_t dt) {
    return dt == TDOT_DT_BOOL ? TDOT_MAP_BOOL : dt == TDOT_DT_STRING ? TDOT_MAP_STRING : TDOT_MAP_NUMBER;
}

int tdot_map_check_point(const tdot_map_t *map, bool raw_mode, const char *datatype_name,
                         char *err, size_t errlen) {
    if (raw_mode) {
        snprintf(err, errlen, "map is not allowed on a raw-mode point");
        return -1;
    }
    if (datatype_name && !strcmp(datatype_name, "bytes")) {
        snprintf(err, errlen, "map is not allowed on a bytes point");
        return -1;
    }
    tdot_datatype_t dt = datatype_name ? tdot_datatype_parse(datatype_name) : TDOT_DT_NONE;
    if (dt == TDOT_DT_NONE)
        return 0; /* a typed point without a datatype is refused elsewhere */
    tdot_map_kind_t native = native_kind(dt);
    for (size_t i = 0; i < map->ncases; i++) {
        const tdot_map_case_t *c = &map->cases[i];
        if (c->range) {
            if (native != TDOT_MAP_NUMBER) {
                snprintf(err, errlen,
                         "map.cases[%zu] is a range, which needs a numeric datatype (got %s)", i,
                         datatype_name);
                return -1;
            }
            continue;
        }
        for (size_t j = 0; j < c->neq; j++) {
            if (c->eq[j].kind != native) {
                char text[128];
                val_text(&c->eq[j], text, sizeof text);
                snprintf(err, errlen, "map.cases[%zu].eq %s can never match a %s value", i, text,
                         datatype_name);
                return -1;
            }
        }
    }
    return 0;
}

/* ---- reads ------------------------------------------------------------------- */

static bool is_big(tdot_datatype_t dt) { return dt == TDOT_DT_INT64 || dt == TDOT_DT_UINT64; }

/* A 64-bit integer carried as text (§4.1): 1 for a negative one in *neg, 2 for
 * a non-negative one in *pos, 0 when the text is not an integer. */
static int big_integer(const char *text, int64_t *neg, uint64_t *pos) {
    const char *s = text;
    if (*s == '+' || *s == '-')
        s++;
    if (!*s)
        return 0;
    for (const char *c = s; *c; c++)
        if (!isdigit((unsigned char)*c))
            return 0;
    errno = 0;
    if (text[0] == '-') {
        long long v = strtoll(text, NULL, 10);
        if (errno)
            return 0;
        *neg = v;
        return v < 0 ? 1 : (*pos = (uint64_t)v, 2);
    }
    unsigned long long v = strtoull(text[0] == '+' ? text + 1 : text, NULL, 10);
    if (errno)
        return 0;
    *pos = v;
    return 2;
}

static bool val_matches(const tdot_map_val_t *eq, const tdot_value_t *v, bool big) {
    switch (eq->kind) {
    case TDOT_MAP_BOOL:
        return v->kind == TDOT_VAL_BOOL && v->b == eq->b;
    case TDOT_MAP_STRING:
        return !big && v->kind == TDOT_VAL_STR && strcmp(v->str, eq->s) == 0;
    case TDOT_MAP_NUMBER:
        if (v->kind == TDOT_VAL_NUM)
            return eq->num == v->num;
        if (v->kind == TDOT_VAL_STR && big) {
            int64_t neg;
            uint64_t pos;
            int which = big_integer(v->str, &neg, &pos);
            if (!which)
                return false;
            if (!eq->is_int)
                return eq->num == (which == 1 ? (double)neg : (double)pos);
            if (which == 1)
                return eq->i == neg;
            return eq->i >= 0 && (uint64_t)eq->i == pos;
        }
        return false;
    }
    return false;
}

static bool case_matches(const tdot_map_case_t *c, const tdot_value_t *v, bool big) {
    if (!c->range) {
        for (size_t i = 0; i < c->neq; i++)
            if (val_matches(&c->eq[i], v, big))
                return true;
        return false;
    }
    double n;
    if (v->kind == TDOT_VAL_NUM) {
        n = v->num;
    } else if (v->kind == TDOT_VAL_STR && big) {
        int64_t neg;
        uint64_t pos;
        int which = big_integer(v->str, &neg, &pos);
        if (!which)
            return false;
        n = which == 1 ? (double)neg : (double)pos;
    } else {
        return false;
    }
    return (!c->has_min || c->min <= n) && (!c->has_max || n <= c->max);
}

static void val_to_value(const tdot_map_val_t *m, tdot_value_t *out) {
    memset(out, 0, sizeof *out);
    switch (m->kind) {
    case TDOT_MAP_NUMBER:
        out->kind = TDOT_VAL_NUM;
        out->num = m->num;
        break;
    case TDOT_MAP_STRING:
        out->kind = TDOT_VAL_STR;
        snprintf(out->str, sizeof out->str, "%s", m->s);
        break;
    case TDOT_MAP_BOOL:
        out->kind = TDOT_VAL_BOOL;
        out->b = m->b;
        break;
    }
}

static bool parse_bool(const char *text, bool *out) {
    const char *s = text;
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    char low[8];
    if (n >= sizeof low)
        return false;
    for (size_t i = 0; i < n; i++)
        low[i] = (char)tolower((unsigned char)s[i]);
    low[n] = '\0';
    if (!strcmp(low, "true") || !strcmp(low, "1") || !strcmp(low, "on") || !strcmp(low, "yes")) {
        *out = true;
        return true;
    }
    if (!strcmp(low, "false") || !strcmp(low, "0") || !strcmp(low, "off") || !strcmp(low, "no")) {
        *out = false;
        return true;
    }
    return false;
}

/* The `as` conversion of `in` to `kind`; `big` marks a 64-bit integer carried
 * as text, which is a number, not a string. */
static int convert(const tdot_value_t *in, tdot_map_kind_t kind, bool big, tdot_value_t *out,
                   char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    bool ok = true;
    switch (kind) {
    case TDOT_MAP_NUMBER:
        out->kind = TDOT_VAL_NUM;
        if (in->kind == TDOT_VAL_NUM)
            out->num = in->num;
        else if (in->kind == TDOT_VAL_BOOL)
            out->num = in->b ? 1.0 : 0.0;
        else
            ok = in->kind == TDOT_VAL_STR && tdot_map_parse_number(in->str, &out->num);
        break;
    case TDOT_MAP_STRING:
        out->kind = TDOT_VAL_STR;
        if (in->kind == TDOT_VAL_STR)
            snprintf(out->str, sizeof out->str, "%s", in->str);
        else if (in->kind == TDOT_VAL_BOOL)
            snprintf(out->str, sizeof out->str, "%s", in->b ? "true" : "false");
        else
            /* fails for NaN and infinities, and for a number whose
             * positional text does not fit a value (str[256]) */
            ok = in->kind == TDOT_VAL_NUM &&
                 tdot_map_format_number(in->num, out->str, sizeof out->str);
        break;
    case TDOT_MAP_BOOL:
        out->kind = TDOT_VAL_BOOL;
        if (in->kind == TDOT_VAL_BOOL) {
            out->b = in->b;
        } else if (in->kind == TDOT_VAL_NUM) {
            ok = !isnan(in->num);
            out->b = in->num != 0.0;
        } else if (in->kind == TDOT_VAL_STR && big) {
            int64_t neg;
            uint64_t pos;
            int which = big_integer(in->str, &neg, &pos);
            ok = which != 0;
            out->b = which == 1 || (which == 2 && pos != 0);
        } else {
            ok = in->kind == TDOT_VAL_STR && parse_bool(in->str, &out->b);
        }
        break;
    }
    if (!ok) {
        char text[300];
        value_text(in, text, sizeof text);
        snprintf(err, errlen, "cannot convert %s to %s", text, noun(kind));
        return -1;
    }
    return 0;
}

int tdot_map_apply(const tdot_map_t *map, const tdot_value_t *in, tdot_datatype_t dt,
                   tdot_value_t *out, char *err, size_t errlen) {
    bool big = is_big(dt);
    for (size_t i = 0; i < map->ncases; i++) {
        if (case_matches(&map->cases[i], in, big)) {
            val_to_value(&map->cases[i].to, out);
            return 0;
        }
    }
    if (map->has_default) {
        val_to_value(&map->def, out);
        return 0;
    }
    if (map->has_as)
        return convert(in, map->as, big, out, err, errlen);
    char text[300];
    value_text(in, text, sizeof text);
    snprintf(err, errlen, "no mapping for value %s", text);
    return -1;
}

/* ---- writes ------------------------------------------------------------------ */

static tdot_map_kind_t value_kind(const tdot_value_t *v) {
    return v->kind == TDOT_VAL_NUM ? TDOT_MAP_NUMBER
           : v->kind == TDOT_VAL_BOOL ? TDOT_MAP_BOOL
                                      : TDOT_MAP_STRING;
}

/* Two outputs, or an output and a written value, are equal: numbers
 * numerically, the rest exactly. */
static bool val_equal(const tdot_map_val_t *a, const tdot_map_val_t *b) {
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
    case TDOT_MAP_NUMBER: return a->num == b->num;
    case TDOT_MAP_STRING: return strcmp(a->s, b->s) == 0;
    case TDOT_MAP_BOOL: return a->b == b->b;
    }
    return false;
}

static bool val_equals_value(const tdot_map_val_t *a, const tdot_value_t *v) {
    switch (a->kind) {
    case TDOT_MAP_NUMBER: return v->kind == TDOT_VAL_NUM && a->num == v->num;
    case TDOT_MAP_STRING: return v->kind == TDOT_VAL_STR && strcmp(a->s, v->str) == 0;
    case TDOT_MAP_BOOL: return v->kind == TDOT_VAL_BOOL && a->b == v->b;
    }
    return false;
}

size_t tdot_map_writable(const tdot_map_t *map, const tdot_map_val_t **out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < map->ncases; i++) {
        const tdot_map_case_t *c = &map->cases[i];
        if (c->range && !c->has_write)
            continue;
        bool seen = false;
        for (size_t j = 0; j < n && j < max; j++)
            if (val_equal(out[j], &c->to))
                seen = true;
        if (seen)
            continue;
        if (n < max)
            out[n] = &c->to;
        n++;
    }
    return n;
}

/* A device value from a map scalar: a 64-bit integer outside the safe range
 * is carried as text, as decode carries it (§4.1). */
static void device_value(const tdot_map_val_t *m, tdot_value_t *out) {
    if (m->kind == TDOT_MAP_NUMBER && m->is_int &&
        (m->i > TDOT_JS_SAFE_MAX || m->i < -TDOT_JS_SAFE_MAX)) {
        memset(out, 0, sizeof *out);
        out->kind = TDOT_VAL_STR;
        snprintf(out->str, sizeof out->str, "%" PRId64, m->i);
        return;
    }
    val_to_value(m, out);
}

int tdot_map_invert(const tdot_map_t *map, const tdot_value_t *in, tdot_datatype_t dt,
                    tdot_value_t *out, char *err, size_t errlen) {
    char text[300];
    value_text(in, text, sizeof text);
    tdot_map_kind_t kind = tdot_map_output_kind(map);
    if (in->kind == TDOT_VAL_NONE || value_kind(in) != kind) {
        snprintf(err, errlen, "cannot write %s: the point's mapped values are %ss", text,
                 repr(kind));
        return -1;
    }
    for (size_t i = 0; i < map->ncases; i++) {
        const tdot_map_case_t *c = &map->cases[i];
        if (!val_equals_value(&c->to, in))
            continue;
        if (!c->range) {
            device_value(&c->eq[0], out);
            return 0;
        }
        if (c->has_write) {
            device_value(&c->write, out);
            return 0;
        }
        /* read-only; a later case may write it */
    }
    if (map->has_as) {
        tdot_map_kind_t native = dt == TDOT_DT_NONE ? map->as : native_kind(dt);
        char why[400];
        if (convert(in, native, false, out, why, sizeof why) != 0) {
            snprintf(err, errlen, "cannot write %s: %s", text, why);
            return -1;
        }
        return 0;
    }
    const tdot_map_val_t *accepted[64];
    size_t n = tdot_map_writable(map, accepted, 64);
    if (n == 0) {
        snprintf(err, errlen, "cannot write %s: the point's map has no writable values", text);
        return -1;
    }
    size_t used = (size_t)snprintf(err, errlen, "cannot write %s; accepted values: ", text);
    for (size_t i = 0; i < n && i < 64 && used < errlen; i++) {
        char item[300];
        val_text(accepted[i], item, sizeof item);
        used += (size_t)snprintf(err + used, errlen - used, "%s%s", i ? ", " : "", item);
    }
    return -1;
}
