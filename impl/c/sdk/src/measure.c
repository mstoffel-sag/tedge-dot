/* Measurement output (contract §5.5): the point tables and the batcher. See
 * tedge_dot/measure.h; the same rules as impl/rust/crates/sdk/src/measure.rs,
 * checked against the same vectors. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cjson/cJSON.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/measure.h"

/* `{"time":"2026-01-01T00:00:00.000Z",` + `:{` + `}}`, around the group name
 * and the series. */
#define FRAME_BYTES (9 + 24 + 2 + 2 + 2)

/* A frame holds at most this many layers; a reading that would open one more
 * publishes them and starts a new frame in the same window. */
#define MAX_LAYERS 64

typedef struct {
    char *s;
    size_t len, cap;
} buf_t;

static bool buf_put(buf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap)
            cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown)
            return false;
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
    return true;
}

static bool buf_str(buf_t *b, const char *s) { return buf_put(b, s, strlen(s)); }

typedef struct {
    char *topic;      /* te/device/<device>///m/<group> */
    char *group;      /* as resolved, to find the topic of a group */
    char *group_json; /* "<group>", escaped and quoted */
    char *units;      /* the retained units message; "" when no series has a unit */
    bool units_settled;
    long time_slot; /* the slot of the topic's `time` message, -1 when none */
} mtopic_t;

typedef struct {
    size_t topic;
    size_t slot;      /* the open message it goes into: one per topic and policy */
    char *series_key; /* "<series>": escaped and quoted, with the colon */
    bool measurable, sample;
    tdot_group_by_t group_by;
    bool has_value; /* the sample slot: the good value held */
    double value;
    /* Under "sample" with batch.aggregate: each aggregate and its series key
     * ("<series>": or "<series>_<aggregate>":); naggs 0 otherwise. */
    size_t naggs;
    tdot_aggregate_t aggs[TDOT_MAX_AGGREGATES];
    char *agg_keys[TDOT_MAX_AGGREGATES];
    /* The period so far, weighted by the device clock: since when the value is
     * held (or the sums were closed), the sum of value x nanoseconds, the good
     * nanoseconds, and the smallest and largest value held. */
    long long since_ns;
    double sum, good_ns;
    bool has_range;
    double lo, hi;
    /* The point's values in its open message (`time`) or frame (`frame`): the
     * layer its next value goes into. */
    unsigned char depth;
} mpoint_t;

/* One message being filled: the open `time` message, or a layer of a frame. */
typedef struct {
    double time_ms;     /* the layer's time */
    long long time_key; /* time_ms at the contract's millisecond precision */
    buf_t body;         /* "a":1,"b":2 without the braces: the part not yet published */
    size_t n;           /* the series in body */
    size_t *points;     /* every point in the layer, parts published by a split included */
    size_t npoints, cap;
} mlayer_t;

typedef struct {
    bool used; /* a message is open */
    size_t topic;
    tdot_group_by_t g;
    long long opened_ns; /* when the frame opened, on the device clock */
    mlayer_t *layers;    /* in order; one for a `time` slot */
    size_t nlayers, cap_layers;
    /* The time key of the slot's last layer, across frames: layer times
     * strictly increase, so a point's values stay in order. */
    bool has_last;
    long long last_key;
} mopen_t;

typedef struct {
    long long every_ms;
    long long next_ns;
} msampler_t;

typedef struct {
    mpoint_t *points;
    size_t npoints;
    mtopic_t *topics;
    size_t ntopics;
    mopen_t *open; /* the slots, one per topic and policy in use */
    size_t nslots;
    msampler_t *samplers;
    size_t nsamplers;
    bool stale;
} mdevice_t;

typedef struct {
    char *topic;
    char *payload;
} published_t;

struct tdot_batcher {
    mdevice_t *devices;
    size_t ndevices;
    bool include_boolean;
    /* The units message last published per meta topic, kept across reloads so
     * an unchanged one is not published again. */
    published_t *published;
    size_t npublished;
};

/* ---- formatting ------------------------------------------------------------ */

void tdot_format_rfc3339_ms(double ms, char *dst, size_t dstlen) {
    long long whole = (long long)floor(ms);
    time_t secs = (time_t)(whole >= 0 ? whole / 1000 : (whole - 999) / 1000);
    int millis = (int)(whole - (long long)secs * 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    size_t n = strftime(dst, dstlen, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(dst + n, dstlen - n, ".%03dZ", millis);
}

size_t tdot_format_number(double d, char *dst, size_t dstlen) {
    if (d == floor(d) && fabs(d) < 1e15)
        return (size_t)snprintf(dst, dstlen, "%lld", (long long)d);
    /* The shortest digits that read back as the same double ... Away from a
     * power of two the doubles that read back as `d` lie symmetrically around
     * it, and a precision that reads back makes every longer one read back
     * too, so a binary search finds the shortest in about five tries instead
     * of up to seventeen. At a power of two the gap below is half the gap
     * above, so a longer form can fall out; there every precision is tried. */
    char sci[40];
    int e2;
    bool power_of_two = fabs(frexp(d, &e2)) == 0.5;
    int lo = 0, hi = 16;
    while (!power_of_two && lo < hi) {
        int mid = (lo + hi) / 2;
        snprintf(sci, sizeof sci, "%.*e", mid, d);
        if (strtod(sci, NULL) == d)
            hi = mid;
        else
            lo = mid + 1;
    }
    for (int prec = lo; prec <= 16; prec++) {
        snprintf(sci, sizeof sci, "%.*e", prec, d);
        if (strtod(sci, NULL) == d)
            break;
    }
    const char *p = sci;
    bool negative = *p == '-';
    if (negative)
        p++;
    char digits[24];
    size_t nd = 0;
    for (; *p && *p != 'e'; p++)
        if (*p != '.' && nd < sizeof digits - 1)
            digits[nd++] = *p;
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    digits[nd] = '\0';
    int exp10 = *p == 'e' ? atoi(p + 1) : 0;
    /* ... laid out as ryu's pretty printer does: kk is where the decimal point
     * falls relative to the first digit, k the zeros after the digits. */
    int kk = exp10 + 1, k = kk - (int)nd;
    char out[64];
    size_t n = 0;
    if (negative)
        out[n++] = '-';
    if (k >= 0 && kk <= 16) {
        memcpy(out + n, digits, nd);
        n += nd;
        for (int i = 0; i < k; i++)
            out[n++] = '0';
        out[n++] = '.';
        out[n++] = '0';
    } else if (kk > 0 && kk <= 16) {
        memcpy(out + n, digits, (size_t)kk);
        n += (size_t)kk;
        out[n++] = '.';
        memcpy(out + n, digits + kk, nd - (size_t)kk);
        n += nd - (size_t)kk;
    } else if (kk > -5 && kk <= 0) {
        out[n++] = '0';
        out[n++] = '.';
        for (int i = 0; i < -kk; i++)
            out[n++] = '0';
        memcpy(out + n, digits, nd);
        n += nd;
    } else {
        out[n++] = digits[0];
        if (nd > 1) {
            out[n++] = '.';
            memcpy(out + n, digits + 1, nd - 1);
            n += nd - 1;
        }
        n += (size_t)snprintf(out + n, sizeof out - n, "e%d", kk - 1);
    }
    out[n] = '\0';
    return (size_t)snprintf(dst, dstlen, "%s", out);
}

/* `s` as a JSON string. */
static char *quoted(const char *s) {
    cJSON *str = cJSON_CreateString(s);
    char *out = cJSON_PrintUnformatted(str);
    cJSON_Delete(str);
    return out;
}

/* ---- the plan of each point -------------------------------------------------- */

/* A non-empty string `meta.measurement.<key>`, or NULL. */
static const char *named(const cJSON *meta, const char *key) {
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(meta, "measurement");
    const cJSON *v = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, key) : NULL;
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

/* A meta key that is present, not null and not false. */
static bool flagged(const cJSON *meta, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(meta, key);
    return v && !cJSON_IsNull(v) && !cJSON_IsFalse(v);
}

bool tdot_publishes_sample(const tdot_point_t *pt) {
    if (pt->sample >= 0)
        return pt->sample == 1;
    if (pt->access & TDOT_ACCESS_WRITE)
        return true;
    cJSON *meta = pt->meta_json ? cJSON_Parse(pt->meta_json) : NULL;
    bool yes = flagged(meta, "alarm") || flagged(meta, "event") || flagged(meta, "parameter");
    cJSON_Delete(meta);
    return yes;
}

/* A point's group and series, in ot-measurement's order: meta.measurement,
 * then the connector's group/series, then a split of the point id at the
 * separator, then the protocol and the point id. Caller frees both. */
static void resolve_naming(const tdot_config_t *cfg, const tdot_point_t *pt,
                           const cJSON *meta, char **group, char **series) {
    const char *g = named(meta, "group");
    const char *s = named(meta, "series");
    if (!g && cfg->measurement_group && cfg->measurement_group[0])
        g = cfg->measurement_group;
    if (!s && cfg->measurement_series && cfg->measurement_series[0])
        s = cfg->measurement_series;
    *group = g ? strdup(g) : NULL;
    *series = s ? strdup(s) : NULL;
    const char *sep = cfg->measurement_separator;
    if (sep && sep[0] && (!*group || !*series)) {
        const char *at = strstr(pt->id, sep);
        size_t len = strlen(pt->id), seplen = strlen(sep);
        if (at && at > pt->id && (size_t)(at - pt->id) + seplen < len) {
            if (!*group)
                *group = strndup(pt->id, (size_t)(at - pt->id));
            if (!*series)
                *series = strdup(at + seplen);
        }
    }
    if (!*group)
        *group = strdup(cfg->protocol);
    if (!*series)
        *series = strdup(pt->id);
}

typedef struct {
    char *key;
    const char *unit;
} unit_t;

static int by_key(const void *a, const void *b) {
    return strcmp(((const unit_t *)a)->key, ((const unit_t *)b)->key);
}

/* The retained units message of topic `t`: every measurable point of it with a
 * unit, by "<group>.<series>" in byte order. "" when there is none. */
static char *units_of(const tdot_device_t *dev, const mdevice_t *md, size_t t,
                      char *const *series) {
    unit_t *units = calloc(dev->npoints ? dev->npoints * TDOT_MAX_AGGREGATES : 1, sizeof *units);
    size_t n = 0;
    for (size_t k = 0; units && k < dev->npoints; k++) {
        const tdot_point_t *pt = &dev->points[k];
        const mpoint_t *p = &md->points[k];
        if (p->topic != t || !p->measurable || !pt->unit || !pt->unit[0])
            continue;
        const char *group = md->topics[t].group;
        bool suffixed = p->naggs > 0 && pt->aggregate_suffixed;
        for (size_t a = 0; a < (suffixed ? p->naggs : 1); a++) {
            const char *agg = suffixed ? tdot_aggregate_name(p->aggs[a]) : NULL;
            size_t len = strlen(group) + strlen(series[k]) + (agg ? strlen(agg) + 1 : 0) + 2;
            units[n].key = malloc(len);
            if (agg)
                snprintf(units[n].key, len, "%s.%s_%s", group, series[k], agg);
            else
                snprintf(units[n].key, len, "%s.%s", group, series[k]);
            units[n++].unit = pt->unit;
        }
    }
    char *out;
    if (n == 0) {
        out = strdup("");
    } else {
        qsort(units, n, sizeof *units, by_key);
        cJSON *obj = cJSON_CreateObject();
        for (size_t i = 0; i < n; i++) {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "unit", units[i].unit);
            cJSON_AddItemToObject(obj, units[i].key, u);
        }
        out = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
    }
    for (size_t i = 0; i < n; i++)
        free(units[i].key);
    free(units);
    return out;
}

static bool build_device(mdevice_t *md, const tdot_config_t *cfg, const tdot_device_t *dev,
                         long long now_ns) {
    md->points = calloc(dev->npoints ? dev->npoints : 1, sizeof *md->points);
    md->topics = calloc(dev->npoints ? dev->npoints : 1, sizeof *md->topics);
    md->open = calloc(dev->npoints ? dev->npoints : 1, sizeof *md->open);
    md->samplers = calloc(dev->npoints ? dev->npoints : 1, sizeof *md->samplers);
    char **series = calloc(dev->npoints ? dev->npoints : 1, sizeof *series);
    if (!md->points || !md->topics || !md->open || !md->samplers || !series) {
        free(series);
        return false;
    }
    md->npoints = dev->npoints;
    for (size_t k = 0; k < dev->npoints; k++) {
        const tdot_point_t *pt = &dev->points[k];
        mpoint_t *p = &md->points[k];
        cJSON *meta = pt->meta_json ? cJSON_Parse(pt->meta_json) : NULL;
        char *group;
        resolve_naming(cfg, pt, meta, &group, &series[k]);
        size_t t = 0;
        while (t < md->ntopics && strcmp(md->topics[t].group, group) != 0)
            t++;
        if (t == md->ntopics) {
            mtopic_t *topic = &md->topics[md->ntopics++];
            size_t len = strlen(dev->name) + strlen(group) + 20;
            topic->topic = malloc(len);
            snprintf(topic->topic, len, "te/device/%s///m/%s", dev->name, group);
            topic->group = group;
            topic->group_json = quoted(group);
            topic->time_slot = -1;
        } else {
            free(group);
        }
        p->topic = t;
        char *q = quoted(series[k]);
        size_t len = strlen(q) + 2;
        p->series_key = malloc(len);
        snprintf(p->series_key, len, "%s:", q);
        free(q);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(meta, "measurement");
        p->measurable = !cJSON_IsFalse(m) && pt->mode == TDOT_MODE_TYPED;
        p->sample = tdot_publishes_sample(pt);
        p->group_by = pt->group_by;
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(meta, "event");
        bool every_event = cJSON_IsObject(event) &&
                           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(event, "every"));
        if (p->group_by.kind == TDOT_GROUP_SAMPLE && every_event) {
            p->group_by.kind = TDOT_GROUP_TIME;
            p->group_by.ms = 0;
        }
        p->since_ns = now_ns;
        if (p->group_by.kind == TDOT_GROUP_SAMPLE)
            for (size_t a = 0; a < pt->naggregates; a++) {
                p->aggs[a] = pt->aggregates[a];
                if (pt->aggregate_suffixed) {
                    const char *agg = tdot_aggregate_name(pt->aggregates[a]);
                    size_t nlen = strlen(series[k]) + strlen(agg) + 2;
                    char *name = malloc(nlen);
                    snprintf(name, nlen, "%s_%s", series[k], agg);
                    char *qn = quoted(name);
                    size_t klen = strlen(qn) + 2;
                    p->agg_keys[a] = malloc(klen);
                    snprintf(p->agg_keys[a], klen, "%s:", qn);
                    free(qn);
                    free(name);
                } else {
                    p->agg_keys[a] = strdup(p->series_key);
                }
                p->naggs++;
            }
        if (p->group_by.kind == TDOT_GROUP_SAMPLE) {
            size_t i = 0;
            while (i < md->nsamplers && md->samplers[i].every_ms != p->group_by.ms)
                i++;
            if (i == md->nsamplers) {
                md->samplers[i].every_ms = p->group_by.ms;
                md->samplers[i].next_ns = now_ns + p->group_by.ms * 1000000LL;
                md->nsamplers++;
            }
        }
        size_t slot = 0;
        while (slot < md->nslots && !(md->open[slot].topic == t &&
                                      md->open[slot].g.kind == p->group_by.kind &&
                                      md->open[slot].g.ms == p->group_by.ms))
            slot++;
        if (slot == md->nslots) {
            md->open[slot].topic = t;
            md->open[slot].g = p->group_by;
            md->nslots++;
            if (p->group_by.kind == TDOT_GROUP_TIME)
                md->topics[t].time_slot = (long)slot;
        }
        p->slot = slot;
        cJSON_Delete(meta);
    }
    for (size_t t = 0; t < md->ntopics; t++)
        md->topics[t].units = units_of(dev, md, t, series);
    for (size_t k = 0; k < dev->npoints; k++)
        free(series[k]);
    free(series);
    return true;
}

tdot_batcher_t *tdot_batcher_new(const tdot_config_t *cfg, long long now_ns,
                                 tdot_batcher_t *previous) {
    tdot_batcher_t *b = calloc(1, sizeof *b);
    if (!b) {
        tdot_batcher_free(previous);
        return NULL;
    }
    if (previous) {
        b->published = previous->published;
        b->npublished = previous->npublished;
        previous->published = NULL;
        previous->npublished = 0;
        tdot_batcher_free(previous);
    }
    b->include_boolean = cfg->include_boolean;
    b->devices = calloc(cfg->ndevices ? cfg->ndevices : 1, sizeof *b->devices);
    if (!b->devices) {
        tdot_batcher_free(b);
        return NULL;
    }
    b->ndevices = cfg->ndevices;
    for (size_t d = 0; d < cfg->ndevices; d++)
        if (!build_device(&b->devices[d], cfg, &cfg->devices[d], now_ns)) {
            tdot_batcher_free(b);
            return NULL;
        }
    return b;
}

void tdot_batcher_free(tdot_batcher_t *b) {
    if (!b)
        return;
    for (size_t d = 0; d < b->ndevices; d++) {
        mdevice_t *md = &b->devices[d];
        for (size_t k = 0; k < md->npoints; k++) {
            free(md->points[k].series_key);
            for (size_t a = 0; a < md->points[k].naggs; a++)
                free(md->points[k].agg_keys[a]);
        }
        for (size_t t = 0; t < md->ntopics; t++) {
            free(md->topics[t].topic);
            free(md->topics[t].group);
            free(md->topics[t].group_json);
            free(md->topics[t].units);
        }
        for (size_t i = 0; md->open && i < md->nslots; i++) {
            for (size_t l = 0; l < md->open[i].cap_layers; l++) {
                free(md->open[i].layers[l].body.s);
                free(md->open[i].layers[l].points);
            }
            free(md->open[i].layers);
        }
        free(md->points);
        free(md->topics);
        free(md->open);
        free(md->samplers);
    }
    free(b->devices);
    for (size_t i = 0; i < b->npublished; i++) {
        free(b->published[i].topic);
        free(b->published[i].payload);
    }
    free(b->published);
    free(b);
}

/* ---- messages ------------------------------------------------------------------ */

/* Publish a topic's units before its first measurement, when they differ from
 * what was last published for it. An emptied map clears the retained message. */
static void settle_units(tdot_batcher_t *b, mtopic_t *topic, tdot_measure_out_fn out,
                         void *ctx) {
    if (topic->units_settled)
        return;
    topic->units_settled = true;
    char meta[512];
    snprintf(meta, sizeof meta, "%s/meta", topic->topic);
    published_t *before = NULL;
    for (size_t i = 0; i < b->npublished && !before; i++)
        if (strcmp(b->published[i].topic, meta) == 0)
            before = &b->published[i];
    if (before ? strcmp(before->payload, topic->units) == 0 : topic->units[0] == '\0')
        return;
    if (before) {
        free(before->payload);
        before->payload = strdup(topic->units);
    } else {
        published_t *grown = realloc(b->published, (b->npublished + 1) * sizeof *grown);
        if (!grown)
            return;
        b->published = grown;
        b->published[b->npublished].topic = strdup(meta);
        b->published[b->npublished++].payload = strdup(topic->units);
    }
    out(ctx, meta, topic->units, true);
}

/* Publish `body` (the series, without braces) on topic `t` with `time_ms`. */
static void publish_body(tdot_batcher_t *b, mtopic_t *topic, double time_ms,
                         const char *body, size_t len, tdot_measure_out_fn out, void *ctx) {
    settle_units(b, topic, out, ctx);
    char time[40];
    tdot_format_rfc3339_ms(time_ms, time, sizeof time);
    buf_t msg = {0};
    if (buf_str(&msg, "{\"time\":\"") && buf_str(&msg, time) && buf_str(&msg, "\",") &&
        buf_str(&msg, topic->group_json) && buf_str(&msg, ":{") && buf_put(&msg, body, len) &&
        buf_str(&msg, "}}"))
        out(ctx, topic->topic, msg.s, false);
    free(msg.s);
}

/* Start a layer of slot `o` at `time_ms`; NULL when out of memory. */
static mlayer_t *open_layer(mopen_t *o, double time_ms, long long time_key) {
    if (o->nlayers == o->cap_layers) {
        size_t cap = o->cap_layers ? o->cap_layers * 2 : 1;
        mlayer_t *grown = realloc(o->layers, cap * sizeof *grown);
        if (!grown)
            return NULL;
        memset(grown + o->cap_layers, 0, (cap - o->cap_layers) * sizeof *grown);
        o->layers = grown;
        o->cap_layers = cap;
    }
    mlayer_t *l = &o->layers[o->nlayers++];
    l->time_ms = time_ms;
    l->time_key = time_key;
    l->body.len = 0;
    if (l->body.s)
        l->body.s[0] = '\0';
    l->n = 0;
    l->npoints = 0;
    return l;
}

/* Publish an open message, or every layer of a frame, and free its place. */
static void close_open(tdot_batcher_t *b, mdevice_t *md, mopen_t *o, tdot_measure_out_fn out,
                       void *ctx) {
    o->used = false;
    for (size_t i = 0; i < o->nlayers; i++) {
        mlayer_t *l = &o->layers[i];
        for (size_t j = 0; j < l->npoints; j++)
            md->points[l->points[j]].depth = 0;
        if (l->n > 0)
            publish_body(b, &md->topics[o->topic], l->time_ms, l->body.s ? l->body.s : "",
                         l->body.len, out, ctx);
    }
    o->nlayers = 0;
}

static long long window_ns(tdot_group_by_t g) { return g.ms * 1000000LL; }

/* Add the held value's time up to `now_ns` to the period's sums. */
static void close_held(mpoint_t *p, long long now_ns) {
    if (p->has_value && now_ns > p->since_ns) {
        double ns = (double)(now_ns - p->since_ns);
        p->sum += p->value * ns;
        p->good_ns += ns;
    }
    p->since_ns = now_ns;
}

/* Hold `value` from `now_ns` on (has false: nothing good is held). A plain
 * slot only keeps the value, as sample-and-hold. */
static void hold(mpoint_t *p, bool has, double value, long long now_ns) {
    if (p->naggs > 0)
        close_held(p, now_ns);
    p->has_value = has;
    p->value = value;
    if (has && p->naggs > 0) {
        if (!p->has_range) {
            p->lo = p->hi = value;
            p->has_range = true;
        } else {
            p->lo = value < p->lo ? value : p->lo;
            p->hi = value > p->hi ? value : p->hi;
        }
    }
}

static void add(tdot_batcher_t *b, mdevice_t *md, size_t k, double ts_ms, double value,
                long long now_ns, tdot_measure_out_fn out, void *ctx) {
    mpoint_t *p = &md->points[k];
    long long key = (long long)floor(ts_ms);
    bool frame = p->group_by.kind == TDOT_GROUP_FRAME;
    /* The window a new frame opens in: a frame that reached its layer cap keeps
     * its window. */
    long long opened = now_ns;
    mopen_t *o = &md->open[p->slot];
    if (o->used) {
        bool close;
        if (frame) {
            if (o->opened_ns + window_ns(p->group_by) <= now_ns) {
                close = true;
            } else {
                close = p->depth >= MAX_LAYERS;
                if (close)
                    opened = o->opened_ns;
            }
        } else {
            close = p->depth > 0 || o->layers[0].time_key != key;
        }
        if (close)
            close_open(b, md, o, out, ctx);
    }
    if (!o->used) {
        o->used = true;
        o->opened_ns = opened;
        o->nlayers = 0;
    }
    if (p->depth == o->nlayers) {
        double time_ms = ts_ms;
        long long time_key = key;
        if (frame) {
            if (o->has_last && time_key <= o->last_key) {
                time_key = o->last_key + 1;
                time_ms = (double)time_key;
            }
            o->has_last = true;
            o->last_key = time_key;
        }
        if (!open_layer(o, time_ms, time_key))
            return;
    }
    mlayer_t *l = &o->layers[p->depth];
    char piece[600];
    size_t keylen = strlen(p->series_key);
    char number[40];
    size_t numlen = tdot_format_number(value, number, sizeof number);
    size_t len = keylen + numlen;
    char *dyn = NULL;
    char *text = piece;
    if (len + 1 > sizeof piece) {
        dyn = malloc(len + 1);
        if (!dyn)
            return;
        text = dyn;
    }
    memcpy(text, p->series_key, keylen);
    memcpy(text + keylen, number, numlen + 1);
    size_t group_len = strlen(md->topics[p->topic].group_json);
    size_t estimate = FRAME_BYTES + group_len + l->body.len + 1 + len +
                      TDOT_SERIES_OVERHEAD * (l->n + 1);
    if (l->n > 0 && estimate > TDOT_SIZE_BUDGET) {
        /* Split: the full part goes now; the rest keeps the layer's time and
         * the frame's window. */
        publish_body(b, &md->topics[o->topic], l->time_ms, l->body.s, l->body.len, out, ctx);
        l->body.len = 0;
        l->body.s[0] = '\0';
        l->n = 0;
    }
    if (l->npoints == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        size_t *grown = realloc(l->points, cap * sizeof *grown);
        if (!grown) {
            free(dyn);
            return;
        }
        l->points = grown;
        l->cap = cap;
    }
    if (l->body.len > 0)
        buf_put(&l->body, ",", 1);
    buf_put(&l->body, text, len);
    l->n++;
    l->points[l->npoints++] = k;
    p->depth++;
    free(dyn);
}

tdot_routing_t tdot_batcher_offer(tdot_batcher_t *b, size_t d, size_t k,
                                  const tdot_point_t *pt, const tdot_sample_t *s,
                                  double ts_ms, long long now_ns, tdot_measure_out_fn out,
                                  void *ctx) {
    tdot_routing_t r = {.sample = true, .measured = false};
    if (!b || d >= b->ndevices || k >= b->devices[d].npoints)
        return r; /* not configured: as before */
    mdevice_t *md = &b->devices[d];
    mpoint_t *p = &md->points[k];
    r.sample = p->sample;
    bool has = false;
    double value = 0;
    if (p->measurable && s->quality == TDOT_Q_GOOD && pt->mode == TDOT_MODE_TYPED) {
        if (s->value.kind == TDOT_VAL_NUM && isfinite(s->value.num)) {
            has = true;
            value = s->value.num;
        } else if (s->value.kind == TDOT_VAL_BOOL && b->include_boolean) {
            has = true;
            value = s->value.b ? 1 : 0;
        }
    }
    if (!has) {
        if (s->quality != TDOT_Q_GOOD)
            hold(p, false, 0, now_ns);
        return r;
    }
    r.measured = true;
    if (p->group_by.kind == TDOT_GROUP_SAMPLE) {
        hold(p, true, value, now_ns);
    } else {
        add(b, md, k, ts_ms, value, now_ns, out, ctx);
    }
    return r;
}

void tdot_batcher_end_of_pass(tdot_batcher_t *b, tdot_measure_out_fn out, void *ctx) {
    if (!b)
        return;
    for (size_t d = 0; d < b->ndevices; d++) {
        mdevice_t *md = &b->devices[d];
        for (size_t t = 0; t < md->ntopics; t++) {
            long slot = md->topics[t].time_slot;
            if (slot >= 0 && md->open[slot].used)
                close_open(b, md, &md->open[slot], out, ctx);
        }
    }
}

void tdot_batcher_flush_all(tdot_batcher_t *b, tdot_measure_out_fn out, void *ctx) {
    if (!b)
        return;
    for (size_t d = 0; d < b->ndevices; d++) {
        mdevice_t *md = &b->devices[d];
        for (size_t t = 0; t < md->ntopics; t++)
            for (size_t i = 0; i < md->nslots; i++)
                if (md->open[i].used && md->open[i].topic == t)
                    close_open(b, md, &md->open[i], out, ctx);
    }
}

/* Publish one sample of device `md`'s slots sampled every `every_ms`. */
static void sample_device(tdot_batcher_t *b, mdevice_t *md, long long every_ms,
                          long long now_ns, double wall_ms, tdot_measure_out_fn out,
                          void *ctx) {
    buf_t *bodies = calloc(md->ntopics ? md->ntopics : 1, sizeof *bodies);
    size_t *counts = calloc(md->ntopics ? md->ntopics : 1, sizeof *counts);
    if (!bodies || !counts) {
        free(bodies);
        free(counts);
        return;
    }
    for (size_t k = 0; k < md->npoints; k++) {
        mpoint_t *p = &md->points[k];
        if (p->group_by.kind != TDOT_GROUP_SAMPLE || p->group_by.ms != every_ms)
            continue;
        const char *keys[TDOT_MAX_AGGREGATES];
        double values[TDOT_MAX_AGGREGATES];
        size_t nseries = 0;
        if (p->naggs == 0) {
            if (!p->has_value || md->stale)
                continue;
            keys[0] = p->series_key;
            values[nseries++] = p->value;
        } else {
            /* A stale device's aggregated slot publishes the good time it had. */
            close_held(p, now_ns);
            for (size_t a = 0; p->good_ns > 0 && a < p->naggs; a++) {
                bool ok = true;
                double v = 0;
                switch (p->aggs[a]) {
                case TDOT_AGG_MEAN:
                    v = p->sum / p->good_ns;
                    break;
                case TDOT_AGG_MIN:
                    ok = p->has_range;
                    v = p->lo;
                    break;
                case TDOT_AGG_MAX:
                    ok = p->has_range;
                    v = p->hi;
                    break;
                case TDOT_AGG_LAST:
                    ok = p->has_value;
                    v = p->value;
                    break;
                }
                if (ok) {
                    keys[nseries] = p->agg_keys[a];
                    values[nseries++] = v;
                }
            }
            /* The next period starts with the value held now. */
            p->sum = 0;
            p->good_ns = 0;
            p->has_range = p->has_value;
            p->lo = p->hi = p->value;
        }
        for (size_t i = 0; i < nseries; i++) {
            char number[40];
            size_t numlen = tdot_format_number(values[i], number, sizeof number);
            size_t len = strlen(keys[i]) + numlen;
            mtopic_t *topic = &md->topics[p->topic];
            buf_t *body = &bodies[p->topic];
            size_t *n = &counts[p->topic];
            size_t size = FRAME_BYTES + strlen(topic->group_json) + body->len + 1 + len +
                          TDOT_SERIES_OVERHEAD * (*n + 1);
            if (*n > 0 && size > TDOT_SIZE_BUDGET) {
                publish_body(b, topic, wall_ms, body->s, body->len, out, ctx);
                body->len = 0;
                *n = 0;
            }
            if (body->len > 0)
                buf_put(body, ",", 1);
            buf_str(body, keys[i]);
            buf_put(body, number, numlen);
            (*n)++;
        }
    }
    for (size_t t = 0; t < md->ntopics; t++) {
        if (counts[t] > 0)
            publish_body(b, &md->topics[t], wall_ms, bodies[t].s, bodies[t].len, out, ctx);
        free(bodies[t].s);
    }
    free(bodies);
    free(counts);
}

typedef struct {
    long long opened_ns;
    size_t topic;
    mopen_t *open;
} ended_t;

static int by_opened(const void *a, const void *b) {
    const ended_t *x = a, *y = b;
    if (x->opened_ns != y->opened_ns)
        return x->opened_ns < y->opened_ns ? -1 : 1;
    return x->topic < y->topic ? -1 : x->topic > y->topic;
}

void tdot_batcher_due(tdot_batcher_t *b, long long now_ns, double wall_ms,
                      tdot_measure_out_fn out, void *ctx) {
    if (!b)
        return;
    for (size_t d = 0; d < b->ndevices; d++) {
        mdevice_t *md = &b->devices[d];
        size_t slots = md->nslots;
        ended_t ended[slots ? slots : 1];
        size_t n = 0;
        for (size_t i = 0; i < slots; i++) {
            mopen_t *o = &md->open[i];
            if (o->used && o->g.kind == TDOT_GROUP_FRAME &&
                o->opened_ns + window_ns(o->g) <= now_ns)
                ended[n++] = (ended_t){o->opened_ns, o->topic, o};
        }
        qsort(ended, n, sizeof *ended, by_opened);
        for (size_t i = 0; i < n; i++)
            close_open(b, md, ended[i].open, out, ctx);
        for (size_t i = 0; i < md->nsamplers; i++) {
            msampler_t *sm = &md->samplers[i];
            if (sm->next_ns > now_ns)
                continue;
            sample_device(b, md, sm->every_ms, now_ns, wall_ms, out, ctx);
            /* On the period's grid; a sampler that fell a whole period behind
             * restarts. */
            long long every_ns = sm->every_ms * 1000000LL;
            sm->next_ns += every_ns;
            if (sm->next_ns <= now_ns)
                sm->next_ns = now_ns + every_ns;
        }
    }
}

long long tdot_batcher_next_deadline(const tdot_batcher_t *b) {
    long long best = -1;
    for (size_t d = 0; b && d < b->ndevices; d++) {
        const mdevice_t *md = &b->devices[d];
        for (size_t i = 0; i < md->nslots; i++) {
            const mopen_t *o = &md->open[i];
            if (!o->used || o->g.kind != TDOT_GROUP_FRAME)
                continue;
            long long at = o->opened_ns + window_ns(o->g);
            if (best < 0 || at < best)
                best = at;
        }
        for (size_t i = 0; i < md->nsamplers; i++)
            if (best < 0 || md->samplers[i].next_ns < best)
                best = md->samplers[i].next_ns;
    }
    return best;
}

void tdot_batcher_set_stale(tdot_batcher_t *b, size_t d, bool stale, long long now_ns) {
    if (!b || d >= b->ndevices)
        return;
    mdevice_t *md = &b->devices[d];
    if (stale && !md->stale)
        for (size_t k = 0; k < md->npoints; k++)
            if (md->points[k].naggs > 0)
                hold(&md->points[k], false, 0, now_ns);
    md->stale = stale;
}

int tdot_measure_check_series(const tdot_config_t *cfg, const tdot_device_t *dev, char *err,
                              size_t errlen) {
    size_t cap = dev->npoints * TDOT_MAX_AGGREGATES + 1, n = 0;
    struct {
        char *group, *series;
        const char *point;
        bool aggregated;
    } *names = calloc(cap, sizeof *names);
    if (!names)
        return 0;
    int rc = 0;
    for (size_t k = 0; rc == 0 && k < dev->npoints; k++) {
        const tdot_point_t *pt = &dev->points[k];
        cJSON *meta = pt->meta_json ? cJSON_Parse(pt->meta_json) : NULL;
        bool measurable = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(meta, "measurement")) &&
                          pt->mode == TDOT_MODE_TYPED;
        char *group, *series;
        resolve_naming(cfg, pt, meta, &group, &series);
        cJSON_Delete(meta);
        bool aggregated = pt->naggregates > 0;
        size_t count = aggregated && pt->aggregate_suffixed ? pt->naggregates : 1;
        for (size_t a = 0; measurable && rc == 0 && a < count; a++) {
            char name[512];
            if (aggregated && pt->aggregate_suffixed)
                snprintf(name, sizeof name, "%s_%s", series, tdot_aggregate_name(pt->aggregates[a]));
            else
                snprintf(name, sizeof name, "%s", series);
            size_t i = 0;
            while (i < n && !(strcmp(names[i].group, group) == 0 && strcmp(names[i].series, name) == 0))
                i++;
            if (i < n) {
                if (aggregated || names[i].aggregated) {
                    snprintf(err, errlen,
                             "device '%s': points '%s' and '%s' both publish the series '%s' in the "
                             "group '%s'",
                             dev->name, names[i].point, pt->id, name, group);
                    rc = -1;
                }
                continue;
            }
            names[n].group = strdup(group);
            names[n].series = strdup(name);
            names[n].point = pt->id;
            names[n++].aggregated = aggregated;
        }
        free(group);
        free(series);
    }
    for (size_t i = 0; i < n; i++) {
        free(names[i].group);
        free(names[i].series);
    }
    free(names);
    return rc;
}
