/* tedge-dot C SDK — value mapping (contract §4.3).
 *
 * A point's `map` converts its value between the device's representation and
 * the one its samples carry: state codes to labels, ranges to bands with a
 * catch-all, numeric text to numbers. The runtime applies it after the
 * transform on reads (tdot_map_apply) and maps a written value back before the
 * inverse transform on writes (tdot_map_invert).
 *
 * Mirrors impl/rust/crates/sdk/src/map.rs rule for rule and message for
 * message; the shared vectors in doc/contract/test-vectors/map/ hold both to
 * them.
 */
#ifndef TDOT_MAP_H
#define TDOT_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"
#include "toml.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The JSON type of a mapped value, as `value_repr` names it. */
typedef enum {
    TDOT_MAP_NUMBER = 0,
    TDOT_MAP_STRING,
    TDOT_MAP_BOOL,
} tdot_map_kind_t;

/* One scalar of a map as written: a TOML integer keeps its exact value. */
typedef struct {
    tdot_map_kind_t kind;
    double num;
    bool is_int;
    int64_t i;
    bool b;
    char *s;
} tdot_map_val_t;

typedef struct {
    bool range;
    /* eq: any of these (a scalar eq is a list of one) */
    tdot_map_val_t *eq;
    size_t neq;
    /* range: both bounds inclusive, either open; `write` is what a write of
     * the case's output sends */
    bool has_min, has_max, has_write;
    double min, max;
    tdot_map_val_t write;
    tdot_map_val_t to;
} tdot_map_case_t;

typedef struct tdot_map {
    tdot_map_case_t *cases;
    size_t ncases;
    bool has_default;
    tdot_map_val_t def;
    bool has_as;
    tdot_map_kind_t as;
} tdot_map_t;

/* Parse and check a `map` table as written. Returns 0 with *out set -- NULL
 * for an empty table, which is how a site removes a map a point library
 * declares -- or -1 with `err` naming the key (`map.cases[1].to`), not the
 * point. */
int tdot_map_parse(toml_table_t *table, tdot_map_t **out, char *err, size_t errlen);
void tdot_map_free(tdot_map_t *map);

/* The checks that need the point's resolved mode and datatype: typed values
 * only, ranges only on numbers, an `eq` only of the kind the point has.
 * `datatype_name` is the datatype as written ("bytes" has no tdot_datatype_t). */
int tdot_map_check_point(const tdot_map_t *map, bool raw_mode, const char *datatype_name,
                         char *err, size_t errlen);

/* The kind of every value the map produces. */
tdot_map_kind_t tdot_map_output_kind(const tdot_map_t *map);

/* Map a decoded (and transformed) value of a point of `dt`. Returns 0 with
 * *out set, or -1 with `err` holding the sample's error text without the
 * point prefix ("no mapping for value 3"). */
int tdot_map_apply(const tdot_map_t *map, const tdot_value_t *in, tdot_datatype_t dt,
                   tdot_value_t *out, char *err, size_t errlen);

/* The device value a write of the mapped `in` sends to a point of `dt`.
 * Returns 0 with *out set, or -1 with the write's failure reason (without the
 * point prefix). */
int tdot_map_invert(const tdot_map_t *map, const tdot_value_t *in, tdot_datatype_t dt,
                    tdot_value_t *out, char *err, size_t errlen);

/* The outputs a write accepts by case, in case order without repeats; fills
 * up to `max` pointers into the map and returns how many there are. A closed
 * set when the map has no `as`. */
size_t tdot_map_writable(const tdot_map_t *map, const tdot_map_val_t **out, size_t max);

/* A decimal number as text, by the grammar
 * `[+-]?(digits[.digits?] | .digits)([eE][+-]?digits)?` with surrounding
 * whitespace, finite. */
bool tdot_map_parse_number(const char *text, double *out);
/* A finite number as the shortest decimal digits that read back as it, in
 * positional notation, `-0` as "0" -- the text Rust's `{}` prints. False for
 * NaN and infinities, or when `len` is too short. */
bool tdot_map_format_number(double n, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_MAP_H */
