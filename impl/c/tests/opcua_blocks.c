/* OPC UA polling in blocks (ua_blocks.h): which points go into which Read,
 * which result reaches which point, and where a lost link ends the call --
 * against a fake Read, since the vendored open62541 is client-only and a test
 * cannot run a server. Point k reads node ns=1;i=k, and the fake answers each
 * node with its own identifier as the value, so a result handed to the wrong
 * point shows. Mirrors the read_in_blocks tests of
 * impl/rust/crates/connector-opcua/src/lib.rs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ua_blocks.h"

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

#define MAX_POINTS 1200
#define MAX_CALLS 16

/* The fake: how each point prepares, how each Read is answered, and what came
 * back to each point. */
typedef struct {
    /* Per point: -2 reads its node; 0 or -1 is its outcome without a read. */
    int prepare[MAX_POINTS];
    /* Per point: the node it reads (0: its own, k + 1: point k's) and its
     * element index (-1 by default). */
    size_t same_as[MAX_POINTS];
    long index[MAX_POINTS];
    /* Per Read call: how many of its nodes carry an IndexRange. */
    size_t ranged[MAX_CALLS + 1];
    /* Per point: the status its node's result carries, and whether it has a
     * value. */
    UA_StatusCode node_status[MAX_POINTS];
    bool no_value[MAX_POINTS];
    /* Per Read call (1-based): the service result, and whether the answer is
     * one result short. */
    UA_StatusCode service[MAX_CALLS + 1];
    bool short_answer[MAX_CALLS + 1];

    size_t calls;
    size_t sizes[MAX_CALLS + 1];
    bool misnamed; /* a Read asked for a node that is not a point's */
    /* What each point got: the status and, when good, the value. */
    bool got[MAX_POINTS];
    UA_StatusCode status[MAX_POINTS];
    double value[MAX_POINTS];
} fake_t;

static void fake_init(fake_t *f) {
    memset(f, 0, sizeof *f);
    for (size_t k = 0; k < MAX_POINTS; k++) {
        f->prepare[k] = -2;
        f->index[k] = -1;
    }
}

static bool fake_prepare(void *ctx, size_t k, UA_NodeId *node, long *index, int *rc) {
    fake_t *f = ctx;
    if (f->prepare[k] != -2) {
        *rc = f->prepare[k];
        return false;
    }
    size_t id = f->same_as[k] ? f->same_as[k] - 1 : k;
    *node = UA_NODEID_NUMERIC(1, (UA_UInt32)id);
    *index = f->index[k];
    return true;
}

static UA_ReadResponse fake_read(void *ctx, UA_ReadRequest *req) {
    fake_t *f = ctx;
    UA_ReadResponse resp;
    UA_ReadResponse_init(&resp);
    size_t call = ++f->calls;
    if (call <= MAX_CALLS) {
        f->sizes[call] = req->nodesToReadSize;
        for (size_t j = 0; j < req->nodesToReadSize; j++)
            f->ranged[call] += req->nodesToRead[j].indexRange.length > 0;
    }
    if (call <= MAX_CALLS && f->service[call] != UA_STATUSCODE_GOOD) {
        resp.responseHeader.serviceResult = f->service[call];
        return resp;
    }
    size_t n = req->nodesToReadSize;
    if (call <= MAX_CALLS && f->short_answer[call])
        n--;
    resp.results = UA_Array_new(n, &UA_TYPES[UA_TYPES_DATAVALUE]);
    resp.resultsSize = n;
    for (size_t j = 0; j < n; j++) {
        const UA_NodeId *id = &req->nodesToRead[j].nodeId;
        if (id->namespaceIndex != 1 || id->identifierType != UA_NODEIDTYPE_NUMERIC ||
            id->identifier.numeric >= MAX_POINTS) {
            f->misnamed = true;
            continue;
        }
        UA_UInt32 k = id->identifier.numeric;
        UA_DataValue *dv = &resp.results[j];
        if (f->node_status[k] != UA_STATUSCODE_GOOD) {
            dv->hasStatus = true;
            dv->status = f->node_status[k];
        }
        if (!f->no_value[k]) {
            UA_Double v = (UA_Double)k;
            UA_Variant_setScalarCopy(&dv->value, &v, &UA_TYPES[UA_TYPES_DOUBLE]);
            dv->hasValue = true;
        }
    }
    return resp;
}

/* read_point's link judgement for the statuses this test uses. */
static int fake_result(void *ctx, size_t k, UA_StatusCode status, const UA_Variant *value) {
    fake_t *f = ctx;
    f->got[k] = true;
    f->status[k] = status;
    if (status == UA_STATUSCODE_GOOD)
        f->value[k] = *(const UA_Double *)value->data;
    return status == UA_STATUSCODE_BADCONNECTIONCLOSED || status == UA_STATUSCODE_BADTIMEOUT
               ? -1
               : 0;
}

/* Read n points in blocks of `block` through the fake; returns what
 * tdot_ua_read_blocks returns. */
static size_t run(fake_t *f, size_t n, size_t block, int *rc) {
    UA_ReadValueId *nodes = calloc(block, sizeof *nodes);
    size_t *at = calloc(2 * n, sizeof *at);
    tdot_ua_blocks_t io = {
        .ctx = f, .prepare = fake_prepare, .read = fake_read, .result = fake_result};
    size_t got = tdot_ua_read_blocks(&io, n, block, nodes, at, rc);
    free(nodes);
    free(at);
    return got;
}

/* Every point read in [from, to) got a good result with its own value. */
static size_t good_in(const fake_t *f, const int *rc, size_t from, size_t to) {
    size_t good = 0;
    for (size_t k = from; k < to; k++)
        good += f->got[k] && rc[k] == 0 && f->status[k] == UA_STATUSCODE_GOOD &&
                f->value[k] == (double)k;
    return good;
}

static fake_t f;
static int rc[MAX_POINTS];

static void check_blocks(void) {
    fake_init(&f);
    size_t got = run(&f, 1200, 500, rc);
    CHECK(got == 1200 && f.calls == 3 && f.sizes[1] == 500 && f.sizes[2] == 500 &&
              f.sizes[3] == 200,
          "1200 points in Reads of 500, 500, 200: %zu read, %zu call(s) of %zu, %zu, %zu", got,
          f.calls, f.sizes[1], f.sizes[2], f.sizes[3]);
    CHECK(!f.misnamed && good_in(&f, rc, 0, 1200) == 1200,
          "every point gets its own node's value: %zu of 1200", good_in(&f, rc, 0, 1200));
}

/* Points that are not read take no place in a block, and the points after
 * them still get their own results. */
static void check_unread_points(void) {
    fake_init(&f);
    f.prepare[3] = 0;
    f.prepare[501] = 0;
    size_t got = run(&f, 1200, 500, rc);
    CHECK(got == 1200 && f.calls == 3 && f.sizes[1] == 500 && f.sizes[2] == 500 &&
              f.sizes[3] == 198,
          "1198 nodes in Reads of 500, 500, 198: %zu read, %zu call(s) of %zu, %zu, %zu", got,
          f.calls, f.sizes[1], f.sizes[2], f.sizes[3]);
    CHECK(!f.got[3] && !f.got[501] && rc[3] == 0 && rc[501] == 0,
          "unread points keep their own outcome");
    CHECK(good_in(&f, rc, 0, 1200) == 1198, "the other 1198 points get their own value: %zu",
          good_in(&f, rc, 0, 1200));
}

/* A point whose preparation takes the link down: the points before it are
 * read, nothing after it, and the call ends there -- as polling one point at a
 * time did. */
static void check_prepare_lost(void) {
    fake_init(&f);
    f.prepare[700] = -1;
    size_t got = run(&f, 1200, 500, rc);
    CHECK(got == 701 && rc[700] == -1, "the call ends at point 700: %zu read, rc %d", got,
          rc[700]);
    CHECK(f.calls == 2 && f.sizes[2] == 200, "points 500..699 are still read: %zu call(s), %zu",
          f.calls, f.sizes[2]);
    CHECK(good_in(&f, rc, 0, 700) == 700, "points 0..699 get their own value: %zu",
          good_in(&f, rc, 0, 700));
}

/* A Read the server refuses as a whole gives each of its points that status;
 * the next blocks are read. */
static void check_refused_block(void) {
    fake_init(&f);
    f.service[2] = UA_STATUSCODE_BADTOOMANYOPERATIONS;
    size_t got = run(&f, 1200, 500, rc);
    size_t refused = 0;
    for (size_t k = 500; k < 1000; k++)
        refused += f.got[k] && rc[k] == 0 && f.status[k] == UA_STATUSCODE_BADTOOMANYOPERATIONS;
    CHECK(got == 1200 && f.calls == 3, "every block is read: %zu read, %zu call(s)", got, f.calls);
    CHECK(refused == 500, "the refused block's points get its status: %zu of 500", refused);
    CHECK(good_in(&f, rc, 0, 500) == 500 && good_in(&f, rc, 1000, 1200) == 200,
          "the other blocks' points get their own value");
}

/* A Read that finds the link gone ends the call at its first point, and no
 * further Read is sent. */
static void check_lost_block(void) {
    fake_init(&f);
    f.service[2] = UA_STATUSCODE_BADCONNECTIONCLOSED;
    size_t got = run(&f, 1200, 500, rc);
    CHECK(got == 501 && rc[500] == -1 && f.calls == 2,
          "the call ends at point 500: %zu read, rc %d, %zu call(s)", got, rc[500], f.calls);
}

/* A result that does not answer every node fails the whole block, as a
 * single read's unexpected answer does. */
static void check_short_answer(void) {
    fake_init(&f);
    f.short_answer[1] = true;
    size_t got = run(&f, 10, 500, rc);
    size_t unexpected = 0;
    for (size_t k = 0; k < 10; k++)
        unexpected += f.status[k] == UA_STATUSCODE_BADUNEXPECTEDERROR;
    CHECK(got == 10 && unexpected == 10, "a short answer fails its block: %zu of 10", unexpected);
}

/* Each result's status is what UA_Client_readValueAttribute returns for the
 * node alone; one that takes the link down ends the call there. */
static void check_result_status(void) {
    fake_init(&f);
    f.node_status[1] = UA_STATUSCODE_BADNODEIDUNKNOWN;
    f.no_value[2] = true; /* good, but no value */
    f.node_status[3] = UA_STATUSCODE_GOOD | 0x400; /* good with info bits */
    f.no_value[4] = true;
    f.node_status[4] = UA_STATUSCODE_UNCERTAININITIALVALUE;
    f.node_status[8] = UA_STATUSCODE_BADTIMEOUT;
    size_t got = run(&f, 10, 500, rc);
    CHECK(f.status[1] == UA_STATUSCODE_BADNODEIDUNKNOWN, "a bad status is passed: %s",
          UA_StatusCode_name(f.status[1]));
    CHECK(f.status[2] == UA_STATUSCODE_BADUNEXPECTEDERROR, "no value is unexpected: %s",
          UA_StatusCode_name(f.status[2]));
    CHECK(f.status[3] == (UA_STATUSCODE_GOOD | 0x400), "info bits are passed: 0x%08x",
          (unsigned)f.status[3]);
    CHECK(f.status[4] == UA_STATUSCODE_UNCERTAININITIALVALUE,
          "an uncertain status is passed: %s", UA_StatusCode_name(f.status[4]));
    CHECK(got == 9 && rc[8] == -1, "a node that takes the link down ends the call: %zu, rc %d",
          got, rc[8]);
    CHECK(good_in(&f, rc, 5, 8) == 3 && good_in(&f, rc, 0, 1) == 1,
          "the other points get their own value");
}

/* Points on the same node share its ReadValueId and its result; points on
 * the same node with different indexes do not (openspec opcua-structures-c,
 * design C6). A block counts nodes, not points. */
static void check_shared_nodes(void) {
    fake_init(&f);
    for (size_t k = 1; k < 600; k++)
        f.same_as[k] = 1; /* points 0..599 all read node 0 */
    f.index[700] = 3;
    f.same_as[701] = 701; /* node 700, no index */
    f.index[702] = 3;
    f.same_as[702] = 701; /* node 700, index 3: shares with point 700 */
    size_t got = run(&f, 1200, 500, rc);
    CHECK(got == 1200 && f.calls == 2 && f.sizes[1] == 500 && f.sizes[2] == 100,
          "1200 points on 600 distinct reads, in Reads of 500 and 100: %zu read, %zu call(s) "
          "of %zu, %zu",
          got, f.calls, f.sizes[1], f.sizes[2]);
    size_t shared = 0;
    for (size_t k = 0; k < 600; k++)
        shared += f.got[k] && rc[k] == 0 && f.value[k] == 0.0;
    CHECK(shared == 600, "every point on node 0 gets its value: %zu of 600", shared);
    CHECK(f.got[700] && f.got[701] && f.got[702] && f.value[702] == 700.0,
          "points on node 700 get its value");
    CHECK(f.ranged[1] + f.ranged[2] == 1, "one IndexRange read for index 3 on node 700: %zu",
          f.ranged[1] + f.ranged[2]);
    CHECK(good_in(&f, rc, 703, 1200) == 497, "the other points get their own value");
}

int main(void) {
    check_blocks();
    check_unread_points();
    check_prepare_lost();
    check_refused_block();
    check_lost_block();
    check_short_answer();
    check_result_status();
    check_shared_nodes();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("opcua-blocks: all checks passed\n");
    return 0;
}
