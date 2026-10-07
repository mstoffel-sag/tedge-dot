/* tedge-dot — OPC UA subscription timing (doc/connectors/opcua-connector-spec.md).
 *
 * The rates the module requests for a device's subscription, kept apart from
 * the open62541 calls so tests/opcua_sampling.c can check them. They match the
 * Rust module (impl/rust/crates/connector-opcua/src/lib.rs
 * `publishing_interval_for`, `revised`), so the same config requests the same
 * rates in both builds.
 */
#ifndef TDOT_UA_SAMPLING_H
#define TDOT_UA_SAMPLING_H

#include <stdbool.h>
#include <stddef.h>

#include "tedge_dot/config.h"

/* A pushed point's monitored-item sampling interval: its effective
 * sampling_interval, resolved by the loader (0: the server's fastest rate). */
double tdot_ua_sampling_interval_ms(const tdot_point_t *pt);

/* The device subscription's publishing interval: the fastest sampling interval
 * of the points it would subscribe, zero included. *wanted is set to how many
 * points that is; the result is meaningless when it is 0. */
double tdot_ua_publishing_interval_ms(const tdot_device_t *dev, size_t *wanted);

/* Whether the server granted another interval than requested. Both are whole
 * milliseconds in practice, so a difference under a millisecond is not one. */
bool tdot_ua_revised(double requested_ms, double revised_ms);

#endif
