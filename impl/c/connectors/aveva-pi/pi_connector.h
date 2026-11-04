/* AVEVA PI connector -- the state the connector glue (connector_aveva_pi.c) and the push
 * worker (pi_push.c) share. Not a public header.
 */
#ifndef TDOT_PI_CONNECTOR_H
#define TDOT_PI_CONNECTOR_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "cjson/cJSON.h"
#include "pi_http.h"
#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/model.h"

#define PI_PROTOCOL "aveva-pi"

typedef enum {
    PI_ADDR_POINT = 0,  /* a PI point, by its full path \\server\tag */
    PI_ADDR_ATTRIBUTE,  /* an AF attribute, by its full path \\af\db\element|attribute */
    PI_ADDR_WEB_ID,     /* any stream, by WebId: never looked up */
} pi_addr_kind_t;

typedef enum {
    PI_DELIVERY_CHANNEL = 0,
    PI_DELIVERY_RECORDED,
    PI_DELIVERY_SNAPSHOT,
} pi_delivery_t;

/* pt->proto: the parsed address (flat, freed by the config). */
typedef struct {
    pi_addr_kind_t kind;
    /* pt->unit was set from PI by this module (spec §4 "Units"), not configured. */
    bool unit_from_pi;
    char text[]; /* the path or the WebId, as named in `not found:` errors and addr.path */
} pi_point_t;

/* A digital state. */
typedef struct {
    char *name;
    long long code;
} pi_digital_state_t;

/* A resolved point: the stream it reads and writes (resolve.rs `Stream`). */
typedef struct {
    bool resolved;
    char *web_id;
    char *point_type;  /* `Float64`, `Digital`, ... or the attribute's Type; NULL unknown */
    char *units;       /* EngineeringUnits / DefaultUnitsNameAbbreviation; NULL when empty */
    char *digital_set; /* DigitalSetName */
    bool has_states;   /* the digital set was found */
    pi_digital_state_t *states;
    size_t nstates;
    char *addr;        /* the addr echo of the last snapshot sample (it outlives read_points) */
} pi_stream_t;

struct pi_push;

/* A connected device (from connect_device to disconnect_device). */
typedef struct {
    pi_creds_t creds;
    atomic_size_t chunk;      /* WebIds per request, halved on a "too long" answer */
    atomic_bool chunk_warned;
    pi_client_t client;       /* the runtime thread's */
    pi_stream_t *streams;     /* one per dev->points[k] */
    double next_retry;        /* monotonic: when unresolved points are looked up again */
    bool fallback;            /* Channels refused: recorded until the next reconnect */
    bool resubscribe;         /* a pushed point resolved late: subscribe again */
    struct pi_push *push;
} pi_live_t;

/* dev->proto: the device's configuration (flat, freed by the config) and its live state. */
typedef struct {
    char base_url[1024]; /* without a trailing / */
    char data_server[256];
    char af_database[512];
    pi_delivery_t delivery;
    pi_settings_t settings;
    pi_live_t *live;     /* NULL while disconnected; freed by disconnect_device */
} pi_device_t;

const char *pi_delivery_name(pi_delivery_t d);

/* Build the sample of one PI value of a point (sample.rs `build`). `source_ms` is the PI time
 * of a pushed value (0 for a snapshot read, which carries the read time). *addr receives the
 * addr echo the sample points at; the caller frees it after the sample was handed on. */
void pi_build_sample(tdot_point_t *pt, const pi_stream_t *stream, const cJSON *pi,
                     double source_ms, bool replayed, tdot_sample_t *s, char **addr);

#endif
