/* The OPC UA structure walker (connectors/opcua/structure.c) over the shared
 * vectors in doc/contract/test-vectors/opcua-struct/vectors.json, which the
 * Rust connector's structure.rs runs too: every value, `raw` and message must
 * match. Plus the Rust module's own walker tests and the C build's size
 * limits (openspec change opcua-structures-c, design C5).
 *
 * Usage: tedge-dot-opcua-struct <vectors.json>
 */
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "opcua_struct_vectors.h"
#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"

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

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = '\0';
}

typedef struct {
    const cJSON *types_json;
    uas_types_t types;
    const char *ns[16];
    size_t nns;
} ctx_t;

/* Decode `body` as a vector describes: 0 with *out, or -1 with err. */
static int decode(const ctx_t *c, const char *root, const char *path_text, tdot_datatype_t dt,
                  const uint8_t *body, size_t len, uas_out_t *out, char *err, size_t errlen) {
    int b = uas_builtin_from_name(root);
    if (b)
        return uas_decode_builtin(b, body, len, dt, c->ns, c->nns, out, err, errlen);
    uas_path_t path;
    if (uas_parse_path(path_text, &path, err, errlen))
        return -1;
    uas_plan_t plan;
    int rc = -1;
    if (uas_compile(&c->types, vec_struct_index(c->types_json, root), &path, dt, &plan, err,
                    errlen) == 0) {
        rc = uas_decode(&plan, &c->types, body, len, c->ns, c->nns, out, err, errlen);
        uas_plan_free(&plan);
    }
    uas_path_free(&path);
    return rc;
}

static void check_read_vectors(const ctx_t *c, const cJSON *root) {
    const cJSON *cases = cJSON_GetObjectItem(root, "read");
    int count = cJSON_GetArraySize(cases);
    CHECK(count >= 70, "only %d read vectors", count);
    const cJSON *v;
    cJSON_ArrayForEach(v, cases) {
        const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
        static uint8_t body[4096];
        size_t len = vec_unhex(cJSON_GetObjectItem(v, "body")->valuestring, body, sizeof body);
        const cJSON *path = cJSON_GetObjectItem(v, "path");
        tdot_datatype_t dt =
            tdot_datatype_parse(cJSON_GetObjectItem(v, "datatype")->valuestring);
        uas_out_t out;
        char err[512] = "";
        int rc = decode(c, cJSON_GetObjectItem(v, "root")->valuestring,
                        path ? path->valuestring : "", dt, body, len, &out, err, sizeof err);
        const cJSON *want_err = cJSON_GetObjectItem(v, "error");
        if (want_err) {
            CHECK(rc != 0 && !strcmp(err, want_err->valuestring),
                  "%s: got rc %d error \"%s\", want \"%s\"", name, rc, err,
                  want_err->valuestring);
            continue;
        }
        if (rc != 0) {
            CHECK(0, "%s: error \"%s\"", name, err);
            continue;
        }
        const cJSON *o = cJSON_GetObjectItem(v, "out")->child;
        bool ok = false;
        if (!strcmp(o->string, "num"))
            ok = out.val.kind == TDOT_VAL_NUM && out.val.num == o->valuedouble;
        else if (!strcmp(o->string, "bool"))
            ok = out.val.kind == TDOT_VAL_BOOL && out.val.b == cJSON_IsTrue(o);
        else
            ok = out.val.kind == TDOT_VAL_STR && !strcmp(out.val.str, o->valuestring);
        CHECK(ok, "%s: value kind %d num %.17g str \"%s\"", name, out.val.kind, out.val.num,
              out.val.str);
        char raw[2 * TDOT_RAW_MAX + 1];
        hex(out.raw, out.raw_len, raw);
        const char *want_raw = cJSON_GetObjectItem(v, "raw")->valuestring;
        CHECK(!strcmp(raw, want_raw), "%s: raw %s, want %s", name, raw, want_raw);
    }
}

static void check_invalid_paths(const cJSON *root) {
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(root, "invalid_paths")) {
        const char *p = cJSON_GetObjectItem(v, "path")->valuestring;
        const char *want = cJSON_GetObjectItem(v, "error")->valuestring;
        uas_path_t path;
        char err[256] = "";
        int rc = uas_parse_path(p, &path, err, sizeof err);
        CHECK(rc != 0 && !strcmp(err, want), "path \"%s\": rc %d \"%s\", want \"%s\"", p, rc,
              err, want);
        if (rc == 0)
            uas_path_free(&path);
    }
}

static void check_paths_parse(void) {
    uas_path_t p;
    char err[128];
    CHECK(uas_parse_path("Items[12].Value", &p, err, sizeof err) == 0, "%s", err);
    CHECK(p.n == 2 && !strcmp(p.segs[0].name, "Items") && p.segs[0].index == 12 &&
              !strcmp(p.segs[1].name, "Value") && p.segs[1].index == -1,
          "Items[12].Value parsed wrong");
    char text[64];
    uas_path_text(&p, text, sizeof text);
    CHECK(!strcmp(text, "Items[12].Value"), "path text %s", text);
    uas_path_free(&p);
    /* The largest u32 stays an index, also where `long` is 32 bits (armhf). */
    CHECK(uas_parse_path("A[4294967295]", &p, err, sizeof err) == 0, "%s", err);
    CHECK(p.n == 1 && p.segs[0].index == 4294967295LL, "A[4294967295] parsed wrong");
    uas_path_text(&p, text, sizeof text);
    CHECK(!strcmp(text, "A[4294967295]"), "path text %s", text);
    uas_path_free(&p);
    /* An index beyond u32, as Rust's parse::<u32>() refuses it. */
    CHECK(uas_parse_path("A[4294967296]", &p, err, sizeof err) != 0 &&
              !strcmp(err, "field path \"A[4294967296]\" has an invalid index"),
          "%s", err);
}

/* A structure without fields encodes to nothing: a forged array length of
 * 2^31-1 must not loop that often. */
static void check_empty_structure_arrays(void) {
    uas_field_t holder_fields[2] = {
        {.name = "Many", .kind = UAS_T_STRUCT, .ref = 1, .array = true},
        {.name = "Tail", .kind = UAS_T_BUILTIN, .builtin = UAS_BYTE},
    };
    uas_struct_t structs[2] = {
        {.name = "Holder", .kind = UAS_STRUCTURE, .fields = holder_fields, .nfields = 2},
        {.name = "Empty", .kind = UAS_STRUCTURE, .fields = NULL, .nfields = 0},
    };
    uas_types_t t = {.structs = structs, .n = 2};
    uas_path_t path;
    uas_plan_t plan;
    char err[256] = "";
    uas_parse_path("Tail", &path, err, sizeof err);
    CHECK(uas_compile(&t, 0, &path, TDOT_DT_UINT8, &plan, err, sizeof err) == 0, "%s", err);
    uint8_t body[] = {0xff, 0xff, 0xff, 0x7f, 7};
    uas_out_t out;
    clock_t start = clock();
    CHECK(uas_decode(&plan, &t, body, sizeof body, NULL, 0, &out, err, sizeof err) == 0 &&
              out.val.num == 7,
          "empty structure array: %s", err);
    CHECK(clock() - start < CLOCKS_PER_SEC, "empty structure array took too long");
    uas_plan_free(&plan);
    uas_path_free(&path);
}

/* A Variant holding a Variant array holding ... deeper than the limit. */
static void check_deep_nesting(void) {
    uas_field_t fields[2] = {
        {.name = "Var", .kind = UAS_T_BUILTIN, .builtin = UAS_VARIANT},
        {.name = "Tail", .kind = UAS_T_BUILTIN, .builtin = UAS_BYTE},
    };
    uas_struct_t s = {.name = "Deep", .kind = UAS_STRUCTURE, .fields = fields, .nfields = 2};
    uas_types_t t = {.structs = &s, .n = 1};
    uint8_t body[34 * 5 + 2];
    size_t n = 0;
    for (int i = 0; i < 34; i++) {
        body[n++] = 0x80 | UAS_VARIANT;
        body[n++] = 1, body[n++] = 0, body[n++] = 0, body[n++] = 0;
    }
    body[n++] = 0;
    body[n++] = 7;
    uas_path_t path;
    uas_plan_t plan;
    char err[256] = "";
    uas_parse_path("Tail", &path, err, sizeof err);
    uas_compile(&t, 0, &path, TDOT_DT_UINT8, &plan, err, sizeof err);
    uas_out_t out;
    CHECK(uas_decode(&plan, &t, body, n, NULL, 0, &out, err, sizeof err) != 0 &&
              !strcmp(err, "nesting too deep while reading \"Var\""),
          "deep nesting: %s", err);
    uas_plan_free(&plan);
    uas_path_free(&path);
}

/* The C build's fixed-size values (design C5). */
static void check_limits(void) {
    uas_out_t out;
    char err[256] = "";
    /* A 300-character string: the value cut at 255 bytes, raw at 256. */
    uint8_t body[4 + 300];
    body[0] = 300 & 0xff, body[1] = 300 >> 8, body[2] = body[3] = 0;
    memset(body + 4, 'a', 300);
    body[4 + 254] = 0xc3, body[4 + 255] = 0xa4; /* "ä" across the cut */
    CHECK(uas_decode_builtin(UAS_STRING, body, sizeof body, TDOT_DT_STRING, NULL, 0, &out, err,
                             sizeof err) == 0,
          "long string: %s", err);
    CHECK(strlen(out.val.str) == 254 && out.raw_len == TDOT_RAW_MAX,
          "long string: value %zu bytes, raw %zu", strlen(out.val.str), out.raw_len);
    /* A ByteString of 128 bytes is refused, 127 is fine. */
    body[0] = 128, body[1] = 0;
    CHECK(uas_decode_builtin(UAS_BYTESTRING, body, 4 + 128, TDOT_DT_BYTES, NULL, 0, &out, err,
                             sizeof err) != 0 &&
              !strcmp(err, "ByteString of 128 bytes is longer than a bytes value holds in "
                           "the C build (127 bytes)"),
          "long bytes: %s", err);
    body[0] = 127;
    CHECK(uas_decode_builtin(UAS_BYTESTRING, body, 4 + 127, TDOT_DT_BYTES, NULL, 0, &out, err,
                             sizeof err) == 0 &&
              strlen(out.val.str) == 254 && out.raw_len == 127,
          "127 bytes: %s", err);
    /* Invalid UTF-8 is refused, as Rust's String::from_utf8 does. */
    uint8_t bad[] = {2, 0, 0, 0, 0xed, 0xa0};
    CHECK(uas_decode_builtin(UAS_STRING, bad, sizeof bad, TDOT_DT_STRING, NULL, 0, &out, err,
                             sizeof err) != 0 &&
              !strcmp(err, "invalid UTF-8 while reading \"value\""),
          "bad utf-8: %s", err);
}

static void check_node_text(void) {
    const char *ns[] = {"http://opcfoundation.org/UA/", NULL, "urn:x"};
    char out[128];
    uas_node_text(2, "i=7", ns, 3, out, sizeof out);
    CHECK(!strcmp(out, "nsu=urn:x;i=7"), "%s", out);
    uas_node_text(1, "s=A", ns, 3, out, sizeof out);
    CHECK(!strcmp(out, "ns=1;s=A"), "%s", out);
    uas_node_text(0, "i=85", ns, 3, out, sizeof out);
    CHECK(!strcmp(out, "i=85"), "%s", out);
}

/* Configure the OPC UA module on one point with `extra` point keys and
 * `address`; returns its error ("" when accepted) and, when accepted, the
 * point's addr echo in addr_out. */
static const char *configure_point(const char *extra, const char *address, char *addr_out,
                                   size_t addr_len) {
    static char err[512];
    char path[] = "/tmp/tdot-opcua-struct-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return "mkstemp failed";
    char text[1024];
    int n = snprintf(text, sizeof text,
                     "[connector]\nprotocol = \"opcua\"\n\n[[device]]\nname = \"d\"\n"
                     "protocol_address = { endpoint = \"opc.tcp://127.0.0.1:1/\" }\n"
                     "  [[device.point]]\n  id = \"p\"\n%s\n  address = { %s }\n",
                     extra, address);
    if (write(fd, text, (size_t)n) != n) {
        close(fd);
        return "write failed";
    }
    close(fd);
    err[0] = '\0';
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    unlink(path);
    if (!cfg)
        return err;
    tdot_connector_t *conn = tdot_connector_opcua_new();
    if (conn->configure(conn, cfg, err, sizeof err) == 0) {
        err[0] = '\0';
        snprintf(addr_out, addr_len, "%s", cfg->devices[0].points[0].addr_json);
    }
    tdot_config_free(cfg);
    conn->destroy(conn);
    return err;
}

/* The configuration rules of `field` and `index`, with the Rust module's
 * messages (connector-opcua/src/lib.rs, configure). */
static void check_config(void) {
    char addr[512] = "";
    const char *e = configure_point("  datatype = \"float32\"",
                                    "node_id = \"ns=2;s=Pump\", field = \"Motor.Current\"",
                                    addr, sizeof addr);
    CHECK(!*e && !strcmp(addr, "{\"node_id\":\"ns=2;s=Pump\",\"field\":\"Motor.Current\"}"),
          "field point: \"%s\", addr %s", e, addr);
    e = configure_point("  datatype = \"float64\"",
                        "node_id = \"ns=2;s=T\", index = 2, field = \"Items[1].Value\"", addr,
                        sizeof addr);
    CHECK(!*e && strstr(addr, "\"index\":2") && strstr(addr, "\"field\":\"Items[1].Value\""),
          "index point: \"%s\", addr %s", e, addr);
    e = configure_point("  datatype = \"float64\"", "node_id = \"ns=2;s=P\", field = \"A..B\"",
                        addr, sizeof addr);
    CHECK(!strcmp(e, "point 'p' address: field path \"A..B\" has an empty segment"), "%s", e);
    e = configure_point("  mode = \"raw\"", "node_id = \"ns=2;s=P\", field = \"A\"", addr,
                        sizeof addr);
    CHECK(!strcmp(e, "point 'p': address.field selects a typed value; a raw point reads the "
                     "whole structure body"),
          "%s", e);
    e = configure_point("  datatype = \"float64\"\n  access = \"read_write\"",
                        "node_id = \"ns=2;s=P\", index = 1", addr, sizeof addr);
    CHECK(!strcmp(e, "point 'p': points with address.field or address.index are read-only"),
          "%s", e);
    e = configure_point("  datatype = \"float64\"", "node_id = \"ns=2;s=P\", index = -1", addr,
                        sizeof addr);
    CHECK(!strcmp(e, "point 'p' address: index must be a non-negative integer"), "%s", e);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vectors.json>\n", argv[0]);
        return 2;
    }
    char *text = vec_slurp(argv[1]);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    if (!root) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    ctx_t c = {.types_json = cJSON_GetObjectItem(root, "types")};
    CHECK(vec_types(root, &c.types) == 0, "types");
    c.nns = vec_namespaces(root, c.ns, 16);
    check_read_vectors(&c, root);
    check_invalid_paths(root);
    check_paths_parse();
    check_empty_structure_arrays();
    check_deep_nesting();
    check_limits();
    check_node_text();
    check_config();
    uas_types_free(&c.types);
    cJSON_Delete(root);
    free(text);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("opcua-struct: all vectors pass\n");
    return 0;
}
