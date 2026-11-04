/* AVEVA PI connector -- decoding a PI value (doc/connectors/aveva-pi-connector-spec.md §4).
 * Mirrors impl/rust/crates/connector-aveva-pi/src/value.rs, checked against the same golden
 * vectors (connectors/aveva-pi/conformance/decode-vectors.json, ctest aveva-pi-vectors).
 *
 * No I/O here. JSON goes through pi_json_parse rather than cJSON_Parse: cJSON keeps every
 * number as a double, which loses the integers PI writes beyond 2^53. pi_json_parse builds an
 * ordinary cJSON tree, and an integer literal of 2^53 or more in magnitude that fits 64 bits
 * also keeps its exact decimal text in the number item's valuestring (cJSON_Delete frees it),
 * the way serde_json keeps i64/u64 on the Rust side.
 */
#ifndef TDOT_PI_VALUE_H
#define TDOT_PI_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "cjson/cJSON.h"
#include "tedge_dot/config.h"
#include "tedge_dot/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* addr.flags bits, in the order PI's fields are listed (spec §4). */
#define PI_FLAG_QUESTIONABLE 0x1
#define PI_FLAG_SUBSTITUTED 0x2
#define PI_FLAG_ANNOTATED 0x4

/* v when it is an array, else NULL: for cJSON_ArrayForEach over an optional field. */
static inline const cJSON *pi_array(const cJSON *v) { return cJSON_IsArray(v) ? v : NULL; }

/* Parse JSON text (len bytes, not necessarily NUL-terminated). NULL when it is not one JSON
 * value. Caller cJSON_Delete()s. */
cJSON *pi_json_parse(const char *text, size_t len);

/* A JSON number in canonical form (spec §4 "Canonical text"): an integer literal exactly, an
 * integral double below 2^53 in magnitude as an integer, any other in shortest round-trip
 * digits as Python's repr writes them. */
void pi_number_text(const cJSON *n, char *dst, size_t cap);
void pi_float_text(double f, char *dst, size_t cap);

/* Compact JSON with object keys sorted, numbers in canonical form and strings escaped as
 * serde_json escapes them. Caller frees. NULL only when out of memory. */
char *pi_canonical(const cJSON *v);

/* A PI flag: a JSON boolean or the string "true"/"false" (any case); absent is false. */
bool pi_flag(const cJSON *pi, const char *key);

/* The messages of an `{"Errors": [...]}` object joined with "; " into dst ("error" when the
 * list holds none). False when v has no Errors array. */
bool pi_errors_of(const cJSON *v, char *dst, size_t cap);

/* Whether v is a digital state `{"Name", "Value", ...}` (and not an Errors object). */
bool pi_is_digital(const cJSON *v);

/* A decoded PI value: what the sample is built from (value.rs `Decoded`). */
typedef struct {
    tdot_quality_t quality;
    bool has_value;         /* typed mode, good samples only */
    tdot_value_t value;
    char error[TDOT_ERR_MAX];
    char *raw;              /* canonical text of `Value`; caller frees */
    unsigned flags;         /* PI_FLAG_* */
    const char *timestamp;  /* PI's Timestamp, borrowed from the input; NULL when none */
    const char *units;      /* the value's UnitsAbbreviation when non-empty, borrowed */
} pi_decoded_t;

/* Decode one PI value object `{Timestamp, Value, Good, ...}` for a point (spec §4). */
void pi_decode(const cJSON *pi, tdot_datatype_t dt, tdot_mode_t mode, pi_decoded_t *out);
void pi_decoded_free(pi_decoded_t *d);

/* Convert a PI value to a datatype (value.rs `to_datatype`). 0, or -1 with err filled. */
int pi_to_datatype(const cJSON *v, tdot_datatype_t dt, tdot_value_t *out, char *err,
                   size_t errlen);

/* The JSON a write sends as `Value` for a non-digital point (value.rs `encode_write`): the
 * requested value checked and encoded for the datatype (TDOT_DT_NONE: as given). Returns a
 * new item, or NULL with err filled. */
cJSON *pi_encode_write(const tdot_value_t *value, tdot_datatype_t dt, char *err,
                       size_t errlen);

/* A requested write value as JSON: bool, number or string. A string that holds a 64-bit
 * integer beyond 2^53 (how the SDK carries one, model.h) stays a string. */
cJSON *pi_value_json(const tdot_value_t *value);

#ifdef __cplusplus
}
#endif

#endif
