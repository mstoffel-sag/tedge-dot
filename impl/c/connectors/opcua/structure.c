/* tedge-dot — the OPC UA structure walker (structure.h). A port of
 * impl/rust/crates/connector-opcua/src/structure.rs: the same plan, the same
 * order of checks and the same messages. */
#include "structure.h"

#include <open62541/types.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ticks (100 ns) between the OPC UA epoch (1601-01-01) and the Unix epoch. */
#define UNIX_EPOCH_TICKS 116444736000000000LL
/* 9999-12-31T23:59:59.9999999Z, where Part 6 clamps every later DateTime. */
#define MAX_TICKS 2650467743999999999LL
/* How deep nested structures, Variants and DiagnosticInfos may go. */
#define MAX_DEPTH 32

static const char *const BUILTIN_NAMES[] = {
    NULL,           "Boolean",       "SByte",          "Byte",
    "Int16",        "UInt16",        "Int32",          "UInt32",
    "Int64",        "UInt64",        "Float",          "Double",
    "String",       "DateTime",      "Guid",           "ByteString",
    "XmlElement",   "NodeId",        "ExpandedNodeId", "StatusCode",
    "QualifiedName", "LocalizedText", "ExtensionObject", "DataValue",
    "Variant",      "DiagnosticInfo"};

const char *uas_builtin_name(int id) {
    return id >= 1 && id <= 25 ? BUILTIN_NAMES[id] : NULL;
}

int uas_builtin_from_name(const char *name) {
    for (int i = 1; i <= 25; i++)
        if (!strcmp(BUILTIN_NAMES[i], name))
            return i;
    return 0;
}

__attribute__((format(printf, 3, 4)))
static void fail(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

void uas_types_free(uas_types_t *t) {
    for (size_t i = 0; i < t->n; i++) {
        uas_struct_t *s = &t->structs[i];
        for (size_t j = 0; j < s->nfields; j++) {
            free(s->fields[j].name);
            free(s->fields[j].type_name);
        }
        free(s->fields);
        free(s->name);
    }
    free(t->structs);
    t->structs = NULL;
    t->n = 0;
}

/* ---- paths ---------------------------------------------------------------- */

void uas_path_free(uas_path_t *p) {
    for (size_t i = 0; i < p->n; i++)
        free(p->segs[i].name);
    free(p->segs);
    p->segs = NULL;
    p->n = 0;
}

int uas_parse_path(const char *path, uas_path_t *out, char *err, size_t errlen) {
    out->segs = NULL;
    out->n = 0;
    if (!*path) {
        fail(err, errlen, "field path is empty");
        return -1;
    }
    const char *p = path;
    for (;;) {
        const char *end = strchr(p, '.');
        size_t plen = end ? (size_t)(end - p) : strlen(p);
        const char *open = memchr(p, '[', plen);
        size_t nlen = open ? (size_t)(open - p) : plen;
        int64_t index = -1;
        if (open) {
            /* digits, then a closing bracket that ends the segment */
            const char *d = open + 1, *stop = p + plen;
            if (stop - d < 2 || stop[-1] != ']') {
                fail(err, errlen, "field path \"%s\" has an invalid index", path);
                goto bad;
            }
            unsigned long long v = 0;
            for (const char *c = d; c < stop - 1; c++) {
                if (*c < '0' || *c > '9' || v > 0xFFFFFFFFull) {
                    v = ~0ull;
                    break;
                }
                v = v * 10 + (unsigned)(*c - '0');
            }
            if (v > 0xFFFFFFFFull) {
                fail(err, errlen, "field path \"%s\" has an invalid index", path);
                goto bad;
            }
            index = (int64_t)v;
        }
        if (nlen == 0) {
            fail(err, errlen, "field path \"%s\" has an empty segment", path);
            goto bad;
        }
        if (memchr(p, ']', nlen)) {
            fail(err, errlen, "field path \"%s\" has an invalid index", path);
            goto bad;
        }
        uas_seg_t *grown = realloc(out->segs, (out->n + 1) * sizeof *grown);
        char *name = malloc(nlen + 1);
        if (!grown || !name) {
            free(name);
            if (grown)
                out->segs = grown;
            fail(err, errlen, "out of memory");
            goto bad;
        }
        memcpy(name, p, nlen);
        name[nlen] = '\0';
        out->segs = grown;
        out->segs[out->n].name = name;
        out->segs[out->n].index = index;
        out->n++;
        if (!end)
            return 0;
        p = end + 1;
    }
bad:
    uas_path_free(out);
    return -1;
}

static void seg_text(const uas_seg_t *s, char *out, size_t outlen) {
    if (s->index >= 0)
        snprintf(out, outlen, "%s[%" PRId64 "]", s->name, s->index);
    else
        snprintf(out, outlen, "%s", s->name);
}

/* path[0..upto) joined with dots. */
static void path_prefix(const uas_path_t *p, size_t upto, char *out, size_t outlen) {
    size_t len = 0;
    out[0] = '\0';
    for (size_t i = 0; i < upto && len < outlen; i++) {
        if (i)
            len += (size_t)snprintf(out + len, outlen - len, ".");
        if (len >= outlen)
            break;
        seg_text(&p->segs[i], out + len, outlen - len);
        len = strlen(out);
    }
}

void uas_path_text(const uas_path_t *p, char *out, size_t outlen) {
    path_prefix(p, p->n, out, outlen);
}

/* ---- datatypes -------------------------------------------------------------- */

static const char *leaf_name(const uas_leaf_t *leaf) {
    return leaf->builtin ? uas_builtin_name(leaf->builtin) : leaf->enum_name;
}

size_t uas_accepted(const uas_leaf_t *leaf, tdot_datatype_t out[2]) {
    if (!leaf->builtin) {
        out[0] = TDOT_DT_INT32;
        return 1;
    }
    switch (leaf->builtin) {
    case UAS_BOOLEAN: out[0] = TDOT_DT_BOOL; return 1;
    case UAS_SBYTE: out[0] = TDOT_DT_INT8; return 1;
    case UAS_BYTE: out[0] = TDOT_DT_UINT8; return 1;
    case UAS_INT16: out[0] = TDOT_DT_INT16; return 1;
    case UAS_UINT16: out[0] = TDOT_DT_UINT16; return 1;
    case UAS_INT32: out[0] = TDOT_DT_INT32; return 1;
    case UAS_UINT32: out[0] = TDOT_DT_UINT32; return 1;
    case UAS_INT64: out[0] = TDOT_DT_INT64; return 1;
    case UAS_UINT64: out[0] = TDOT_DT_UINT64; return 1;
    case UAS_FLOAT: out[0] = TDOT_DT_FLOAT32; return 1;
    case UAS_DOUBLE: out[0] = TDOT_DT_FLOAT64; return 1;
    case UAS_STRING: out[0] = TDOT_DT_STRING; return 1;
    case UAS_DATETIME:
        out[0] = TDOT_DT_STRING;
        out[1] = TDOT_DT_INT64;
        return 2;
    case UAS_STATUSCODE:
        out[0] = TDOT_DT_UINT32;
        out[1] = TDOT_DT_STRING;
        return 2;
    case UAS_LOCALIZEDTEXT:
    case UAS_GUID:
    case UAS_NODEID:
    case UAS_EXPANDEDNODEID:
    case UAS_QUALIFIEDNAME: out[0] = TDOT_DT_STRING; return 1;
    case UAS_BYTESTRING: out[0] = TDOT_DT_BYTES; return 1;
    default: return 0;
    }
}

int uas_check_datatype(const uas_leaf_t *leaf, tdot_datatype_t dt, const char *subject,
                       char *err, size_t errlen) {
    tdot_datatype_t ok[2];
    size_t n = uas_accepted(leaf, ok);
    if (n == 0) {
        fail(err, errlen, "%s is %s, which a point cannot read", subject, leaf_name(leaf));
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if (ok[i] == dt)
            return 0;
    fail(err, errlen, "%s is %s, point declares %s (accepted: %s%s%s)", subject,
         leaf_name(leaf), tdot_datatype_str(dt), tdot_datatype_str(ok[0]), n > 1 ? ", " : "",
         n > 1 ? tdot_datatype_str(ok[1]) : "");
    return -1;
}

/* ---- compile ---------------------------------------------------------------- */

static const char *type_name(const uas_types_t *t, const uas_field_t *f) {
    switch (f->kind) {
    case UAS_T_BUILTIN: return uas_builtin_name(f->builtin);
    case UAS_T_ENUM: return f->type_name;
    default: return f->ref < t->n ? t->structs[f->ref].name : "?";
    }
}

void uas_plan_free(uas_plan_t *p) {
    free(p->steps);
    p->steps = NULL;
    p->n = 0;
}

int uas_compile(const uas_types_t *t, size_t root, const uas_path_t *path,
                tdot_datatype_t dt, uas_plan_t *plan, char *err, size_t errlen) {
    memset(plan, 0, sizeof *plan);
    if (path->n == 0) {
        fail(err, errlen, "field path is empty");
        return -1;
    }
    plan->steps = calloc(path->n, sizeof *plan->steps);
    if (!plan->steps) {
        fail(err, errlen, "out of memory");
        return -1;
    }
    size_t key = root;
    char so_far[512];
    for (size_t i = 0; i < path->n; i++) {
        const uas_seg_t *seg = &path->segs[i];
        if (key >= t->n) {
            fail(err, errlen, "no definition for structure type %zu", key);
            goto bad;
        }
        const uas_struct_t *def = &t->structs[key];
        size_t pos = def->nfields;
        for (size_t j = 0; j < def->nfields && pos == def->nfields; j++)
            if (!strcmp(def->fields[j].name, seg->name))
                pos = j;
        if (pos == def->nfields) {
            char names[400];
            size_t len = 0;
            names[0] = '\0';
            for (size_t j = 0; j < def->nfields && len < sizeof names; j++)
                len += (size_t)snprintf(names + len, sizeof names - len, "%s%s",
                                        j ? ", " : "", def->fields[j].name);
            fail(err, errlen, "field \"%s\" not found in %s (fields: %s)", seg->name,
                 def->name, names);
            goto bad;
        }
        const uas_field_t *field = &def->fields[pos];
        path_prefix(path, i + 1, so_far, sizeof so_far);
        if (!field->array && seg->index >= 0) {
            fail(err, errlen, "field \"%s\" is not an array", field->name);
            goto bad;
        }
        if (field->array && seg->index < 0) {
            fail(err, errlen, "field \"%s\" is an array of %s; select one element, e.g. \"%s[0]\"",
                 field->name, type_name(t, field), so_far);
            goto bad;
        }
        plan->steps[plan->n++] = (uas_step_t){.s = key, .field = pos, .index = seg->index};
        bool last = i + 1 == path->n;
        if (!last) {
            if (field->kind != UAS_T_STRUCT) {
                fail(err, errlen, "field \"%s\" is %s, not a structure", field->name,
                     type_name(t, field));
                goto bad;
            }
            key = field->ref;
            continue;
        }
        char subject[300];
        snprintf(subject, sizeof subject, "field \"%s\"", field->name);
        switch (field->kind) {
        case UAS_T_STRUCT: {
            const uas_struct_t *inner = field->ref < t->n ? &t->structs[field->ref] : NULL;
            if (!inner) {
                fail(err, errlen, "no definition for structure type %zu", field->ref);
                goto bad;
            }
            fail(err, errlen,
                 "field \"%s\" is a structure (%s); select one of its fields, e.g. \"%s.%s\"",
                 field->name, inner->name, so_far, inner->nfields ? inner->fields[0].name : "");
            goto bad;
        }
        case UAS_T_BUILTIN:
            plan->leaf = (uas_leaf_t){.builtin = field->builtin};
            break;
        case UAS_T_ENUM:
            plan->leaf = (uas_leaf_t){.builtin = 0, .enum_name = field->type_name};
            break;
        }
        if (uas_check_datatype(&plan->leaf, dt, subject, err, errlen) != 0)
            goto bad;
        plan->datatype = dt;
        return 0;
    }
bad:
    uas_plan_free(plan);
    return -1;
}

/* ---- the reader ------------------------------------------------------------- */

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
    const char *ctx; /* the field being read, for messages */
    char *err;
    size_t errlen;
} reader_t;

static int take(reader_t *r, size_t n, const uint8_t **out) {
    if (n > r->len - r->pos) {
        fail(r->err, r->errlen, "structure body truncated while reading \"%s\"", r->ctx);
        return -1;
    }
    if (out)
        *out = r->buf + r->pos;
    r->pos += n;
    return 0;
}

static int rd_u8(reader_t *r, uint8_t *v) {
    const uint8_t *p;
    if (take(r, 1, &p))
        return -1;
    *v = p[0];
    return 0;
}

static int rd_u16(reader_t *r, uint16_t *v) {
    const uint8_t *p;
    if (take(r, 2, &p))
        return -1;
    *v = (uint16_t)(p[0] | p[1] << 8);
    return 0;
}

static int rd_u32(reader_t *r, uint32_t *v) {
    const uint8_t *p;
    if (take(r, 4, &p))
        return -1;
    *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 0;
}

static int rd_u64(reader_t *r, uint64_t *v) {
    uint32_t lo, hi;
    if (rd_u32(r, &lo) || rd_u32(r, &hi))
        return -1;
    *v = (uint64_t)hi << 32 | lo;
    return 0;
}

/* An Int32 length prefix: -1 is null (no elements). */
static int rd_len(reader_t *r, size_t *n) {
    uint32_t u;
    if (rd_u32(r, &u))
        return -1;
    int32_t i = (int32_t)u;
    if (i == -1) {
        *n = 0;
        return 0;
    }
    if (i < 0) {
        fail(r->err, r->errlen, "invalid length %d while reading \"%s\"", (int)i, r->ctx);
        return -1;
    }
    *n = (size_t)i;
    return 0;
}

/* A length-prefixed byte run; *null for the null encoding. */
static int rd_bytes(reader_t *r, const uint8_t **p, size_t *n, bool *null) {
    uint32_t u;
    if (rd_u32(r, &u))
        return -1;
    int32_t i = (int32_t)u;
    *null = i == -1;
    *p = NULL;
    *n = 0;
    if (i == -1)
        return 0;
    if (i < 0) {
        fail(r->err, r->errlen, "invalid length %d while reading \"%s\"", (int)i, r->ctx);
        return -1;
    }
    *n = (size_t)i;
    return take(r, *n, p);
}

/* Strict UTF-8, as Rust's String::from_utf8: no overlongs, no surrogates,
 * nothing past U+10FFFF. */
static bool utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t k;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) {
            k = 1, cp = c & 0x1F, min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            k = 2, cp = c & 0x0F, min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            k = 3, cp = c & 0x07, min = 0x10000;
        } else {
            return false;
        }
        if (n - i <= k)
            return false;
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80)
                return false;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += k + 1;
    }
    return true;
}

static int rd_string(reader_t *r, const uint8_t **p, size_t *n, bool *null) {
    if (rd_bytes(r, p, n, null))
        return -1;
    if (*n && !utf8_valid(*p, *n)) {
        fail(r->err, r->errlen, "invalid UTF-8 while reading \"%s\"", r->ctx);
        return -1;
    }
    return 0;
}

static int deeper(reader_t *r, int *depth) {
    if (*depth >= MAX_DEPTH) {
        fail(r->err, r->errlen, "nesting too deep while reading \"%s\"", r->ctx);
        return -1;
    }
    (*depth)++;
    return 0;
}

static int bad_encoding(reader_t *r, const char *what, uint8_t byte) {
    fail(r->err, r->errlen, "invalid %s encoding 0x%02x while reading \"%s\"", what, byte,
         r->ctx);
    return -1;
}

/* A NodeId as read, borrowing from the body. */
typedef struct {
    uint16_t ns;
    char type; /* 'i', 's', 'g', 'b' */
    uint32_t num;
    const uint8_t *data; /* s, b; NULL when null */
    size_t dlen;
    const uint8_t *guid; /* 16 bytes */
} nodeid_t;

/* The NodeId after its encoding byte (only the low 6 bits select the form). */
static int node_id_body(reader_t *r, uint8_t enc, nodeid_t *id) {
    memset(id, 0, sizeof *id);
    uint8_t b;
    uint16_t w;
    bool null;
    switch (enc & 0x3F) {
    case 0:
        if (rd_u8(r, &b))
            return -1;
        id->type = 'i', id->num = b;
        return 0;
    case 1:
        if (rd_u8(r, &b) || rd_u16(r, &w))
            return -1;
        id->ns = b, id->type = 'i', id->num = w;
        return 0;
    case 2:
        id->type = 'i';
        return rd_u16(r, &id->ns) || rd_u32(r, &id->num) ? -1 : 0;
    case 3:
        id->type = 's';
        return rd_u16(r, &id->ns) || rd_string(r, &id->data, &id->dlen, &null) ? -1 : 0;
    case 4:
        id->type = 'g';
        return rd_u16(r, &id->ns) || take(r, 16, &id->guid) ? -1 : 0;
    case 5:
        id->type = 'b';
        return rd_u16(r, &id->ns) || rd_bytes(r, &id->data, &id->dlen, &null) ? -1 : 0;
    default:
        return bad_encoding(r, "NodeId", enc);
    }
}

typedef struct {
    nodeid_t node;
    const uint8_t *uri; /* NULL: none */
    size_t urilen;
    uint32_t server;
} xnodeid_t;

static int expanded_node_id(reader_t *r, xnodeid_t *x) {
    uint8_t enc;
    bool null;
    memset(x, 0, sizeof *x);
    if (rd_u8(r, &enc) || node_id_body(r, enc, &x->node))
        return -1;
    if (enc & 0x80) {
        if (rd_string(r, &x->uri, &x->urilen, &null))
            return -1;
        if (null)
            x->uri = NULL;
        else if (!x->uri)
            x->uri = (const uint8_t *)""; /* empty, not absent */
    }
    if ((enc & 0x40) && rd_u32(r, &x->server))
        return -1;
    return 0;
}

static int skip_builtin(reader_t *r, int b, int depth);

static int skip_field(reader_t *r, const uas_types_t *t, const uas_field_t *f, int depth);

static int skip_value(reader_t *r, const uas_types_t *t, const uas_field_t *f, int depth) {
    switch (f->kind) {
    case UAS_T_BUILTIN: return skip_builtin(r, f->builtin, depth);
    case UAS_T_ENUM: return take(r, 4, NULL);
    default: break;
    }
    if (deeper(r, &depth))
        return -1;
    if (f->ref >= t->n) {
        fail(r->err, r->errlen, "no definition for structure type %zu", f->ref);
        return -1;
    }
    const uas_struct_t *def = &t->structs[f->ref];
    uint32_t mask, sw;
    switch (def->kind) {
    case UAS_STRUCTURE:
        for (size_t j = 0; j < def->nfields; j++)
            if (skip_field(r, t, &def->fields[j], depth))
                return -1;
        return 0;
    case UAS_OPTIONAL_FIELDS: {
        if (rd_u32(r, &mask))
            return -1;
        unsigned bit = 0;
        for (size_t j = 0; j < def->nfields; j++) {
            const uas_field_t *g = &def->fields[j];
            bool present = !g->optional || (bit < 32 && (mask & (1u << bit)));
            if (g->optional)
                bit++;
            if (present && skip_field(r, t, g, depth))
                return -1;
        }
        return 0;
    }
    case UAS_UNION:
        if (rd_u32(r, &sw))
            return -1;
        if (sw == 0)
            return 0;
        if (sw <= def->nfields)
            return skip_field(r, t, &def->fields[sw - 1], depth);
        fail(r->err, r->errlen, "invalid union switch %u in %s", (unsigned)sw, def->name);
        return -1;
    }
    return 0;
}

/* Skip `count` array elements. A structure without fields encodes to nothing,
 * so once an element consumed no bytes every remaining one is the same: stop,
 * rather than loop up to 2^31 times on a forged length. */
static int skip_elements(reader_t *r, const uas_types_t *t, const uas_field_t *f, size_t count,
                         int depth) {
    for (size_t i = 0; i < count; i++) {
        size_t before = r->pos;
        if (skip_value(r, t, f, depth))
            return -1;
        if (r->pos == before)
            break;
    }
    return 0;
}

static int skip_field(reader_t *r, const uas_types_t *t, const uas_field_t *f, int depth) {
    if (f->array) {
        size_t len;
        if (rd_len(r, &len))
            return -1;
        return skip_elements(r, t, f, len, depth);
    }
    return skip_value(r, t, f, depth);
}

static int skip_builtin(reader_t *r, int b, int depth) {
    const uint8_t *p;
    size_t n;
    bool null;
    uint8_t m;
    switch (b) {
    case UAS_BOOLEAN:
    case UAS_SBYTE:
    case UAS_BYTE: return take(r, 1, NULL);
    case UAS_INT16:
    case UAS_UINT16: return take(r, 2, NULL);
    case UAS_INT32:
    case UAS_UINT32:
    case UAS_FLOAT:
    case UAS_STATUSCODE: return take(r, 4, NULL);
    case UAS_INT64:
    case UAS_UINT64:
    case UAS_DOUBLE:
    case UAS_DATETIME: return take(r, 8, NULL);
    case UAS_GUID: return take(r, 16, NULL);
    case UAS_STRING:
    case UAS_BYTESTRING:
    case UAS_XMLELEMENT: return rd_bytes(r, &p, &n, &null);
    case UAS_NODEID: {
        nodeid_t id;
        if (rd_u8(r, &m))
            return -1;
        if (m & 0xC0)
            return bad_encoding(r, "NodeId", m);
        return node_id_body(r, m, &id);
    }
    case UAS_EXPANDEDNODEID: {
        xnodeid_t x;
        return expanded_node_id(r, &x);
    }
    case UAS_QUALIFIEDNAME:
        if (take(r, 2, NULL))
            return -1;
        return rd_bytes(r, &p, &n, &null);
    case UAS_LOCALIZEDTEXT:
        if (rd_u8(r, &m))
            return -1;
        if ((m & 0x01) && rd_bytes(r, &p, &n, &null))
            return -1;
        if ((m & 0x02) && rd_bytes(r, &p, &n, &null))
            return -1;
        return 0;
    case UAS_EXTENSIONOBJECT:
        if (skip_builtin(r, UAS_NODEID, depth) || rd_u8(r, &m))
            return -1;
        if (m == 0)
            return 0;
        if (m == 1 || m == 2)
            return rd_bytes(r, &p, &n, &null);
        return bad_encoding(r, "ExtensionObject", m);
    case UAS_DATAVALUE:
        if (deeper(r, &depth) || rd_u8(r, &m))
            return -1;
        if ((m & 0x01) && skip_builtin(r, UAS_VARIANT, depth))
            return -1;
        if ((m & 0x02) && take(r, 4, NULL))
            return -1;
        if ((m & 0x04) && take(r, 8, NULL))
            return -1;
        if ((m & 0x10) && take(r, 2, NULL))
            return -1;
        if ((m & 0x08) && take(r, 8, NULL))
            return -1;
        if ((m & 0x20) && take(r, 2, NULL))
            return -1;
        return 0;
    case UAS_VARIANT: {
        if (deeper(r, &depth) || rd_u8(r, &m))
            return -1;
        int id = m & 0x3F;
        if (id == 0)
            return 0;
        if (!uas_builtin_name(id))
            return bad_encoding(r, "Variant", m);
        if (!(m & 0x80))
            return skip_builtin(r, id, depth);
        size_t len;
        if (rd_len(r, &len))
            return -1;
        for (size_t i = 0; i < len; i++)
            if (skip_builtin(r, id, depth))
                return -1;
        if (m & 0x40) {
            if (rd_len(r, &len))
                return -1;
            for (size_t i = 0; i < len; i++)
                if (take(r, 4, NULL))
                    return -1;
        }
        return 0;
    }
    case UAS_DIAGNOSTICINFO:
        if (deeper(r, &depth) || rd_u8(r, &m))
            return -1;
        for (unsigned bit = 0x01; bit <= 0x08; bit <<= 1)
            if ((m & bit) && take(r, 4, NULL))
                return -1;
        if ((m & 0x10) && rd_bytes(r, &p, &n, &null))
            return -1;
        if ((m & 0x20) && take(r, 4, NULL))
            return -1;
        if ((m & 0x40) && skip_builtin(r, UAS_DIAGNOSTICINFO, depth))
            return -1;
        return 0;
    default:
        fail(r->err, r->errlen, "unknown built-in type %d", b);
        return -1;
    }
}

/* ---- rendering -------------------------------------------------------------- */

static void be_bytes(uint64_t v, size_t n, uas_out_t *out) {
    for (size_t i = 0; i < n; i++)
        out->raw[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    out->raw_len = n;
}

static void set_num(uas_out_t *out, double v) {
    out->val.kind = TDOT_VAL_NUM;
    out->val.num = v;
}

/* Text: the value cut to UAS_TEXT_MAX bytes at a character boundary (C5),
 * `raw` its UTF-8 bytes up to TDOT_RAW_MAX. */
static void set_text(uas_out_t *out, const char *s, size_t n) {
    out->val.kind = TDOT_VAL_STR;
    size_t vn = n;
    if (vn > UAS_TEXT_MAX) {
        vn = UAS_TEXT_MAX;
        while (vn > 0 && ((uint8_t)s[vn] & 0xC0) == 0x80)
            vn--;
    }
    if (vn)
        memcpy(out->val.str, s, vn);
    out->val.str[vn] = '\0';
    size_t rn = n < TDOT_RAW_MAX ? n : TDOT_RAW_MAX;
    if (rn)
        memcpy(out->raw, s, rn);
    out->raw_len = rn;
}

static void set_i64(uas_out_t *out, int64_t v) {
    if (v > TDOT_JS_SAFE_MAX || v < -TDOT_JS_SAFE_MAX) {
        out->val.kind = TDOT_VAL_STR;
        snprintf(out->val.str, sizeof out->val.str, "%lld", (long long)v);
    } else {
        set_num(out, (double)v);
    }
}

static int64_t div_floor(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int64_t mod_floor(int64_t a, int64_t b) {
    return a - div_floor(a, b) * b;
}

/* RFC 3339 UTC with up to 7 fraction digits, trailing zeros trimmed; `ticks`
 * clamped. H. Hinnant's civil_from_days. */
static void datetime_text(int64_t ticks, char *out, size_t outlen) {
    int64_t secs = div_floor(ticks, 10000000) - UNIX_EPOCH_TICKS / 10000000;
    int64_t frac = mod_floor(ticks, 10000000);
    int64_t z = div_floor(secs, 86400) + 719468;
    int64_t era = div_floor(z, 146097);
    int64_t doe = mod_floor(z, 146097);
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    int64_t sod = mod_floor(secs, 86400);
    int len = snprintf(out, outlen, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld", (long long)y,
                       (long long)m, (long long)d, (long long)(sod / 3600),
                       (long long)(sod / 60 % 60), (long long)(sod % 60));
    if (frac != 0) {
        char digits[8];
        snprintf(digits, sizeof digits, "%07lld", (long long)frac);
        size_t n = 7;
        while (n > 0 && digits[n - 1] == '0')
            n--;
        digits[n] = '\0';
        len += snprintf(out + len, outlen - (size_t)len, ".%s", digits);
    }
    snprintf(out + len, outlen - (size_t)len, "Z");
}

/* The symbolic name of a status code, or 0x and 8 hex digits when it has
 * none. A code with any of the low 16 (info) bits set has no name. */
static void status_code_text(uint32_t code, char *out, size_t outlen) {
    if ((code & 0xFFFF) == 0) {
        const char *name = UA_StatusCode_name(code);
        if (name && *name && strcmp(name, "Unknown StatusCode") != 0) {
            snprintf(out, outlen, "%s", name);
            return;
        }
    }
    snprintf(out, outlen, "0x%08X", (unsigned)code);
}

static void guid_text(const uint8_t *g, char *out, size_t outlen) {
    snprintf(out, outlen, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned)(g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24),
             (unsigned)(g[4] | g[5] << 8), (unsigned)(g[6] | g[7] << 8), g[8], g[9], g[10],
             g[11], g[12], g[13], g[14], g[15]);
}

/* Appends to out (NUL-terminated, outlen total), never past it. */
static void append(char *out, size_t outlen, const char *s, size_t n) {
    size_t len = strlen(out);
    if (len + 1 >= outlen)
        return;
    if (n > outlen - 1 - len)
        n = outlen - 1 - len;
    memcpy(out + len, s, n);
    out[len + n] = '\0';
}

static void base64(const uint8_t *b, size_t n, char *out, size_t outlen) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        size_t k = n - i < 3 ? n - i : 3;
        if (k > 1)
            v |= (uint32_t)b[i + 1] << 8;
        if (k > 2)
            v |= b[i + 2];
        char q[4];
        for (size_t j = 0; j < 4; j++)
            q[j] = j <= k ? A[(v >> (18 - 6 * j)) & 0x3F] : '=';
        append(out, outlen, q, 4);
    }
}

static void identifier_text(const nodeid_t *id, char *out, size_t outlen) {
    char buf[64];
    switch (id->type) {
    case 'i':
        snprintf(buf, sizeof buf, "i=%u", (unsigned)id->num);
        append(out, outlen, buf, strlen(buf));
        break;
    case 's':
        append(out, outlen, "s=", 2);
        append(out, outlen, (const char *)id->data, id->dlen);
        break;
    case 'g':
        append(out, outlen, "g=", 2);
        guid_text(id->guid, buf, sizeof buf);
        append(out, outlen, buf, strlen(buf));
        break;
    default:
        append(out, outlen, "b=", 2);
        base64(id->data, id->dlen, out, outlen);
        break;
    }
}

static void ns_prefix(unsigned ns, const char *const *nsa, size_t nns, char *out, size_t outlen) {
    if (ns == 0)
        return;
    if (ns < nns && nsa[ns]) {
        append(out, outlen, "nsu=", 4);
        append(out, outlen, nsa[ns], strlen(nsa[ns]));
        append(out, outlen, ";", 1);
    } else {
        char buf[16];
        snprintf(buf, sizeof buf, "ns=%u;", ns);
        append(out, outlen, buf, strlen(buf));
    }
}

void uas_node_text(unsigned ns_index, const char *ident, const char *const *ns, size_t nns,
                   char *out, size_t outlen) {
    out[0] = '\0';
    ns_prefix(ns_index, ns, nns, out, outlen);
    append(out, outlen, ident, strlen(ident));
}

/* Big enough for any text a value holds plus what raw echoes. */
#define TEXT_BUF 1024

/* Read one value of the leaf's type and render it as `dt`. */
static int read_scalar(reader_t *r, const uas_leaf_t *leaf, tdot_datatype_t dt,
                       const char *const *ns, size_t nns, uas_out_t *out) {
    memset(out, 0, sizeof *out);
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    const uint8_t *p;
    size_t n;
    bool null;
    char text[TEXT_BUF];
    if (!leaf->builtin) {
        if (rd_u32(r, &u32))
            return -1;
        set_num(out, (int32_t)u32);
        be_bytes(u32, 4, out);
        return 0;
    }
    switch (leaf->builtin) {
    case UAS_BOOLEAN:
        if (rd_u8(r, &u8))
            return -1;
        out->val.kind = TDOT_VAL_BOOL;
        out->val.b = u8 != 0;
        out->raw[0] = u8 != 0;
        out->raw_len = 1;
        return 0;
    case UAS_SBYTE:
        if (rd_u8(r, &u8))
            return -1;
        set_num(out, (int8_t)u8);
        be_bytes(u8, 1, out);
        return 0;
    case UAS_BYTE:
        if (rd_u8(r, &u8))
            return -1;
        set_num(out, u8);
        be_bytes(u8, 1, out);
        return 0;
    case UAS_INT16:
        if (rd_u16(r, &u16))
            return -1;
        set_num(out, (int16_t)u16);
        be_bytes(u16, 2, out);
        return 0;
    case UAS_UINT16:
        if (rd_u16(r, &u16))
            return -1;
        set_num(out, u16);
        be_bytes(u16, 2, out);
        return 0;
    case UAS_INT32:
        if (rd_u32(r, &u32))
            return -1;
        set_num(out, (int32_t)u32);
        be_bytes(u32, 4, out);
        return 0;
    case UAS_UINT32:
        if (rd_u32(r, &u32))
            return -1;
        set_num(out, u32);
        be_bytes(u32, 4, out);
        return 0;
    case UAS_INT64:
        if (rd_u64(r, &u64))
            return -1;
        set_i64(out, (int64_t)u64);
        be_bytes(u64, 8, out);
        return 0;
    case UAS_UINT64:
        if (rd_u64(r, &u64))
            return -1;
        if (u64 > (uint64_t)TDOT_JS_SAFE_MAX) {
            out->val.kind = TDOT_VAL_STR;
            snprintf(out->val.str, sizeof out->val.str, "%llu", (unsigned long long)u64);
        } else {
            set_num(out, (double)u64);
        }
        be_bytes(u64, 8, out);
        return 0;
    case UAS_FLOAT: {
        if (rd_u32(r, &u32))
            return -1;
        float f;
        memcpy(&f, &u32, 4);
        set_num(out, (double)f);
        be_bytes(u32, 4, out);
        return 0;
    }
    case UAS_DOUBLE: {
        if (rd_u64(r, &u64))
            return -1;
        double d;
        memcpy(&d, &u64, 8);
        set_num(out, d);
        be_bytes(u64, 8, out);
        return 0;
    }
    case UAS_STRING:
        if (rd_string(r, &p, &n, &null))
            return -1;
        set_text(out, (const char *)p, n);
        return 0;
    case UAS_LOCALIZEDTEXT:
        if (rd_u8(r, &u8))
            return -1;
        if ((u8 & 0x01) && rd_string(r, &p, &n, &null))
            return -1;
        p = NULL, n = 0;
        if ((u8 & 0x02) && rd_string(r, &p, &n, &null))
            return -1;
        set_text(out, (const char *)p, n);
        return 0;
    case UAS_DATETIME: {
        if (rd_u64(r, &u64))
            return -1;
        int64_t ticks = (int64_t)u64;
        if (ticks < 0)
            ticks = 0;
        if (ticks > MAX_TICKS)
            ticks = MAX_TICKS;
        if (dt == TDOT_DT_INT64) {
            set_i64(out, div_floor(ticks - UNIX_EPOCH_TICKS, 10000));
            be_bytes((uint64_t)ticks, 8, out);
        } else {
            datetime_text(ticks, text, sizeof text);
            set_text(out, text, strlen(text));
        }
        return 0;
    }
    case UAS_STATUSCODE:
        if (rd_u32(r, &u32))
            return -1;
        if (dt == TDOT_DT_UINT32) {
            set_num(out, u32);
            be_bytes(u32, 4, out);
        } else {
            status_code_text(u32, text, sizeof text);
            set_text(out, text, strlen(text));
        }
        return 0;
    case UAS_GUID:
        if (take(r, 16, &p))
            return -1;
        guid_text(p, text, sizeof text);
        set_text(out, text, strlen(text));
        return 0;
    case UAS_BYTESTRING: {
        if (rd_bytes(r, &p, &n, &null))
            return -1;
        if (n > UAS_BYTES_MAX) {
            /* Refused rather than cut: a truncated hex run reads as a
             * different, valid value (C5, as a top-level ByteString). */
            fail(r->err, r->errlen,
                 "ByteString of %zu bytes is longer than a bytes value holds in the C build "
                 "(%zu bytes)",
                 n, (size_t)UAS_BYTES_MAX);
            return -1;
        }
        static const char HEX[] = "0123456789abcdef";
        out->val.kind = TDOT_VAL_STR;
        for (size_t i = 0; i < n; i++) {
            out->val.str[2 * i] = HEX[p[i] >> 4];
            out->val.str[2 * i + 1] = HEX[p[i] & 0x0f];
        }
        out->val.str[2 * n] = '\0';
        if (n)
            memcpy(out->raw, p, n);
        out->raw_len = n;
        return 0;
    }
    case UAS_NODEID: {
        nodeid_t id;
        if (rd_u8(r, &u8))
            return -1;
        if (u8 & 0xC0)
            return bad_encoding(r, "NodeId", u8);
        if (node_id_body(r, u8, &id))
            return -1;
        text[0] = '\0';
        ns_prefix(id.ns, ns, nns, text, sizeof text);
        identifier_text(&id, text, sizeof text);
        set_text(out, text, strlen(text));
        return 0;
    }
    case UAS_EXPANDEDNODEID: {
        xnodeid_t x;
        if (expanded_node_id(r, &x))
            return -1;
        text[0] = '\0';
        if (x.server != 0) {
            char buf[24];
            snprintf(buf, sizeof buf, "svr=%u;", (unsigned)x.server);
            append(text, sizeof text, buf, strlen(buf));
        }
        if (x.uri) {
            append(text, sizeof text, "nsu=", 4);
            append(text, sizeof text, (const char *)x.uri, x.urilen);
            append(text, sizeof text, ";", 1);
        } else {
            ns_prefix(x.node.ns, ns, nns, text, sizeof text);
        }
        identifier_text(&x.node, text, sizeof text);
        set_text(out, text, strlen(text));
        return 0;
    }
    case UAS_QUALIFIEDNAME:
        if (rd_u16(r, &u16) || rd_string(r, &p, &n, &null))
            return -1;
        text[0] = '\0';
        if (u16 != 0)
            snprintf(text, sizeof text, "%u:", (unsigned)u16);
        append(text, sizeof text, (const char *)p, n);
        set_text(out, text, strlen(text));
        return 0;
    default:
        fail(r->err, r->errlen, "%s cannot be read as a point", uas_builtin_name(leaf->builtin));
        return -1;
    }
}

/* ---- decode ----------------------------------------------------------------- */

int uas_decode(const uas_plan_t *plan, const uas_types_t *t, const uint8_t *body,
               size_t len, const char *const *ns, size_t nns, uas_out_t *out,
               char *err, size_t errlen) {
    reader_t r = {.buf = body, .len = len, .ctx = "", .err = err, .errlen = errlen};
    for (size_t k = 0; k < plan->n; k++) {
        const uas_step_t *step = &plan->steps[k];
        const uas_struct_t *def = &t->structs[step->s];
        const uas_field_t *field = &def->fields[step->field];
        r.ctx = field->name;
        uint32_t mask, sw;
        switch (def->kind) {
        case UAS_STRUCTURE:
            for (size_t j = 0; j < step->field; j++) {
                r.ctx = def->fields[j].name;
                if (skip_field(&r, t, &def->fields[j], 0))
                    return -1;
            }
            break;
        case UAS_OPTIONAL_FIELDS: {
            if (rd_u32(&r, &mask))
                return -1;
            unsigned bit = 0;
            for (size_t j = 0; j < step->field; j++) {
                const uas_field_t *g = &def->fields[j];
                bool present = !g->optional || (bit < 32 && (mask & (1u << bit)));
                if (g->optional)
                    bit++;
                if (present) {
                    r.ctx = g->name;
                    if (skip_field(&r, t, g, 0))
                        return -1;
                }
            }
            if (field->optional && !(bit < 32 && (mask & (1u << bit)))) {
                fail(err, errlen, "optional field \"%s\" is absent", field->name);
                return -1;
            }
            break;
        }
        case UAS_UNION:
            if (rd_u32(&r, &sw))
                return -1;
            if (sw > def->nfields) {
                fail(err, errlen, "invalid union switch %u in %s", (unsigned)sw, def->name);
                return -1;
            }
            if (sw != step->field + 1) {
                if (sw == 0)
                    fail(err, errlen, "union field \"%s\" is not set (active: none)",
                         field->name);
                else
                    fail(err, errlen, "union field \"%s\" is not set (active: \"%s\")",
                         field->name, def->fields[sw - 1].name);
                return -1;
            }
            break;
        }
        r.ctx = field->name;
        if (step->index >= 0) {
            size_t n;
            if (rd_len(&r, &n))
                return -1;
            if ((size_t)step->index >= n) {
                fail(err, errlen, "index %" PRId64 " out of range for \"%s\" (length %zu)", step->index,
                     field->name, n);
                return -1;
            }
            if (skip_elements(&r, t, field, (size_t)step->index, 0))
                return -1;
        }
        if (k + 1 == plan->n)
            return read_scalar(&r, &plan->leaf, plan->datatype, ns, nns, out);
    }
    fail(err, errlen, "field path is empty");
    return -1;
}

int uas_decode_builtin(int b, const uint8_t *body, size_t len, tdot_datatype_t dt,
                       const char *const *ns, size_t nns, uas_out_t *out, char *err,
                       size_t errlen) {
    uas_leaf_t leaf = {.builtin = b};
    if (!uas_builtin_name(b)) {
        fail(err, errlen, "unknown built-in type %d", b);
        return -1;
    }
    if (uas_check_datatype(&leaf, dt, "value", err, errlen) != 0)
        return -1;
    reader_t r = {.buf = body, .len = len, .ctx = "value", .err = err, .errlen = errlen};
    return read_scalar(&r, &leaf, dt, ns, nns, out);
}
