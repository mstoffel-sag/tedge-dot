/* AVEVA PI connector -- the push ring. See pi_ring.h. */
#include "pi_ring.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int pi_ring_init(pi_ring_t *r, size_t points) {
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->space, NULL);
    r->bound = 4 * points > PI_RING_BOUND_MIN ? 4 * points : PI_RING_BOUND_MIN;
    size_t want = PI_RING_MIN;
    while (want < 2 * points + 1 && want < r->bound)
        want *= 2;
    r->slots = calloc(want, sizeof *r->slots);
    if (!r->slots)
        return -1;
    r->cap = want;
    return 0;
}

void pi_ring_destroy(pi_ring_t *r) {
    if (r->slots)
        for (size_t i = r->tail; i != r->head; i = (i + 1) % r->cap)
            free(r->slots[i].event);
    free(r->slots);
    pthread_cond_destroy(&r->space);
    pthread_mutex_destroy(&r->mu);
    memset(r, 0, sizeof *r);
}

static size_t len_locked(const pi_ring_t *r) {
    return r->cap ? (r->head + r->cap - r->tail) % r->cap : 0;
}

size_t pi_ring_len(pi_ring_t *r) {
    pthread_mutex_lock(&r->mu);
    size_t n = len_locked(r);
    pthread_mutex_unlock(&r->mu);
    return n;
}

size_t pi_ring_high_water(const pi_ring_t *r) { return r->bound / 2; }

/* Move the queued events, in order, into a ring of `cap` slots. */
static bool grow(pi_ring_t *r, size_t cap) {
    if (cap > SIZE_MAX / sizeof(pi_event_t))
        return false;
    pi_event_t *slots = malloc(cap * sizeof *slots);
    if (!slots)
        return false;
    size_t n = len_locked(r);
    for (size_t i = 0; i < n; i++)
        slots[i] = r->slots[(r->tail + i) % r->cap];
    free(r->slots);
    r->slots = slots;
    r->cap = cap;
    r->tail = 0;
    r->head = n;
    return true;
}

int pi_ring_push(pi_ring_t *r, pi_event_t *ev, pi_ring_idle_t idle, void *ctx) {
    pthread_mutex_lock(&r->mu);
    while (!r->closed && len_locked(r) >= pi_ring_high_water(r)) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100 * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&r->space, &r->mu, &until);
        if (rc == ETIMEDOUT && idle && len_locked(r) >= pi_ring_high_water(r)) {
            /* Let the worker read its channels without holding the lock. */
            pthread_mutex_unlock(&r->mu);
            bool go_on = idle(ctx);
            pthread_mutex_lock(&r->mu);
            if (!go_on) {
                pthread_mutex_unlock(&r->mu);
                return -1;
            }
        }
    }
    if (r->closed) {
        pthread_mutex_unlock(&r->mu);
        return -1;
    }
    if ((r->head + 1) % r->cap == r->tail) {
        size_t cap = 2 * r->cap > r->bound ? r->bound : 2 * r->cap;
        if (cap <= r->cap || !grow(r, cap)) {
            r->dropped++;
            pthread_mutex_unlock(&r->mu);
            return -1;
        }
    }
    r->slots[r->head] = *ev;
    r->head = (r->head + 1) % r->cap;
    pthread_mutex_unlock(&r->mu);
    return 0;
}

size_t pi_ring_take(pi_ring_t *r, pi_event_t **out) {
    *out = NULL;
    pthread_mutex_lock(&r->mu);
    size_t n = len_locked(r);
    pi_event_t *taken = n ? malloc(n * sizeof *taken) : NULL;
    if (!taken) {
        pthread_mutex_unlock(&r->mu);
        return 0;
    }
    for (size_t i = 0; i < n; i++)
        taken[i] = r->slots[(r->tail + i) % r->cap];
    r->tail = r->head;
    pthread_cond_broadcast(&r->space);
    pthread_mutex_unlock(&r->mu);
    *out = taken;
    return n;
}

void pi_ring_close(pi_ring_t *r) {
    pthread_mutex_lock(&r->mu);
    r->closed = true;
    pthread_cond_broadcast(&r->space);
    pthread_mutex_unlock(&r->mu);
}
