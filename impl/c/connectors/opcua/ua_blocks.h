/* tedge-dot — OPC UA polling in blocks (doc/connectors/opcua-connector-spec.md,
 * "Many polled points").
 *
 * How a device's due points are gathered into Read requests and each result
 * is handed back to its point, kept apart from the session so
 * tests/opcua_blocks.c can check the bookkeeping against a fake Read. The
 * Rust module's `read_in_blocks` (impl/rust/crates/connector-opcua/src/lib.rs)
 * does the same.
 */
#ifndef TDOT_UA_BLOCKS_H
#define TDOT_UA_BLOCKS_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <open62541/types.h>

/* The IndexRange text of an element index ("3"), or none for -1. */
static inline UA_String tdot_ua_index_range(int64_t index) {
    char buf[24];
    if (index < 0)
        return UA_STRING_NULL;
    snprintf(buf, sizeof buf, "%" PRId64, index);
    return UA_STRING_ALLOC(buf);
}

/* What a block read needs from its caller, for points 0..n. */
typedef struct {
    void *ctx;
    /* Point k's node: true with *node set (then owned by the caller) and
     * *index the array element it reads (-1 for the whole value), or false
     * with point k's read_point outcome in *rc (0 or -1) when it is not read
     * at all (a method, an untranslated namespace, an invalid node id). */
    bool (*prepare)(void *ctx, size_t k, UA_NodeId *node, int64_t *index, int *rc);
    /* One Read request. The caller clears the response. */
    UA_ReadResponse (*read)(void *ctx, UA_ReadRequest *req);
    /* Point k's read_point outcome for a Read of its node that returned
     * `status` and, when that is GOOD, `value`. */
    int (*result)(void *ctx, size_t k, UA_StatusCode status, const UA_Variant *value);
} tdot_ua_blocks_t;

/* Read points 0..n in blocks of at most `block` (>= 1) nodes, one Read per
 * block, with nodes[] (`block` entries) and at[] (`2 * n` entries) as
 * scratch. Points that read the same node and index share one ReadValueId
 * (openspec change opcua-structures-c, design C6): each gets the same result,
 * and a block counts each node once. A point with an index sends it as the
 * IndexRange. Each point's
 * status is what UA_Client_readValueAttribute would return for it alone; a
 * Read the server fails as a whole gives each of its points that status.
 * rc[k] is point k's outcome. Stops after the first point whose outcome is -1
 * and returns how many points it got to (that one included), else n. */
size_t tdot_ua_read_blocks(const tdot_ua_blocks_t *io, size_t n, size_t block,
                           UA_ReadValueId *nodes, size_t *at, int *rc);

#endif
