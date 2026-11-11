/* tedge-dot — reading one value out of an OPC UA structure, an array, or one
 * of the built-in types that have no SDK primitive of their own (openspec
 * changes opcua-custom-datatypes D3/D7/D8 and opcua-structures-c).
 *
 * A port of impl/rust/crates/connector-opcua/src/structure.rs: the connector
 * turns the server's DataTypeDefinitions into a uas_types_t, a point's `field`
 * path is checked against it once (a uas_plan_t), and each value is decoded
 * by walking the binary body (OPC UA Part 6 §5.2) with that plan. Every field
 * before the selected one is skipped by its encoded length. Messages are the
 * Rust module's, word for word: both are held to the vectors in
 * doc/contract/test-vectors/opcua-struct/.
 *
 * Pure: no open62541 session, no allocation while decoding, no static mutable
 * state -- safe on concurrent connector threads.
 */
#ifndef TDOT_UA_STRUCTURE_H
#define TDOT_UA_STRUCTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdint.h>

#include "tedge_dot/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The built-in types (Part 6 §5.1.2), numbered as on the wire. */
enum {
    UAS_BOOLEAN = 1, UAS_SBYTE, UAS_BYTE, UAS_INT16, UAS_UINT16, UAS_INT32,
    UAS_UINT32, UAS_INT64, UAS_UINT64, UAS_FLOAT, UAS_DOUBLE, UAS_STRING,
    UAS_DATETIME, UAS_GUID, UAS_BYTESTRING, UAS_XMLELEMENT, UAS_NODEID,
    UAS_EXPANDEDNODEID, UAS_STATUSCODE, UAS_QUALIFIEDNAME, UAS_LOCALIZEDTEXT,
    UAS_EXTENSIONOBJECT, UAS_DATAVALUE, UAS_VARIANT, UAS_DIAGNOSTICINFO
};

/* The built-in type's name, or NULL for an id outside 1..25. */
const char *uas_builtin_name(int id);
/* The id of a built-in type name, or 0. */
int uas_builtin_from_name(const char *name);

typedef enum { UAS_T_BUILTIN, UAS_T_ENUM, UAS_T_STRUCT } uas_tkind_t;

typedef struct {
    char *name;
    uas_tkind_t kind;
    int builtin;     /* UAS_T_BUILTIN */
    char *type_name; /* UAS_T_ENUM: the Enumeration's name */
    size_t ref;      /* UAS_T_STRUCT: its index in uas_types_t.structs */
    bool array;      /* ValueRank 1 */
    bool optional;
} uas_field_t;

typedef enum { UAS_STRUCTURE, UAS_OPTIONAL_FIELDS, UAS_UNION } uas_skind_t;

typedef struct {
    char *name; /* the type's display name, for messages */
    uas_skind_t kind;
    uas_field_t *fields;
    size_t nfields;
} uas_struct_t;

typedef struct {
    uas_struct_t *structs;
    size_t n;
} uas_types_t;

/* Frees every structure's names and fields, and the array. */
void uas_types_free(uas_types_t *t);

/* One segment of a `field` path; index -1 for none. */
typedef struct {
    char *name;
    int64_t index;
} uas_seg_t;

typedef struct {
    uas_seg_t *segs;
    size_t n;
} uas_path_t;

/* Parse `Motor.Current` or `Items[1].Value`. 0, or -1 with err. */
int uas_parse_path(const char *path, uas_path_t *out, char *err, size_t errlen);
void uas_path_free(uas_path_t *p);
/* The path as text again, as the sample's addr.field echoes it. */
void uas_path_text(const uas_path_t *p, char *out, size_t outlen);

/* What a point finally reads: a built-in type, or an Enumeration (builtin 0,
 * with its name). */
typedef struct {
    int builtin;
    const char *enum_name; /* borrowed from the types */
} uas_leaf_t;

typedef struct {
    size_t s;     /* the structure this step reads from */
    size_t field; /* the position of the selected field in it */
    int64_t index;/* -1 for none */
} uas_step_t;

typedef struct {
    uas_step_t *steps;
    size_t n;
    uas_leaf_t leaf;
    tdot_datatype_t datatype;
} uas_plan_t;

/* Check a path against the definitions and the declared datatype, from
 * structure `root`. 0 with *plan, or -1 with err. */
int uas_compile(const uas_types_t *t, size_t root, const uas_path_t *path,
                tdot_datatype_t dt, uas_plan_t *plan, char *err, size_t errlen);
void uas_plan_free(uas_plan_t *p);

/* The datatypes a point may declare for a leaf; returns how many (0: none). */
size_t uas_accepted(const uas_leaf_t *leaf, tdot_datatype_t out[2]);

/* Check that a point declaring `dt` can read `leaf`; `subject` names the
 * value in the message (`field "Speed"`, `value`). */
int uas_check_datatype(const uas_leaf_t *leaf, tdot_datatype_t dt, const char *subject,
                       char *err, size_t errlen);

/* A decoded value: `val` as the point renders it and its `raw` bytes (at
 * most TDOT_RAW_MAX). */
typedef struct {
    tdot_value_t val;
    uint8_t raw[TDOT_RAW_MAX];
    size_t raw_len;
} uas_out_t;

/* Decode the value a plan selects from a structure body. `ns` is the server's
 * namespace array (nns entries; NULL entries allowed), for the nsu= forms.
 * 0 with *out, or -1 with err. */
int uas_decode(const uas_plan_t *plan, const uas_types_t *t, const uint8_t *body,
               size_t len, const char *const *ns, size_t nns, uas_out_t *out,
               char *err, size_t errlen);

/* Decode a value of built-in type `b` on its own (an array element or a
 * top-level variable's binary encoding). */
int uas_decode_builtin(int b, const uint8_t *body, size_t len, tdot_datatype_t dt,
                       const char *const *ns, size_t nns, uas_out_t *out, char *err,
                       size_t errlen);

/* A NodeId in the samples' text form: `nsu=<uri>;` for a namespace other
 * than 0 that ns lists, `ns=<n>;` otherwise. `ident` is the identifier part
 * (`i=7`, `s=X`, ...), as open62541 prints it after `;`. */
void uas_node_text(unsigned ns_index, const char *ident, const char *const *ns, size_t nns,
                   char *out, size_t outlen);

/* The C build's limits (design C5). */
#define UAS_TEXT_MAX (sizeof(((tdot_value_t *)0)->str) - 1) /* 255 */
#define UAS_BYTES_MAX (UAS_TEXT_MAX / 2)                     /* 127 */

#ifdef __cplusplus
}
#endif

#endif
