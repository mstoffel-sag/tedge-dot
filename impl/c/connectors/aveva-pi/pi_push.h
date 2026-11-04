/* AVEVA PI connector -- push delivery (spec §5.2-§5.4, design D5, D9). Mirrors
 * impl/rust/crates/connector-aveva-pi/src/push.rs on a worker thread per subscribed device.
 *
 * pi_push_start runs on the runtime thread: it opens the device's Channels WebSockets (or sets
 * up the recorded loop) and starts the worker. Everything after that -- the catch-up from the
 * cursors, the channel reads, the recorded polls, every backfill request -- runs on the
 * worker, which owns its own curl handles. It decodes nothing: each event it accepts goes into
 * the device's ring as canonical JSON text, and pi_push_drain (runtime thread) turns the
 * queued events into samples, hands them to the sink and advances the persisted cursors.
 *
 * Dedup needs to know what is queued, not only what was drained, so the worker keeps its own
 * copy of the cursors ("sent"), advanced as it queues; the persisted ones advance on drain.
 */
#ifndef TDOT_PI_PUSH_H
#define TDOT_PI_PUSH_H

#include <stdbool.h>
#include <stddef.h>

#include "pi_connector.h"

typedef struct pi_push pi_push_t;

typedef struct {
    tdot_device_t *dev;
    const pi_device_t *pd;
    pi_live_t *live;
    const size_t *points;   /* indexes of the pushed points in dev->points */
    size_t n;
    const char *state_file; /* <state_dir>/<service_name>/<device>.json */
    double interval_s;      /* the recorded polling interval */
    bool recorded;          /* poll the archive instead of opening channels */
} pi_push_args_t;

/* Start push delivery. -1 with *e when it could not start; *refused is then set when the
 * WebSocket upgrade was answered with a status other than 101 (the caller falls back to
 * recorded delivery). */
int pi_push_start(const pi_push_args_t *a, pi_push_t **out, pi_error_t *e, bool *refused);

/* Hand the queued events to the sink and advance the cursors. Never waits for the worker.
 * Returns -1 once the worker stopped on a failure or went silent (liveness). */
int pi_push_drain(pi_push_t *p, tdot_device_t *dev, const pi_live_t *live,
                  tdot_sample_sink_t sink, void *ctx);

/* Stop the worker, join it, write the cursors and free everything. */
void pi_push_stop(pi_push_t *p);

#endif
