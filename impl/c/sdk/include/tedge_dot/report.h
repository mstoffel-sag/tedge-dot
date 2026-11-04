/* tedge-dot C SDK — the reporting policy (report by exception, contract §5.3).
 *
 * A point's `report` table decides which of its readings are published: only
 * changes, only changes beyond a deadband, at most once per interval, once a
 * change has settled -- and a heartbeat that still publishes a fresh reading
 * when nothing changes. The runtime applies it in front of the sample topic,
 * so every connector behaves alike and no module filters.
 *
 * tdot_report_state_t is the per-point state machine. It is pure -- no clock,
 * no MQTT -- so the runtime drives it with the monotonic time and the shared
 * test vectors (doc/contract/test-vectors/report/) drive it in
 * impl/c/tests/report.c. It implements exactly the rules of the Rust SDK's
 * impl/rust/crates/sdk/src/report.rs, in the same order; the vectors are what
 * keeps the two identical.
 */
#ifndef TDOT_REPORT_H
#define TDOT_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/* The keys a `report` table may carry, NULL-terminated, in their canonical
 * order (the order the capability descriptor lists them in). */
extern const char *const TDOT_REPORT_KEYS[];

typedef enum {
    TDOT_DEADBAND_NONE = 0,
    TDOT_DEADBAND_ABSOLUTE, /* in the point's transformed units */
    TDOT_DEADBAND_PERCENT,  /* of the last published value's magnitude */
} tdot_deadband_kind_t;

/* A point's effective reporting policy. All zero publishes every reading.
 * Durations are nanoseconds, 0 meaning off. */
typedef struct {
    bool on_change;
    tdot_deadband_kind_t deadband_kind;
    double deadband;
    int64_t min_interval;
    int64_t max_interval;
    int64_t debounce;
} tdot_report_policy_t;

/* Build the policy from a (merged) `report` table as JSON. Values are assumed
 * validated (the loader's check); a zero duration or deadband switches the
 * setting off. Numbers may be cJSON numbers or raw number text (the loader
 * keeps them as written). A NULL or non-object table is the passthrough. */
void tdot_report_policy_from_json(const struct cJSON *table, tdot_report_policy_t *out);

/* The effective policy of a point whose merged table is `table`. Returns true,
 * with `warning` filled, when inheritance put a heartbeat at or under the rate
 * limit -- which no single table may do: the heartbeat is then raised to twice
 * the rate limit, keeping it strictly longer. */
bool tdot_report_effective(const struct cJSON *table, tdot_report_policy_t *out,
                           char *warning, size_t wlen);

/* Whether the policy publishes every reading, so the runtime can skip it. */
bool tdot_report_is_passthrough(const tdot_report_policy_t *p);

/* Whether readings are compared with the last published one. */
bool tdot_report_filters_changes(const tdot_report_policy_t *p);

/* `"<p>%"` as p; false when `s` is not a well-formed percentage. */
bool tdot_report_parse_percent(const char *s, double *out);

/* What a reading is compared by: numbers numerically, everything else exactly
 * -- its value with its representation, or its raw bytes when it has none. */
#define TDOT_REPORT_OBS_MAX (TDOT_RAW_MAX * 3 + 8)
typedef struct {
    bool is_num;
    double num;
    char other[TDOT_REPORT_OBS_MAX];
} tdot_report_obs_t;

/* The comparable part of a sample, as it will be published: a bad sample and a
 * raw-mode one (value kind NONE) compare by their raw bytes. */
void tdot_report_obs_of(const tdot_sample_t *s, tdot_report_obs_t *out);

/* A reading as the runtime holds it back: the sample and the time it was
 * taken, so a held reading publishes with its own `ts`. While the state holds
 * an item, `sample.addr_json` (when set) is a heap copy the state owns. */
typedef struct {
    tdot_sample_t sample;
    char ts[40];
    double ts_ms;
} tdot_report_item_t;

/* Release what an item returned by tdot_report_due owns. */
void tdot_report_item_release(tdot_report_item_t *item);

typedef struct {
    tdot_report_policy_t policy;
    /* When the state was created or last reset: the heartbeat's reference
     * before any publish. */
    int64_t origin;
    bool has_last;
    tdot_report_obs_t last_obs;
    tdot_quality_t last_quality;
    /* Whether last_published is set: a publish since the last reset, or the
     * time a replay resumes from. */
    bool has_published;
    int64_t last_published;
    bool has_read;
    int64_t last_read;
    /* A heartbeat read returned nothing for the point; no more until a reset. */
    bool unreadable;
    bool has_pending;
    tdot_report_item_t pending;
    tdot_report_obs_t pending_obs;
    bool has_candidate;
    tdot_report_obs_t cand_first;
    int64_t cand_since;
    tdot_report_item_t cand_latest;
    tdot_report_obs_t cand_latest_obs;
    /* Replay (contract §5.3): while `replaying`, the interval timers run on
     * the source time of replayed readings; replay_last_offer is the runtime
     * time of the last one. last_published_src is the source time of the last
     * reading published in a replay, to resume from in the next. */
    bool replaying;
    int64_t replay_last_offer;
    bool has_published_src;
    int64_t last_published_src;
} tdot_report_state_t;

/* How long a point stays on source time after its last replayed reading when
 * no live reading ends the replay first, in nanoseconds. */
#define TDOT_REPORT_REPLAY_IDLE_NS ((int64_t)2000000000)

/* `now` is monotonic time in nanoseconds, from any fixed origin. */
void tdot_report_init(tdot_report_state_t *st, const tdot_report_policy_t *policy,
                      int64_t now);
/* Release the held items. The state must be initialised again before use. */
void tdot_report_free(tdot_report_state_t *st);
/* Forget everything, so the next reading is published: a reload, a device
 * reconnect, or an MQTT session restore. */
void tdot_report_reset(tdot_report_state_t *st, int64_t now);
/* A heartbeat read returned nothing for this point: it cannot be read on
 * demand, and is not asked again until a reset. */
void tdot_report_no_data(tdot_report_state_t *st);

/* Offer a reading. Returns true when `item` is to be published now; otherwise
 * it is dropped or held (a copy -- the caller keeps `item`) and may come back
 * from tdot_report_due. */
bool tdot_report_offer(tdot_report_state_t *st, const tdot_report_item_t *item,
                       const tdot_report_obs_t *obs, tdot_quality_t quality, int64_t now);

/* Offer a reading the module replays from the source's history, with its
 * source time `src` (nanoseconds from any fixed origin, such as the Unix
 * epoch). The policy's intervals are measured on source time, so a burst of
 * history is reduced the way the live stream would have been: a held or
 * settled reading whose interval ended by `src` is released first, into
 * `released` (the caller then owns it: tdot_report_item_release), with
 * *has_released set. Returns true when `item` is to be published now, after
 * the released one. */
bool tdot_report_offer_replayed(tdot_report_state_t *st, const tdot_report_item_t *item,
                                const tdot_report_obs_t *obs, tdot_quality_t quality,
                                int64_t src, int64_t now, tdot_report_item_t *released,
                                bool *has_released);

/* A pass of the runtime loop. Returns true with `out` filled (the caller then
 * owns it: tdot_report_item_release) when a held reading's interval ended or a
 * debounced one settled. `*read` says whether a pushed point's heartbeat read
 * is due; `pushed` is whether the point is delivered by push (off the poll
 * schedule), since only those are read on demand. */
bool tdot_report_due(tdot_report_state_t *st, int64_t now, bool pushed,
                     tdot_report_item_t *out, bool *read);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_REPORT_H */
