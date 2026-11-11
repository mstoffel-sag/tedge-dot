/* The shared OPC UA structure vectors
 * (doc/contract/test-vectors/opcua-struct/vectors.json) as walker inputs:
 * the type set, keyed by type name as in Rust's type_set_from_json, and
 * helpers for the vector runner (tests/opcua_struct.c) and the fuzz target
 * (tests/fuzz_opcua_struct.c). */
#ifndef TDOT_OPCUA_STRUCT_VECTORS_H
#define TDOT_OPCUA_STRUCT_VECTORS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "structure.h"

static char *vec_slurp(const char *path) {
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
    if (buf)
        buf[n] = '\0';
    return buf;
}

static size_t vec_unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* The index of the structure named `name` (a non-enum entry of `types`),
 * counting the entries in order; (size_t)-1 when there is none. */
static size_t vec_struct_index(const cJSON *types, const char *name) {
    size_t i = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, types) {
        if (cJSON_GetObjectItem(t, "enum"))
            continue;
        if (!strcmp(t->string, name))
            return i;
        i++;
    }
    return (size_t)-1;
}

static int vec_types(const cJSON *root, uas_types_t *out) {
    const cJSON *types = cJSON_GetObjectItem(root, "types");
    memset(out, 0, sizeof *out);
    size_t n = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, types) n += cJSON_GetObjectItem(t, "enum") == NULL;
    out->structs = calloc(n, sizeof *out->structs);
    if (!out->structs)
        return -1;
    cJSON_ArrayForEach(t, types) {
        if (cJSON_GetObjectItem(t, "enum"))
            continue;
        uas_struct_t *s = &out->structs[out->n++];
        s->name = strdup(t->string);
        const char *kind = cJSON_GetObjectItem(t, "structure_type")->valuestring;
        s->kind = !strcmp(kind, "Union") ? UAS_UNION
                  : !strcmp(kind, "StructureWithOptionalFields") ? UAS_OPTIONAL_FIELDS
                                                                  : UAS_STRUCTURE;
        const cJSON *fields = cJSON_GetObjectItem(t, "fields");
        s->nfields = (size_t)cJSON_GetArraySize(fields);
        s->fields = calloc(s->nfields ? s->nfields : 1, sizeof *s->fields);
        size_t j = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            uas_field_t *g = &s->fields[j++];
            g->name = strdup(cJSON_GetObjectItem(f, "name")->valuestring);
            const char *ty = cJSON_GetObjectItem(f, "type")->valuestring;
            const cJSON *def = cJSON_GetObjectItem(types, ty);
            if ((g->builtin = uas_builtin_from_name(ty))) {
                g->kind = UAS_T_BUILTIN;
            } else if (def && cJSON_GetObjectItem(def, "enum")) {
                g->kind = UAS_T_ENUM;
                g->type_name = strdup(ty);
            } else {
                g->kind = UAS_T_STRUCT;
                g->ref = vec_struct_index(types, ty);
            }
            g->array = cJSON_IsTrue(cJSON_GetObjectItem(f, "array"));
            g->optional = cJSON_IsTrue(cJSON_GetObjectItem(f, "optional"));
        }
    }
    return 0;
}

/* The namespace array, borrowed from the JSON. */
static size_t vec_namespaces(const cJSON *root, const char **out, size_t cap) {
    size_t n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItem(root, "namespaces")) {
        if (n < cap)
            out[n++] = s->valuestring;
    }
    return n;
}

#endif
