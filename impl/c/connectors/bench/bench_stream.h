/* tedge-dot — the `bench` module's value streams, defined in
 * doc/contract/test-vectors/bench/README.md and implemented identically by
 * impl/rust/crates/connector-bench/src/stream.rs. Header-only so the unit test
 * runs the same code; every file including it is compiled with
 * -ffp-contract=off, so no fused multiply-add changes the last bit of a walk.
 */
#ifndef TDOT_BENCH_STREAM_H
#define TDOT_BENCH_STREAM_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "tedge_dot/model.h"

typedef enum { BENCH_WALK = 0, BENCH_CONSTANT, BENCH_COUNTER } bench_model_t;

/* FNV-1a 64 over `<a>\0<b>`; a zero hash becomes 1. */
static inline uint64_t bench_seed(const char *a, const char *b) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)a; *p; p++)
        h = (h ^ *p) * 0x100000001b3ULL;
    h = (h ^ 0) * 0x100000001b3ULL;
    for (const unsigned char *p = (const unsigned char *)b; *p; p++)
        h = (h ^ *p) * 0x100000001b3ULL;
    return h ? h : 1;
}

/* xorshift64*. */
typedef struct {
    uint64_t x;
} bench_rng_t;

static inline void bench_rng_init(bench_rng_t *r, const char *a, const char *b) {
    r->x = bench_seed(a, b);
}

static inline uint64_t bench_rng_next(bench_rng_t *r) {
    uint64_t x = r->x;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->x = x;
    return x * 0x2545f4914f6cdd1dULL;
}

/* A draw in [0, 1). */
static inline double bench_rng_draw(bench_rng_t *r) {
    return (double)(bench_rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

typedef struct {
    bench_rng_t rng;
    bench_model_t model;
    double step;
    double change;
    double acc;
    uint64_t changes;
} bench_stream_t;

static inline void bench_stream_init(bench_stream_t *s, const char *device, const char *point,
                                     bench_model_t model, double step, double change) {
    bench_rng_init(&s->rng, device, point);
    s->model = model;
    s->step = step;
    s->change = change;
    s->acc = 0.0;
    s->changes = 0;
}

/* Take the next reading. */
static inline void bench_stream_advance(bench_stream_t *s) {
    double u1 = bench_rng_draw(&s->rng);
    if (u1 < s->change && s->model != BENCH_CONSTANT) {
        s->changes++;
        if (s->model == BENCH_WALK) {
            double u2 = bench_rng_draw(&s->rng);
            double delta = (2.0 * u2 - 1.0) * s->step;
            s->acc = s->acc + delta;
        } else {
            s->acc = s->acc + 1.0;
        }
    }
}

/* The current reading as `datatype` (float64, int32, bool or string). */
static inline void bench_stream_value(const bench_stream_t *s, tdot_datatype_t datatype,
                                      tdot_value_t *out) {
    switch (datatype) {
    case TDOT_DT_INT32: {
        double r = round(s->acc); /* half away from zero */
        out->kind = TDOT_VAL_NUM;
        out->num = r < -2147483648.0 ? -2147483648.0 : r > 2147483647.0 ? 2147483647.0 : r;
        break;
    }
    case TDOT_DT_BOOL:
        out->kind = TDOT_VAL_BOOL;
        out->b = s->changes % 2 == 1;
        break;
    case TDOT_DT_STRING:
        out->kind = TDOT_VAL_STR;
        snprintf(out->str, sizeof out->str, "%llu", (unsigned long long)s->changes);
        break;
    default:
        out->kind = TDOT_VAL_NUM;
        out->num = s->acc;
        break;
    }
}

#endif /* TDOT_BENCH_STREAM_H */
