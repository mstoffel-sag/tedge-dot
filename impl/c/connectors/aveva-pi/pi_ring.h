/* AVEVA PI connector -- the ring that carries a device's pushed events from its push worker to
 * the runtime thread (spec §5.4, design D9).
 *
 * The OPC UA ring's bounds (connectors/opcua/push_ring.h): it starts small, grows by doubling,
 * and may grow to max(65536, 4 x points). Unlike the OPC UA ring it is filled on another thread
 * than the one that empties it, so it is locked, and the producer WAITS while the ring holds
 * more than half its bound (the high-water mark) until the drain brings it below: a catch-up
 * is paced by the runtime and never makes the ring drop. An entry is small -- the event as
 * canonical JSON text -- and is decoded on the runtime thread when drained.
 */
#ifndef TDOT_PI_RING_H
#define TDOT_PI_RING_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI_RING_MIN 256
#define PI_RING_BOUND_MIN 65536

typedef struct {
    uint32_t point;    /* index into the device's points */
    uint8_t replayed;  /* delivered after the fact (contract §5.3 rule 7) */
    char *event;       /* the PI event `{Timestamp, Value, ...}` as canonical JSON; owned */
} pi_event_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t space; /* signalled when the drain took events, and on close */
    pi_event_t *slots;
    size_t cap;    /* slots; one stays free to tell full from empty */
    size_t bound;  /* the most slots it may grow to */
    size_t head, tail;
    bool closed;
    unsigned long dropped; /* past the bound, or out of memory: never in normal operation */
} pi_ring_t;

/* A ring for a device that pushes `points` points. */
int pi_ring_init(pi_ring_t *r, size_t points);
/* Free the queued events and the ring. */
void pi_ring_destroy(pi_ring_t *r);

size_t pi_ring_len(pi_ring_t *r);
/* The high-water mark: the producer waits while the ring holds more. */
size_t pi_ring_high_water(const pi_ring_t *r);

/* Called about every 100 ms while the producer waits: the worker reads its channels meanwhile.
 * Returning false gives up the wait (the worker is stopping). */
typedef bool (*pi_ring_idle_t)(void *ctx);

/* Queue one event, waiting while the ring is above its high-water mark. On success the ring
 * owns ev->event. Returns -1 (the caller keeps ev->event) when the ring was closed, the idle
 * callback gave up, or the event could not be queued. */
int pi_ring_push(pi_ring_t *r, pi_event_t *ev, pi_ring_idle_t idle, void *ctx);

/* Take every queued event, in order: *out is a heap array the caller frees with its events.
 * Never waits for the producer beyond the lock. Returns the count. */
size_t pi_ring_take(pi_ring_t *r, pi_event_t **out);

/* Wake the producer and refuse further events. */
void pi_ring_close(pi_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif
