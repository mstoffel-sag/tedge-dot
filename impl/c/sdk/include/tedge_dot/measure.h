/* tedge-dot C SDK — measurement output (contract §5.5).
 *
 * With `[connector] output = "measurements"` the runtime publishes thin-edge
 * measurements itself, batched per device, group and time, instead of one
 * sample per reading for the ot-measurement flow to convert. The batcher holds
 * one table per device with a slot per point; the runtime offers it every
 * reading the report policy (§5.3) and the value map (§4.3) let through, and
 * the point's `batch.group_by` decides when a series is published and with
 * which time:
 *
 *   time    readings that share a millisecond timestamp share a message; a
 *           message closes when another time arrives for its group, when a
 *           point repeats, or at the end of the runtime's pass
 *   frame   readings within `window` of the device clock share a message
 *           stamped with the first reading's time; a repeated point starts a
 *           new message in the same window
 *   sample  every `every`, one series per slot holding a good value, stamped
 *           with the sample time; stale slots are left out, and event points
 *           (meta.event.every) are published as under `time`
 *
 * It is pure -- no clock, no MQTT -- so the runtime drives it with the real
 * clocks and the shared vectors (doc/contract/test-vectors/measurements/)
 * drive it in tests/measure.c, as they drive measure.rs in the Rust SDK.
 */
#ifndef TDOT_MEASURE_H
#define TDOT_MEASURE_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A batch is split before its estimated Cumulocity size passes this many
 * bytes: the thin-edge size plus TDOT_SERIES_OVERHEAD per series. */
#define TDOT_SIZE_BUDGET 15000
#define TDOT_SERIES_OVERHEAD 12

/* Where the batcher's messages go: a measurement (not retained) or a topic's
 * units (retained; an empty payload clears it). */
typedef void (*tdot_measure_out_fn)(void *ctx, const char *topic, const char *payload,
                                    bool retain);

typedef struct tdot_batcher tdot_batcher_t;

/* What the runtime does with a reading besides the measurement. */
typedef struct {
    bool sample;   /* publish the sample envelope */
    bool measured; /* the reading also became a series: "measured": true */
} tdot_routing_t;

/* The device clock is monotonic nanoseconds, so a frame's end compares exactly
 * as the Rust build's Instants do.
 *
 * The tables for `cfg`'s devices, `now_ns` the device clock. `previous`
 * hands over the units already published, so a reload publishes only the ones
 * that changed; it is freed. NULL when out of memory. */
tdot_batcher_t *tdot_batcher_new(const tdot_config_t *cfg, long long now_ns,
                                 tdot_batcher_t *previous);
void tdot_batcher_free(tdot_batcher_t *b);

/* Take one reading of point `k` of device `d` (indexes into cfg->devices and
 * dev->points) that the report policy let through, with its time `ts_ms` (Unix
 * milliseconds) and the device clock `now_ns`. Messages that close because of
 * it go to `out`. */
tdot_routing_t tdot_batcher_offer(tdot_batcher_t *b, size_t d, size_t k,
                                  const tdot_point_t *pt, const tdot_sample_t *s,
                                  double ts_ms, long long now_ns, tdot_measure_out_fn out,
                                  void *ctx);

/* The end of a runtime pass: publish every open `time` message. */
void tdot_batcher_end_of_pass(tdot_batcher_t *b, tdot_measure_out_fn out, void *ctx);

/* Publish the frames whose window has ended and the samples due at `now_ns`
 * (device clock); `wall_ms` is the time samples are stamped with. */
void tdot_batcher_due(tdot_batcher_t *b, long long now_ns, double wall_ms,
                      tdot_measure_out_fn out, void *ctx);

/* Publish every open message, frames included: the table is about to be
 * rebuilt or the connector stops. */
void tdot_batcher_flush_all(tdot_batcher_t *b, tdot_measure_out_fn out, void *ctx);

/* The earliest device-clock time tdot_batcher_due has something to do, or -1
 * when there is none. */
long long tdot_batcher_next_deadline(const tdot_batcher_t *b);

/* Mark device `d`'s slots stale (its link is down or its push delivery failed),
 * or healthy again. A stale device's slots are left out of samples. */
void tdot_batcher_set_stale(tdot_batcher_t *b, size_t d, bool stale);

/* Whether a flow reads this point's samples, so measurement output still
 * publishes them: meta.alarm, meta.event or meta.parameter (each unless
 * false), write access, or `sample = true`; `sample = false` turns them off. */
bool tdot_publishes_sample(const tdot_point_t *pt);

/* `ms` (Unix milliseconds) as RFC 3339 with milliseconds and a `Z`. */
void tdot_format_rfc3339_ms(double ms, char *dst, size_t dstlen);

/* Append `n` as JSON to `dst`: an integer without a fraction below 1e15, else
 * the shortest form that reads back as the same double, laid out as the Rust
 * build's ryu prints it. Returns the length written. */
size_t tdot_format_number(double n, char *dst, size_t dstlen);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_MEASURE_H */
