/* The reporting policy (report by exception, contract §5.3): the per-point
 * state machine. Rule for rule, in the same order, the Rust SDK's
 * impl/rust/crates/sdk/src/report.rs -- the shared vectors in
 * doc/contract/test-vectors/report/ run against both. */
#include "tedge_dot/report.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "tedge_dot/config.h"
#include "tedge_dot/decode.h"

const char *const TDOT_REPORT_KEYS[] = {"on_change",    "deadband", "min_interval",
                                        "max_interval", "debounce", NULL};

/* Differences up to this are not a change for `on_change` without a deadband. */
#define EPSILON 1e-9

bool tdot_report_parse_percent(const char *s, double *out) {
    size_t n = strlen(s);
    if (n < 2 || s[n - 1] != '%')
        return false;
    size_t len = n - 1; /* the number before the '%' */
    size_t dots = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '.')
            dots++;
        else if (s[i] < '0' || s[i] > '9')
            return false;
    }
    if (dots > 1 || s[0] == '.' || s[len - 1] == '.')
        return false;
    if (out)
        *out = strtod(s, NULL);
    return true;
}

/* A number of the table: a cJSON number, or the raw text the loader keeps so a
 * value is echoed as written. */
static bool json_number(const cJSON *v, double *out) {
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    if (cJSON_IsRaw(v) && v->valuestring) {
        char *end = NULL;
        *out = strtod(v->valuestring, &end);
        return end != v->valuestring;
    }
    return false;
}

/* A duration of the table in nanoseconds; 0 when absent, off or unparseable
 * (the loader has refused the latter already). */
static int64_t json_duration(const cJSON *table, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(table, key);
    if (!cJSON_IsString(v))
        return 0;
    double secs = tdot_duration_parse(v->valuestring);
    if (secs <= 0)
        return 0;
    return (int64_t)llround(secs * 1e9);
}

void tdot_report_policy_from_json(const cJSON *table, tdot_report_policy_t *out) {
    memset(out, 0, sizeof *out);
    if (!cJSON_IsObject(table))
        return;
    out->on_change = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(table, "on_change"));
    const cJSON *db = cJSON_GetObjectItemCaseSensitive(table, "deadband");
    double d;
    if (json_number(db, &d)) {
        if (d > 0) {
            out->deadband_kind = TDOT_DEADBAND_ABSOLUTE;
            out->deadband = d;
        }
    } else if (cJSON_IsString(db) && tdot_report_parse_percent(db->valuestring, &d) &&
               d > 0) {
        out->deadband_kind = TDOT_DEADBAND_PERCENT;
        out->deadband = d;
    }
    out->min_interval = json_duration(table, "min_interval");
    out->max_interval = json_duration(table, "max_interval");
    out->debounce = json_duration(table, "debounce");
}

/* A duration as the Rust `Duration` Debug prints the common ones ("1800s",
 * "500ms"), so the warning reads the same in both builds. */
static void format_duration(int64_t ns, char *dst, size_t len) {
    if (ns % 1000000000 == 0)
        snprintf(dst, len, "%llds", (long long)(ns / 1000000000));
    else if (ns % 1000000 == 0 && ns < 1000000000)
        snprintf(dst, len, "%lldms", (long long)(ns / 1000000));
    else
        snprintf(dst, len, "%gs", (double)ns / 1e9);
}

bool tdot_report_effective(const cJSON *table, tdot_report_policy_t *out, char *warning,
                           size_t wlen) {
    tdot_report_policy_from_json(table, out);
    if (out->min_interval > 0 && out->max_interval > 0 &&
        out->max_interval <= out->min_interval) {
        int64_t raised = out->min_interval * 2;
        char max[32], min[32], use[32];
        format_duration(out->max_interval, max, sizeof max);
        format_duration(out->min_interval, min, sizeof min);
        format_duration(raised, use, sizeof use);
        if (warning && wlen)
            snprintf(warning, wlen,
                     "the inherited report.max_interval (%s) is not longer than "
                     "report.min_interval (%s); using %s",
                     max, min, use);
        out->max_interval = raised;
        return true;
    }
    return false;
}

bool tdot_report_is_passthrough(const tdot_report_policy_t *p) {
    return !p->on_change && p->deadband_kind == TDOT_DEADBAND_NONE && p->min_interval == 0 &&
           p->max_interval == 0 && p->debounce == 0;
}

bool tdot_report_filters_changes(const tdot_report_policy_t *p) {
    return p->on_change || p->deadband_kind != TDOT_DEADBAND_NONE || p->debounce > 0;
}

/* Whether `obs` is a change from `base` under this policy. */
static bool changed(const tdot_report_policy_t *p, const tdot_report_obs_t *obs,
                    const tdot_report_obs_t *base) {
    if (obs->is_num && base->is_num) {
        double a = obs->num, b = base->num;
        /* NaN equals only NaN, so a published NaN cannot block the point. */
        if (isnan(a) || isnan(b))
            return isnan(a) != isnan(b);
        double delta = fabs(a - b);
        switch (p->deadband_kind) {
        case TDOT_DEADBAND_ABSOLUTE:
            return delta >= p->deadband;
        case TDOT_DEADBAND_PERCENT:
            /* From zero, any change passes: a percentage of 0 is 0. */
            if (b == 0.0)
                return delta > 0.0;
            return delta >= p->deadband / 100.0 * fabs(b);
        default:
            return delta > EPSILON;
        }
    }
    if (!obs->is_num && !base->is_num)
        return strcmp(obs->other, base->other) != 0;
    return true; /* a number against anything else: its representation changed */
}

void tdot_report_obs_of(const tdot_sample_t *s, tdot_report_obs_t *out) {
    out->is_num = false;
    out->num = 0;
    out->other[0] = '\0';
    /* A bad sample carries no value (the envelope omits it), whatever the
     * module left in the struct. */
    tdot_value_kind_t kind = s->quality == TDOT_Q_BAD ? TDOT_VAL_NONE : s->value.kind;
    switch (kind) {
    case TDOT_VAL_NUM:
        out->is_num = true;
        out->num = s->value.num;
        break;
    case TDOT_VAL_BOOL:
        snprintf(out->other, sizeof out->other, "b:%s", s->value.b ? "true" : "false");
        break;
    case TDOT_VAL_STR:
        snprintf(out->other, sizeof out->other, "s:%s", s->value.str);
        break;
    default: {
        char hex[TDOT_RAW_MAX * 3 + 1];
        tdot_hex_format(s->raw, s->raw_len, 1, hex, sizeof hex);
        snprintf(out->other, sizeof out->other, "r:%s", hex);
        break;
    }
    }
}

/* ---- held items --------------------------------------------------------------
 * A module's addr_json is borrowed for the duration of one call only, so an
 * item the state keeps gets its own copy, freed when the item is dropped. */

static void item_hold(tdot_report_item_t *dst, const tdot_report_item_t *src) {
    *dst = *src;
    if (src->sample.addr_json)
        dst->sample.addr_json = strdup(src->sample.addr_json);
}

void tdot_report_item_release(tdot_report_item_t *item) {
    /* const only because a module's copy is borrowed; this one is ours. */
    free((void *)(uintptr_t)item->sample.addr_json);
    item->sample.addr_json = NULL;
}

static void clear_pending(tdot_report_state_t *st) {
    if (st->has_pending)
        tdot_report_item_release(&st->pending);
    st->has_pending = false;
}

static void clear_candidate(tdot_report_state_t *st) {
    if (st->has_candidate)
        tdot_report_item_release(&st->cand_latest);
    st->has_candidate = false;
}

/* Hold `item` as pending, replacing any older pending reading. */
static void hold_pending(tdot_report_state_t *st, const tdot_report_item_t *item,
                         const tdot_report_obs_t *obs) {
    clear_pending(st);
    item_hold(&st->pending, item);
    st->pending_obs = *obs;
    st->has_pending = true;
}

/* ---- the state machine -------------------------------------------------------- */

void tdot_report_init(tdot_report_state_t *st, const tdot_report_policy_t *policy,
                      int64_t now) {
    memset(st, 0, sizeof *st);
    st->policy = *policy;
    st->origin = now;
}

void tdot_report_free(tdot_report_state_t *st) {
    clear_pending(st);
    clear_candidate(st);
}

void tdot_report_reset(tdot_report_state_t *st, int64_t now) {
    tdot_report_policy_t policy = st->policy;
    tdot_report_free(st);
    tdot_report_init(st, &policy, now);
}

void tdot_report_no_data(tdot_report_state_t *st) { st->unreadable = true; }

static int64_t since_published(const tdot_report_state_t *st, int64_t now) {
    int64_t ref = st->has_published ? st->last_published : st->origin;
    return now > ref ? now - ref : 0;
}

/* Record a publish: it becomes the reference, and nothing held survives it. */
static void publish(tdot_report_state_t *st, const tdot_report_obs_t *obs,
                    tdot_quality_t quality, int64_t now) {
    st->last_obs = *obs;
    st->last_quality = quality;
    st->has_last = true;
    st->has_published = true;
    st->last_published = now;
    clear_pending(st);
    clear_candidate(st);
}

/* End a replay: the timers of a held or debouncing reading restart on the
 * runtime clock. */
static void hand_over(tdot_report_state_t *st, int64_t now) {
    st->replaying = false;
    if (st->has_published) {
        st->has_published_src = true;
        st->last_published_src = st->last_published;
        st->last_published = now;
    }
    if (st->has_candidate)
        st->cand_since = now;
}

static bool offer_at(tdot_report_state_t *st, const tdot_report_item_t *item,
                     const tdot_report_obs_t *obs, tdot_quality_t quality, int64_t now);

bool tdot_report_offer(tdot_report_state_t *st, const tdot_report_item_t *item,
                       const tdot_report_obs_t *obs, tdot_quality_t quality, int64_t now) {
    if (st->replaying)
        hand_over(st, now);
    return offer_at(st, item, obs, quality, now);
}

static bool release(tdot_report_state_t *st, int64_t now, tdot_report_item_t *out);

bool tdot_report_offer_replayed(tdot_report_state_t *st, const tdot_report_item_t *item,
                                const tdot_report_obs_t *obs, tdot_quality_t quality,
                                int64_t src, int64_t now, tdot_report_item_t *released,
                                bool *has_released) {
    if (!st->replaying) {
        /* Resume source time where the last replay left it; with none, the
         * next reading is far enough from the origin to count as a new
         * interval. */
        st->has_published = st->has_published_src;
        st->last_published = st->last_published_src;
        if (st->has_candidate)
            st->cand_since = src;
    }
    st->replaying = true;
    st->replay_last_offer = now;
    *has_released = release(st, src, released);
    bool out = offer_at(st, item, obs, quality, src);
    if (st->has_published) {
        st->has_published_src = true;
        st->last_published_src = st->last_published;
    }
    return out;
}

static bool offer_at(tdot_report_state_t *st, const tdot_report_item_t *item,
                     const tdot_report_obs_t *obs, tdot_quality_t quality, int64_t now) {
    const tdot_report_policy_t *p = &st->policy;
    /* Always published: the first reading, and a quality change. */
    if (!st->has_last || st->last_quality != quality) {
        publish(st, obs, quality, now);
        return true;
    }
    /* A fresh reading after max_interval of silence is the heartbeat. */
    if (p->max_interval > 0 && since_published(st, now) >= p->max_interval) {
        publish(st, obs, quality, now);
        return true;
    }
    bool changed_from_last = changed(p, obs, &st->last_obs);

    if (p->debounce > 0) {
        if (!changed_from_last) {
            /* Back at the published value: nothing settled or held is news
             * any more. */
            clear_candidate(st);
            clear_pending(st);
            return false;
        }
        if (st->has_candidate && !changed(p, obs, &st->cand_first)) {
            /* The run goes on: keep its most recent reading. */
            tdot_report_item_release(&st->cand_latest);
            item_hold(&st->cand_latest, item);
            st->cand_latest_obs = *obs;
            if (now - st->cand_since < p->debounce)
                return false;
            /* Settled with this very reading, which goes on below. */
            clear_candidate(st);
        } else {
            clear_candidate(st);
            st->cand_first = *obs;
            st->cand_since = now;
            item_hold(&st->cand_latest, item);
            st->cand_latest_obs = *obs;
            st->has_candidate = true;
            return false;
        }
    }

    if (p->min_interval > 0 && since_published(st, now) < p->min_interval) {
        hold_pending(st, item, obs);
        return false;
    }
    /* A newer reading supersedes anything held, whether or not it is published
     * itself. */
    clear_pending(st);
    if (tdot_report_filters_changes(p) && !changed(p, obs, &st->last_obs))
        return false;
    publish(st, obs, quality, now);
    return true;
}

/* Publish a held reading if it is still a change from the last published one.
 * Takes ownership of `item` either way: on true it moves to `out`. */
static bool publish_if_changed(tdot_report_state_t *st, tdot_report_item_t *item,
                               const tdot_report_obs_t *obs, int64_t now,
                               tdot_report_item_t *out) {
    if (!st->has_last ||
        (tdot_report_filters_changes(&st->policy) && !changed(&st->policy, obs, &st->last_obs))) {
        tdot_report_item_release(item);
        return false;
    }
    tdot_report_obs_t o = *obs; /* obs may live in the state publish() clears */
    publish(st, &o, st->last_quality, now);
    *out = *item;
    return true;
}

/* A held reading whose interval ended, or a debounced one that settled, by
 * `now`: true with `out` filled. */
static bool release(tdot_report_state_t *st, int64_t now, tdot_report_item_t *out) {
    const tdot_report_policy_t *p = &st->policy;
    bool published = false;
    if (p->min_interval > 0 && st->has_pending && since_published(st, now) >= p->min_interval) {
        tdot_report_item_t item = st->pending;
        tdot_report_obs_t obs = st->pending_obs;
        st->has_pending = false; /* ownership moves to `item` */
        published = publish_if_changed(st, &item, &obs, now, out);
    }
    if (!published && p->debounce > 0 && st->has_candidate &&
        now - st->cand_since >= p->debounce) {
        tdot_report_item_t item = st->cand_latest;
        tdot_report_obs_t obs = st->cand_latest_obs;
        st->has_candidate = false; /* ownership moves to `item` */
        if (p->min_interval > 0 && since_published(st, now) < p->min_interval) {
            clear_pending(st);
            st->pending = item;
            st->pending_obs = obs;
            st->has_pending = true;
        } else {
            published = publish_if_changed(st, &item, &obs, now, out);
        }
    }
    return published;
}

bool tdot_report_due(tdot_report_state_t *st, int64_t now, bool pushed,
                     tdot_report_item_t *out, bool *read) {
    const tdot_report_policy_t *p = &st->policy;
    *read = false;
    if (st->replaying) {
        /* Replayed readings release what they hold themselves, and a replay
         * publishes, so no heartbeat read is due while it goes on. */
        if (now - st->replay_last_offer < TDOT_REPORT_REPLAY_IDLE_NS)
            return false;
        hand_over(st, now);
    }
    bool published = release(st, now, out);
    if (p->max_interval > 0) {
        int64_t published_at = st->has_published ? st->last_published : st->origin;
        int64_t read_at = st->has_read ? st->last_read : st->origin;
        int64_t reference = published_at > read_at ? published_at : read_at;
        int64_t idle = now > reference ? now - reference : 0;
        if (pushed && !st->unreadable && idle >= p->max_interval) {
            st->last_read = now;
            st->has_read = true;
            *read = true;
        }
    }
    return published;
}
