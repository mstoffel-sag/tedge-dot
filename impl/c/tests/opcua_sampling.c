/* OPC UA subscription timing (doc/connectors/opcua-connector-spec.md): the
 * sampling and publishing intervals the module requests, and when a server's
 * revision counts as one. Mirrors the tests of
 * impl/rust/crates/connector-opcua/src/lib.rs `publishing_interval_for` and
 * `revised`, so both builds request the same rates for the same config.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tedge_dot/config.h"
#include "ua_sampling.h"

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static tdot_point_t pushed(double sampling_s) {
    tdot_point_t pt;
    memset(&pt, 0, sizeof pt);
    pt.subscribe = true;
    pt.access = TDOT_ACCESS_READ;
    pt.sampling_interval_s = sampling_s;
    return pt;
}

static double publishing(tdot_point_t *points, size_t n, size_t *wanted) {
    tdot_device_t dev;
    memset(&dev, 0, sizeof dev);
    dev.points = points;
    dev.npoints = n;
    return tdot_ua_publishing_interval_ms(&dev, wanted);
}

/* The subscription publishes at the fastest sampling interval of the points it
 * subscribes, zero included and wherever it comes in the list. */
static void check_publishing_interval(void) {
    size_t wanted;
    tdot_point_t a[] = {pushed(1.0), pushed(0.25)};
    CHECK(publishing(a, 2, &wanted) == 250.0 && wanted == 2, "fastest of 1s/250ms");

    tdot_point_t b[] = {pushed(2.0), pushed(5.0)};
    CHECK(publishing(b, 2, &wanted) == 2000.0, "fastest of 2s/5s");

    tdot_point_t c[] = {pushed(1.0), pushed(0.0)};
    CHECK(publishing(c, 2, &wanted) == 0.0, "zero last stays the fastest");

    tdot_point_t d[] = {pushed(0.0), pushed(1.0)};
    CHECK(publishing(d, 2, &wanted) == 0.0, "zero first stays the fastest");

    /* Opted-out and write-only points are polled, so they do not set the pace. */
    tdot_point_t e[] = {pushed(0.1), pushed(0.05), pushed(2.0)};
    e[0].subscribe = false;
    e[1].access = TDOT_ACCESS_WRITE;
    CHECK(publishing(e, 3, &wanted) == 2000.0 && wanted == 1,
          "only subscribed readable points count, got %zu", wanted);

    tdot_point_t f[] = {pushed(1.0)};
    f[0].subscribe = false;
    publishing(f, 1, &wanted);
    CHECK(wanted == 0, "nothing to subscribe");
}

static void check_sampling_interval(void) {
    tdot_point_t pt = pushed(0.2);
    pt.poll_interval_s = 3600.0;
    CHECK(tdot_ua_sampling_interval_ms(&pt) == 200.0,
          "the sampling interval, not the poll interval");
}

/* Only a revision of a millisecond or more is one. */
static void check_revised(void) {
    CHECK(tdot_ua_revised(50.0, 1000.0), "50ms -> 1s");
    CHECK(tdot_ua_revised(0.0, 100.0), "0 -> 100ms");
    CHECK(!tdot_ua_revised(200.0, 200.0), "granted as requested");
    CHECK(!tdot_ua_revised(200.0, 200.4), "under a millisecond");
    CHECK(!tdot_ua_revised(200.0, NAN), "NaN");
}

int main(void) {
    check_publishing_interval();
    check_sampling_interval();
    check_revised();
    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("opcua sampling: all checks passed\n");
    return failures ? 1 : 0;
}
