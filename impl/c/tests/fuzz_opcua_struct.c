/* Fuzz target for the OPC UA structure walker (connectors/opcua/structure.c),
 * the C counterpart of impl/rust/crates/connector-opcua/fuzz structure_walk:
 * every plan the shared vectors compile, and every built-in type with each
 * datatype it accepts, decode arbitrary bodies without crashing or reading
 * past them.
 *
 * Two builds of the same file:
 * - with -DTDOT_FUZZ=ON (clang, -fsanitize=fuzzer,address): a libFuzzer
 *   target, `tedge-dot-fuzz-opcua-struct [corpus] -max_total_time=60`;
 * - otherwise a deterministic driver run by ctest: random bodies and single-
 *   byte mutations and cuts of every vector body.
 *
 * The vectors file comes from TDOT_STRUCT_VECTORS (set by CMake) or the
 * environment variable of the same name.
 */
#include <stdint.h>

#include "opcua_struct_vectors.h"

#ifndef TDOT_STRUCT_VECTORS
#define TDOT_STRUCT_VECTORS "doc/contract/test-vectors/opcua-struct/vectors.json"
#endif

typedef struct {
    uas_plan_t plan;
    uint8_t body[1024];
    size_t len;
} fz_case_t;

static uas_types_t g_types;
static const char *g_ns[16];
static size_t g_nns;
static fz_case_t *g_cases;
static size_t g_ncases;
static cJSON *g_root;

static void setup(void) {
    if (g_root)
        return;
    const char *path = getenv("TDOT_STRUCT_VECTORS");
    char *text = vec_slurp(path ? path : TDOT_STRUCT_VECTORS);
    g_root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!g_root) {
        fprintf(stderr, "cannot read the vectors (set TDOT_STRUCT_VECTORS)\n");
        abort();
    }
    vec_types(g_root, &g_types);
    g_nns = vec_namespaces(g_root, g_ns, 16);
    const cJSON *types = cJSON_GetObjectItem(g_root, "types");
    const cJSON *reads = cJSON_GetObjectItem(g_root, "read");
    g_cases = calloc((size_t)cJSON_GetArraySize(reads), sizeof *g_cases);
    const cJSON *v;
    cJSON_ArrayForEach(v, reads) {
        const cJSON *p = cJSON_GetObjectItem(v, "path");
        size_t root = vec_struct_index(types, cJSON_GetObjectItem(v, "root")->valuestring);
        if (!p || root == (size_t)-1)
            continue;
        uas_path_t path;
        char err[256];
        if (uas_parse_path(p->valuestring, &path, err, sizeof err))
            continue;
        fz_case_t *c = &g_cases[g_ncases];
        tdot_datatype_t dt =
            tdot_datatype_parse(cJSON_GetObjectItem(v, "datatype")->valuestring);
        if (uas_compile(&g_types, root, &path, dt, &c->plan, err, sizeof err) == 0) {
            c->len = vec_unhex(cJSON_GetObjectItem(v, "body")->valuestring, c->body,
                               sizeof c->body);
            g_ncases++;
        }
        uas_path_free(&path);
    }
}

/* One input: its first byte picks a plan, the rest is the body. Every built-in
 * type decodes the body too. */
static void one(const uint8_t *data, size_t size) {
    uas_out_t out;
    char err[256];
    if (size == 0)
        return;
    const fz_case_t *c = &g_cases[data[0] % g_ncases];
    (void)uas_decode(&c->plan, &g_types, data + 1, size - 1, g_ns, g_nns, &out, err, sizeof err);
    for (int b = 1; b <= 25; b++) {
        uas_leaf_t leaf = {.builtin = b};
        tdot_datatype_t dts[2];
        size_t n = uas_accepted(&leaf, dts);
        for (size_t i = 0; i < n; i++)
            (void)uas_decode_builtin(b, data + 1, size - 1, dts[i], g_ns, g_nns, &out, err,
                                     sizeof err);
    }
}

#ifdef TDOT_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setup();
    one(data, size);
    return 0;
}
#else
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

int main(int argc, char **argv) {
    if (argc > 1)
        setenv("TDOT_STRUCT_VECTORS", argv[1], 1);
    setup();
    long rounds = argc > 2 ? atol(argv[2]) : 20000;
    /* Inputs are heap copies of exactly their length, so ASan would catch a
     * read past the body. */
    for (long k = 0; k < rounds; k++) {
        size_t pick = next() % g_ncases;
        const fz_case_t *c = &g_cases[pick];
        size_t size;
        uint8_t *in;
        if (k % 2) {
            size = 1 + next() % 200;
            in = malloc(size);
            for (size_t i = 1; i < size; i++)
                in[i] = (uint8_t)next();
        } else {
            size = 1 + c->len;
            in = malloc(size);
            memcpy(in + 1, c->body, c->len);
            if (c->len) {
                in[1 + next() % c->len] = (uint8_t)next();
                size = 1 + next() % (c->len + 1);
            }
        }
        in[0] = (uint8_t)pick;
        one(in, size);
        free(in);
    }
    printf("fuzz-opcua-struct: %ld inputs over %zu plans\n", rounds, g_ncases);
    return 0;
}
#endif
