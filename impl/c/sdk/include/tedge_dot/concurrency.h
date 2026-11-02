/* tedge-dot C SDK -- concurrent reads (contract §5.6): how many requests may
 * be in flight to one endpoint, and in all. The same rules as
 * impl/rust/crates/sdk/src/concurrency.rs.
 *
 * Every device has an endpoint, named by its module (the connector's endpoint
 * hook): the host and port it talks to, a serial port, a CAN interface.
 * Devices that share one share its budget, max_inflight (default 4; the
 * smallest of their values when they differ), and the module's cap for the
 * endpoint always applies (1 for a bus). max_inflight_total (default 32)
 * bounds the requests in flight across all endpoints.
 *
 * A read takes one request of its endpoint's budget, waiting if none is free,
 * then as many more as are free, up to what it asks for
 * (tdot_budgets_acquire); every device on an endpoint therefore makes
 * progress. The module sends at most tdot_inflight_current() requests at once
 * and reports refusals and timeouts: an endpoint that says it is busy has its
 * budget halved, and regains one step after TDOT_REGROW_AFTER successful
 * requests. Thread-safe: reads run on the runtime's worker threads. */
#ifndef TDOT_CONCURRENCY_H
#define TDOT_CONCURRENCY_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TDOT_DEFAULT_MAX_INFLIGHT 4
#define TDOT_DEFAULT_MAX_INFLIGHT_TOTAL 32
#define TDOT_REGROW_AFTER 20

typedef struct tdot_budget tdot_budget_t;
typedef struct tdot_budgets tdot_budgets_t;

/* The requests a read holds while it runs; give them back with
 * tdot_inflight_release. */
typedef struct {
    tdot_budget_t *endpoint;
    tdot_budget_t *total;
    size_t held;
} tdot_inflight_t;

/* Names device `dev`'s endpoint into buf and returns the module's cap for it
 * (0: none), or returns -1 when the device is its own endpoint. */
typedef int (*tdot_endpoint_fn)(void *ctx, const tdot_device_t *dev, char *buf, size_t len);

/* The budgets of `cfg`'s devices. `endpoint` may be NULL: every device is its
 * own endpoint. NULL when out of memory. */
tdot_budgets_t *tdot_budgets_new(const tdot_config_t *cfg, tdot_endpoint_fn endpoint, void *ctx);
void tdot_budgets_free(tdot_budgets_t *b);

/* Take the requests a read of device `index` may send at once: one, waiting
 * until its endpoint and the total each have one free, then up to wanted - 1
 * more that are free now. */
void tdot_budgets_acquire(tdot_budgets_t *b, size_t index, size_t wanted, tdot_inflight_t *out);
void tdot_inflight_release(tdot_inflight_t *f);

/* The requests the read was granted, and what it may have in flight now: that,
 * or less once the endpoint backed off; never below 1. */
size_t tdot_inflight_allowance(const tdot_inflight_t *f);
size_t tdot_inflight_current(const tdot_inflight_t *f);

/* A request was answered; was refused as busy; timed out (a backoff only when
 * other requests to the endpoint were in flight). */
void tdot_inflight_ok(const tdot_inflight_t *f);
void tdot_inflight_busy(const tdot_inflight_t *f);
void tdot_inflight_timed_out(const tdot_inflight_t *f, bool others_in_flight);

/* Device `index`'s endpoint: its name, its configured budget and the budget in
 * effect now. */
const char *tdot_budgets_endpoint(const tdot_budgets_t *b, size_t index);
size_t tdot_budgets_configured(const tdot_budgets_t *b, size_t index);
size_t tdot_budgets_effective(const tdot_budgets_t *b, size_t index);

/* The capability descriptor's `endpoints`: each endpoint with its devices and
 * its budget, as a cJSON array the caller owns. */
struct cJSON *tdot_budgets_describe(const tdot_budgets_t *b);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_CONCURRENCY_H */
