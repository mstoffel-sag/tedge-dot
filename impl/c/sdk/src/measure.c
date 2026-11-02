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
    bool has_value; /* the sample slot */
    double value;
    bool in_open; /* the point is in its open message */
} mpoint_t;

typedef struct {
    bool used; /* a message is open */
    size_t topic;
    tdot_group_by_t g;
    double time_ms;      /* the first reading's time */
    long long time_key;  /* time_ms at the contract's millisecond precision */
    long long opened_ns; /* when the frame opened, on the device clock */
    buf_t body;          /* "a":1,"b":2 without the braces */
    size_t *points;
    size_t npoints, cap;
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
    /* The shortest digits that read back as the same double ... */
    char sci[40];
    for (int prec = 0; prec <= 16; prec++) {
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
    unit_t *units = calloc(dev->npoints ? dev->npoints : 1, sizeof *units);
    size_t n = 0;
    for (size_t k = 0; units && k < dev->npoints; k++) {
        const tdot_point_t *pt = &dev->points[k];
        if (md->points[k].topic != t || !md->points[k].measurable || !pt->unit || !pt->unit[0])
            continue;
        const char *group = md->topics[t].group;
        size_t len = strlen(group) + strlen(series[k]) + 2;
        units[n].key = malloc(len);
        snprintf(units[n].key, len, "%s.%s", group, series[k]);
        units[n++].unit = pt->unit;
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
        for (size_t k = 0; k < md->npoints; k++)
            free(md->points[k].series_key);
        for (size_t t = 0; t < md->ntopics; t++) {
            free(md->topics[t].topic);
            free(md->topics[t].group);
            free(md->topics[t].group_json);
            free(md->topics[t].units);
        }
        for (size_t i = 0; md->open && i < md->nslots; i++) {
            free(md->open[i].body.s);
            free(md->open[i].points);
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

/* Open the message of `slot` with the first reading's time. */
static mopen_t *open_slot(mdevice_t *md, size_t slot, double time_ms, long long time_key,
                          long long opened_ns) {
    mopen_t *o = &md->open[slot];
    o->used = true;
    o->time_ms = time_ms;
    o->time_key = time_key;
    o->opened_ns = opened_ns;
    o->body.len = 0;
    if (o->body.s)
        o->body.s[0] = '\0';
    o->npoints = 0;
    return o;
}

/* Publish an open message and free its place. */
static void close_open(tdot_batcher_t *b, mdevice_t *md, mopen_t *o, tdot_measure_out_fn out,
                       void *ctx) {
    for (size_t i = 0; i < o->npoints; i++)
        md->points[o->points[i]].in_open = false;
    o->used = false;
    publish_body(b, &md->topics[o->topic], o->time_ms, o->body.s ? o->body.s : "",
                 o->body.len, out, ctx);
}

static long long window_ns(tdot_group_by_t g) { return g.ms * 1000000LL; }

static void add(tdot_batcher_t *b, mdevice_t *md, size_t k, double ts_ms, double value,
                long long now_ns, tdot_measure_out_fn out, void *ctx) {
    mpoint_t *p = &md->points[k];
    long long key = (long long)floor(ts_ms);
    /* The window a new message opens in: a repeated point's frame keeps its. */
    long long opened = now_ns;
    mopen_t *o = md->open[p->slot].used ? &md->open[p->slot] : NULL;
    if (o) {
        bool close;
        if (p->group_by.kind == TDOT_GROUP_FRAME) {
            if (o->opened_ns + window_ns(p->group_by) <= now_ns) {
                close = true;
            } else {
                close = p->in_open;
                if (close)
                    opened = o->opened_ns;
            }
        } else {
            close = p->in_open || o->time_key != key;
        }
        if (close) {
            close_open(b, md, o, out, ctx);
            o = NULL;
        }
    }
    if (!o)
        o = open_slot(md, p->slot, ts_ms, key, opened);
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
    size_t estimate = FRAME_BYTES + group_len + o->body.len + 1 + len +
                      TDOT_SERIES_OVERHEAD * (o->npoints + 1);
    if (o->npoints > 0 && estimate > TDOT_SIZE_BUDGET) {
        /* Split: the full part goes now; the rest keeps the message's time
         * and window. */
        double time_ms = o->time_ms;
        long long time_key = o->time_key, opened_ns = o->opened_ns;
        close_open(b, md, o, out, ctx);
        o = open_slot(md, p->slot, time_ms, time_key, opened_ns);
    }
    if (o->npoints == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 64;
        size_t *grown = realloc(o->points, cap * sizeof *grown);
        if (!grown) {
            free(dyn);
            return;
        }
        o->points = grown;
        o->cap = cap;
    }
    if (o->body.len > 0)
        buf_put(&o->body, ",", 1);
    buf_put(&o->body, text, len);
    o->points[o->npoints++] = k;
    p->in_open = true;
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
            p->has_value = false;
        return r;
    }
    r.measured = true;
    if (p->group_by.kind == TDOT_GROUP_SAMPLE) {
        p->has_value = true;
        p->value = value;
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
                          double wall_ms, tdot_measure_out_fn out, void *ctx) {
    if (md->stale)
        return;
    buf_t *bodies = calloc(md->ntopics ? md->ntopics : 1, sizeof *bodies);
    size_t *counts = calloc(md->ntopics ? md->ntopics : 1, sizeof *counts);
    if (!bodies || !counts) {
        free(bodies);
        free(counts);
        return;
    }
    for (size_t k = 0; k < md->npoints; k++) {
        mpoint_t *p = &md->points[k];
        if (p->group_by.kind != TDOT_GROUP_SAMPLE || p->group_by.ms != every_ms || !p->has_value)
            continue;
        char number[40];
        size_t numlen = tdot_format_number(p->value, number, sizeof number);
        size_t len = strlen(p->series_key) + numlen;
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
        buf_str(body, p->series_key);
        buf_put(body, number, numlen);
        (*n)++;
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
            sample_device(b, md, sm->every_ms, wall_ms, out, ctx);
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

void tdot_batcher_set_stale(tdot_batcher_t *b, size_t d, bool stale) {
    if (b && d < b->ndevices)
        b->devices[d].stale = stale;
}
