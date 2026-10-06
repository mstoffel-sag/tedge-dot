#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cjson/cJSON.h"
#include "tedge_dot/decode.h"
#include "tedge_dot/map.h"
#include "tedge_dot/runtime.h"

double tdot_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double tdot_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

void tdot_now_rfc3339(char *dst, size_t dstlen) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    size_t n = strftime(dst, dstlen, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(dst + n, dstlen - n, ".%03dZ", (int)(tv.tv_usec / 1000));
}

/* `v` as the envelope's `<field>` and `<field>_repr`. */
static void add_value_as(cJSON *obj, const tdot_value_t *v, const char *field) {
    char repr[64];
    snprintf(repr, sizeof repr, "%s_repr", field);
    switch (v->kind) {
    case TDOT_VAL_BOOL:
        cJSON_AddBoolToObject(obj, field, v->b);
        cJSON_AddStringToObject(obj, repr, "boolean");
        break;
    case TDOT_VAL_NUM:
        cJSON_AddNumberToObject(obj, field, v->num);
        cJSON_AddStringToObject(obj, repr, "number");
        break;
    case TDOT_VAL_STR:
        cJSON_AddStringToObject(obj, field, v->str);
        cJSON_AddStringToObject(obj, repr, "string");
        break;
    default:
        break;
    }
}

static void add_value(cJSON *obj, const tdot_value_t *v) { add_value_as(obj, v, "value"); }

void tdot_sample_apply_map(const tdot_point_t *pt, tdot_sample_t *s) {
    if (!pt->map || pt->mode != TDOT_MODE_TYPED || s->value.kind == TDOT_VAL_NONE ||
        s->quality == TDOT_Q_BAD)
        return;
    tdot_value_t mapped;
    char why[TDOT_ERR_MAX];
    s->source = s->value;
    if (tdot_map_apply(pt->map, &s->value, pt->datatype, &mapped, why, sizeof why) == 0) {
        s->value = mapped;
    } else {
        s->quality = TDOT_Q_BAD;
        s->value.kind = TDOT_VAL_NONE;
        snprintf(s->error, sizeof s->error, "point %s: %s", pt->id, why);
    }
}

char *tdot_envelope_sample(const tdot_config_t *cfg, const tdot_device_t *dev,
                           const tdot_point_t *pt, const tdot_sample_t *s) {
    char ts[40];
    tdot_now_rfc3339(ts, sizeof ts);
    return tdot_envelope_sample_at(cfg, dev, pt, s, ts, tdot_now_ms());
}

char *tdot_envelope_sample_at(const tdot_config_t *cfg, const tdot_device_t *dev,
                              const tdot_point_t *pt, const tdot_sample_t *s,
                              const char *ts, double ts_ms) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ts", ts);
    cJSON_AddNumberToObject(obj, "ts_ms", ts_ms);
    cJSON_AddStringToObject(obj, "device", dev->name);
    /* The device type (§3.1), when declared: what a consumer needs to name the
     * point's parameter set without reading the configuration file (§5.2). */
    if (dev->type)
        cJSON_AddStringToObject(obj, "type", dev->type);
    cJSON_AddStringToObject(obj, "protocol", cfg->protocol);
    cJSON_AddStringToObject(obj, "point", pt->id);
    bool raw_mode = pt->mode == TDOT_MODE_RAW;
    cJSON_AddStringToObject(obj, "mode", raw_mode ? "raw" : "typed");
    if (!raw_mode && pt->datatype != TDOT_DT_NONE)
        cJSON_AddStringToObject(obj, "datatype",
                                tdot_datatype_str(pt->datatype));
    /* raw mode publishes the wire bytes only: no decoded value (contract §5) */
    if (!raw_mode && s->quality != TDOT_Q_BAD)
        add_value(obj, &s->value);

    char hex[TDOT_RAW_MAX * 3 + 1];
    tdot_hex_format(s->raw, s->raw_len, s->raw_group, hex, sizeof hex);
    cJSON_AddStringToObject(obj, "raw", hex);
    cJSON_AddStringToObject(obj, "quality", tdot_quality_str(s->quality));
    if (pt->unit)
        cJSON_AddStringToObject(obj, "unit", pt->unit);
    /* Declared access, so flows can tell writable points (parameters) apart. */
    cJSON_AddStringToObject(obj, "access",
                            (pt->access & TDOT_ACCESS_WRITE)
                                ? ((pt->access & TDOT_ACCESS_READ) ? "read_write"
                                                                   : "write")
                                : "read");
    /* A sample's own address echo wins over the point's static one. */
    const char *addr_json = s->addr_json ? s->addr_json : pt->addr_json;
    if (addr_json) {
        cJSON *addr = cJSON_Parse(addr_json);
        if (addr)
            cJSON_AddItemToObject(obj, "addr", addr);
    }
    cJSON_AddNumberToObject(obj, "seq", (double)pt->seq);
    if (s->quality == TDOT_Q_BAD)
        cJSON_AddStringToObject(obj, "error", s->error);
    /* The value before the point's map (§4.3); only mapped samples carry it. */
    if (!raw_mode)
        add_value_as(obj, &s->source, "source_value");
    if (pt->meta_json) {
        cJSON *meta = cJSON_Parse(pt->meta_json);
        if (meta)
            cJSON_AddItemToObject(obj, "meta", meta);
    }
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}
