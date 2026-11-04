/* tedge-dot -- AVEVA PI Web API connector (doc/connectors/aveva-pi-connector-spec.md), on
 * libcurl. Mirrors impl/rust/crates/connector-aveva-pi, in the same layers:
 *
 *  - pi_http.c: HTTP and the Channels WebSocket (TLS, authentication, chunking, PI errors);
 *  - pi_value.c: the pure decoder of a PI value, checked against the shared golden vectors;
 *  - pi_push.c, pi_cursor.c, pi_ring.c: push delivery, recorded polling and outage backfill
 *    on a worker thread per subscribed device (design D9);
 *  - this file: the configuration, WebId resolution and the connector vtable.
 *
 * Snapshot reads (read_points) and writes run on the runtime thread: the chunks of one
 * read_points call run in parallel on one curl multi handle, so the call takes at most one
 * request_timeout. All push traffic runs on the device's worker, which hands the runtime its
 * events through a ring that drain_subscriptions empties.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cjson/cJSON.h"
#include "pi_connector.h"
#include "pi_http.h"
#include "pi_log.h"
#include "pi_push.h"
#include "pi_value.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/runtime.h"

/* Keys a management command may not add, change or remove (spec §3.1): they name files on the
 * gateway or relax security. Mirrors config.rs LOCAL_ONLY. */
static const char *const LOCAL_ONLY_SETTINGS[] = {
    "password_file", "token_file", "cert_file", "key_file", "ca_file", "insecure_skip_verify",
    "state_dir",     NULL,
};

/* The runtime adds write-batch and the management verbs (spec §2). */
static const char CAPABILITIES[] =
    "{\"protocol\":\"aveva-pi\",\"version\":\"" TDOT_VERSION "\","
    "\"modes\":[\"raw\",\"typed\"],"
    "\"datatypes\":[\"bool\",\"int8\",\"uint8\",\"int16\",\"uint16\","
    "\"int32\",\"uint32\",\"int64\",\"uint64\",\"float32\",\"float64\","
    "\"string\"],"
    "\"point_kinds\":[\"point\",\"attribute\",\"stream\"],"
    "\"command_verbs\":[\"write\"],"
    "\"features\":[\"polling\",\"subscribe\",\"bulk_read\",\"backfill\"],"
    "\"subscribe\":true}";

static const char *const CONNECTION_KEYS[] = {
    "auth",          "tls",           "request_timeout",         "connect_timeout",
    "resolve_retry", "max_web_ids_per_request", "max_url_bytes", "max_concurrent_requests",
    "update_option", "buffer_option", "state_dir",               "backfill",
    "channel",       NULL,
};

/* `protocol_address` keys: the device's own, then the [connection] keys it may override. */
static const char *const DEVICE_KEYS[] = {
    "base_url",        "data_server",   "af_database",   "delivery",
    "auth",            "tls",           "request_timeout", "connect_timeout",
    "resolve_retry",   "max_web_ids_per_request", "max_url_bytes", "max_concurrent_requests",
    "state_dir",       "backfill",      "channel",       NULL,
};

static const char *const UPDATE_OPTIONS[] = {"Replace", "Insert", "NoReplace", "ReplaceOnly",
                                             "InsertNoCompression", NULL};
static const char *const BUFFER_OPTIONS[] = {"DoNotBuffer", "BufferIfPossible", "Buffer", NULL};

/* self->state */
typedef struct {
    char service[256];
    double default_interval_s; /* [connector] sampling_interval, else poll_interval */
} pi_state_t;

const char *pi_delivery_name(pi_delivery_t d) {
    switch (d) {
    case PI_DELIVERY_RECORDED:
        return "recorded";
    case PI_DELIVERY_SNAPSHOT:
        return "snapshot";
    default:
        return "channel";
    }
}

static double mono(void) { return tdot_mono(); }

/* ---- configuration (spec §3, config.rs) ----------------------------------------------------- */

static int check_keys(const toml_table_t *tab, const char *const *allowed, const char *what,
                      char *err, size_t errlen) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key)
            return 0;
        bool known = false;
        for (const char *const *a = allowed; *a && !known; a++)
            known = strcmp(*a, key) == 0;
        if (!known) {
            snprintf(err, errlen, "%s: unknown key '%s'", what, key);
            return -1;
        }
    }
}

static bool one_of(const char *const *list, const char *s) {
    for (; *list; list++)
        if (strcmp(*list, s) == 0)
            return true;
    return false;
}

static void join_list(const char *const *list, char *dst, size_t cap) {
    dst[0] = '\0';
    for (size_t i = 0; list[i]; i++) {
        size_t n = strlen(dst);
        snprintf(dst + n, cap - n, "%s%s", i ? ", " : "", list[i]);
    }
}

/* A non-empty string key copied into dst: 1 present, 0 absent, -1 error. */
static int opt_string(const toml_table_t *tab, const char *key, const char *what, char *dst,
                      size_t cap, char *err, size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok || !d.u.s[0]) {
        if (d.ok)
            free(d.u.s);
        snprintf(err, errlen, "%s.%s must be a non-empty string", what, key);
        return -1;
    }
    if (strlen(d.u.s) >= cap) {
        snprintf(err, errlen, "%s.%s is longer than %zu characters", what, key, cap - 1);
        free(d.u.s);
        return -1;
    }
    snprintf(dst, cap, "%s", d.u.s);
    free(d.u.s);
    return 1;
}

static int opt_duration(const toml_table_t *tab, const char *key, const char *what, double *out,
                        char *err, size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok) {
        snprintf(err, errlen, "%s.%s must be a duration string such as \"10s\"", what, key);
        return -1;
    }
    double s = tdot_duration_parse(d.u.s);
    if (s < 0) {
        snprintf(err, errlen, "%s.%s: '%s' is not a duration such as \"10s\"", what, key, d.u.s);
        free(d.u.s);
        return -1;
    }
    free(d.u.s);
    *out = s;
    return 1;
}

static int opt_positive(const toml_table_t *tab, const char *key, const char *what, size_t *out,
                        char *err, size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_int_in(tab, key);
    if (!d.ok || d.u.i <= 0) {
        snprintf(err, errlen, "%s.%s must be a positive integer", what, key);
        return -1;
    }
    *out = (size_t)d.u.i;
    return 1;
}

static int opt_bool(const toml_table_t *tab, const char *key, const char *what, bool *out,
                    char *err, size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_bool_in(tab, key);
    if (!d.ok) {
        snprintf(err, errlen, "%s.%s must be true or false", what, key);
        return -1;
    }
    *out = d.u.b != 0;
    return 1;
}

/* A sub-table `key` of `tab`: NULL with *rc 0 when absent, -1 (err filled) when not a table. */
static toml_table_t *sub_table(const toml_table_t *tab, const char *key, const char *what,
                               int *rc, char *err, size_t errlen) {
    *rc = 0;
    if (!toml_key_exists(tab, key))
        return NULL;
    toml_table_t *t = toml_table_in(tab, key);
    if (!t) {
        snprintf(err, errlen, "%s must be a table", what);
        *rc = -1;
    }
    return t;
}

static int parse_auth(const toml_table_t *m, const char *what, pi_settings_t *s, char *err,
                      size_t errlen) {
    static const char *const inline_keys[][2] = {{"password", "password_file"},
                                                 {"token", "token_file"}};
    for (size_t i = 0; i < 2; i++)
        if (toml_key_exists(m, inline_keys[i][0])) {
            snprintf(err, errlen,
                     "%s.%s is not allowed: put the secret in a file and name it with %s.%s",
                     what, inline_keys[i][0], what, inline_keys[i][1]);
            return -1;
        }
    char method[32] = "none";
    if (opt_string(m, "method", what, method, sizeof method, err, errlen) < 0)
        return -1;
    s->auth = PI_AUTH_NONE;
    s->username[0] = s->password_file[0] = s->token_file[0] = '\0';
    s->cert_file[0] = s->key_file[0] = '\0';
#define REQUIRED(key, dst)                                                                     \
    do {                                                                                       \
        int r_ = opt_string(m, key, what, dst, sizeof dst, err, errlen);                       \
        if (r_ < 0)                                                                            \
            return -1;                                                                         \
        if (r_ == 0) {                                                                         \
            snprintf(err, errlen, "%s.%s is required for method '%s'", what, key, method);     \
            return -1;                                                                         \
        }                                                                                      \
    } while (0)
    if (strcmp(method, "none") == 0) {
        static const char *const keys[] = {"method", NULL};
        if (check_keys(m, keys, what, err, errlen) != 0)
            return -1;
    } else if (strcmp(method, "basic") == 0) {
        static const char *const keys[] = {"method", "username", "password_file", NULL};
        if (check_keys(m, keys, what, err, errlen) != 0)
            return -1;
        REQUIRED("username", s->username);
        REQUIRED("password_file", s->password_file);
        s->auth = PI_AUTH_BASIC;
    } else if (strcmp(method, "bearer") == 0) {
        static const char *const keys[] = {"method", "token_file", NULL};
        if (check_keys(m, keys, what, err, errlen) != 0)
            return -1;
        REQUIRED("token_file", s->token_file);
        s->auth = PI_AUTH_BEARER;
    } else if (strcmp(method, "certificate") == 0) {
        static const char *const keys[] = {"method", "cert_file", "key_file", NULL};
        if (check_keys(m, keys, what, err, errlen) != 0)
            return -1;
        REQUIRED("cert_file", s->cert_file);
        REQUIRED("key_file", s->key_file);
        s->auth = PI_AUTH_CERTIFICATE;
    } else {
        snprintf(err, errlen, "%s.method '%s' is not one of none, basic, bearer, certificate",
                 what, method);
        return -1;
    }
#undef REQUIRED
    return 0;
}

static int parse_tls(const toml_table_t *m, const char *what, pi_settings_t *s, char *err,
                     size_t errlen) {
    static const char *const keys[] = {"ca_file", "insecure_skip_verify", NULL};
    if (check_keys(m, keys, what, err, errlen) != 0)
        return -1;
    s->ca_file[0] = '\0';
    s->insecure_skip_verify = false;
    if (opt_string(m, "ca_file", what, s->ca_file, sizeof s->ca_file, err, errlen) < 0 ||
        opt_bool(m, "insecure_skip_verify", what, &s->insecure_skip_verify, err, errlen) < 0)
        return -1;
    return 0;
}

static int parse_backfill(const toml_table_t *parent, const char *what, pi_settings_t *s,
                          char *err, size_t errlen) {
    toml_datum_t b = toml_bool_in(parent, "backfill");
    pi_settings_t d;
    pi_settings_default(&d);
    s->max_age_s = d.max_age_s;
    s->max_events = d.max_events;
    s->page_size = d.page_size;
    s->live_buffer = d.live_buffer;
    s->cursor_flush_s = d.cursor_flush_s;
    if (b.ok) {
        s->backfill = b.u.b != 0;
        return 0;
    }
    toml_table_t *m = toml_table_in(parent, "backfill");
    if (!m) {
        snprintf(err, errlen, "%s must be a table or false", what);
        return -1;
    }
    static const char *const keys[] = {"max_age", "max_events", "page_size", "live_buffer",
                                       "cursor_flush", NULL};
    if (check_keys(m, keys, what, err, errlen) != 0)
        return -1;
    if (opt_positive(m, "page_size", what, &s->page_size, err, errlen) < 0)
        return -1;
    if (s->page_size > 150000) {
        snprintf(err, errlen, "%s.page_size must be at most 150000 (the server's default cap)",
                 what);
        return -1;
    }
    if (opt_duration(m, "max_age", what, &s->max_age_s, err, errlen) < 0 ||
        opt_positive(m, "max_events", what, &s->max_events, err, errlen) < 0 ||
        opt_positive(m, "live_buffer", what, &s->live_buffer, err, errlen) < 0 ||
        opt_duration(m, "cursor_flush", what, &s->cursor_flush_s, err, errlen) < 0)
        return -1;
    s->backfill = true;
    return 0;
}

static int parse_channel(const toml_table_t *m, const char *what, pi_settings_t *s, char *err,
                         size_t errlen) {
    static const char *const keys[] = {"heartbeat_rate", "polling_interval",
                                       "include_initial_values", NULL};
    if (check_keys(m, keys, what, err, errlen) != 0)
        return -1;
    pi_settings_t d;
    pi_settings_default(&d);
    s->heartbeat_rate = d.heartbeat_rate;
    s->polling_interval_s = d.polling_interval_s;
    s->include_initial_values = d.include_initial_values;
    if (toml_key_exists(m, "heartbeat_rate")) {
        toml_datum_t r = toml_int_in(m, "heartbeat_rate");
        if (!r.ok || r.u.i < 0 || r.u.i > 4294967295LL) {
            snprintf(err, errlen, "%s.heartbeat_rate must be a non-negative integer", what);
            return -1;
        }
        s->heartbeat_rate = (unsigned)r.u.i;
    }
    if (opt_duration(m, "polling_interval", what, &s->polling_interval_s, err, errlen) < 0 ||
        opt_bool(m, "include_initial_values", what, &s->include_initial_values, err, errlen) < 0)
        return -1;
    return 0;
}

/* Apply the keys of `m` (a [connection] table or a device's overrides) to `s`. */
static int apply(pi_settings_t *s, const toml_table_t *m, const char *what, char *err,
                 size_t errlen) {
    char sub[96];
    int rc;
    snprintf(sub, sizeof sub, "%s.auth", what);
    toml_table_t *t = sub_table(m, "auth", sub, &rc, err, errlen);
    if (rc < 0 || (t && parse_auth(t, sub, s, err, errlen) != 0))
        return -1;
    snprintf(sub, sizeof sub, "%s.tls", what);
    t = sub_table(m, "tls", sub, &rc, err, errlen);
    if (rc < 0 || (t && parse_tls(t, sub, s, err, errlen) != 0))
        return -1;
    if (opt_duration(m, "request_timeout", what, &s->request_timeout_s, err, errlen) < 0 ||
        opt_duration(m, "connect_timeout", what, &s->connect_timeout_s, err, errlen) < 0 ||
        opt_duration(m, "resolve_retry", what, &s->resolve_retry_s, err, errlen) < 0 ||
        opt_positive(m, "max_web_ids_per_request", what, &s->max_web_ids_per_request, err,
                     errlen) < 0 ||
        opt_positive(m, "max_url_bytes", what, &s->max_url_bytes, err, errlen) < 0 ||
        opt_positive(m, "max_concurrent_requests", what, &s->max_concurrent_requests, err,
                     errlen) < 0)
        return -1;
    char option[64], list[128];
    rc = opt_string(m, "update_option", what, option, sizeof option, err, errlen);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        if (!one_of(UPDATE_OPTIONS, option)) {
            join_list(UPDATE_OPTIONS, list, sizeof list);
            snprintf(err, errlen, "%s.update_option '%s' is not one of %s", what, option, list);
            return -1;
        }
        snprintf(s->update_option, sizeof s->update_option, "%s", option);
    }
    rc = opt_string(m, "buffer_option", what, option, sizeof option, err, errlen);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        if (!one_of(BUFFER_OPTIONS, option)) {
            join_list(BUFFER_OPTIONS, list, sizeof list);
            snprintf(err, errlen, "%s.buffer_option '%s' is not one of %s", what, option, list);
            return -1;
        }
        snprintf(s->buffer_option, sizeof s->buffer_option, "%s", option);
    }
    if (opt_string(m, "state_dir", what, s->state_dir, sizeof s->state_dir, err, errlen) < 0)
        return -1;
    if (toml_key_exists(m, "backfill")) {
        snprintf(sub, sizeof sub, "%s.backfill", what);
        if (parse_backfill(m, sub, s, err, errlen) != 0)
            return -1;
    }
    snprintf(sub, sizeof sub, "%s.channel", what);
    t = sub_table(m, "channel", sub, &rc, err, errlen);
    if (rc < 0 || (t && parse_channel(t, sub, s, err, errlen) != 0))
        return -1;
    return 0;
}

static char *strip_leading_backslashes(char *s) {
    while (*s == '\\')
        s++;
    return s;
}

/* A point's `address` (spec §3.3): pt->proto and the static addr echo. */
static int configure_point(const pi_device_t *pd, tdot_point_t *pt, char *err, size_t errlen) {
    const toml_table_t *m = pt->address;
    const char *exactly_one = "address must have exactly one of tag, path or web_id";
    if (!m) {
        snprintf(err, errlen, "%s", exactly_one);
        return -1;
    }
    static const char *const keys[] = {"tag", "path", "web_id", NULL};
    if (check_keys(m, keys, "address", err, errlen) != 0)
        return -1;
    int nkeys = 0;
    while (toml_key_in(m, nkeys))
        nkeys++;
    if (nkeys != 1) {
        snprintf(err, errlen, "%s", exactly_one);
        return -1;
    }
    char given[1024], text[2048];
    pi_addr_kind_t kind;
    int rc;
    if ((rc = opt_string(m, "tag", "address", given, sizeof given, err, errlen)) != 0) {
        if (rc < 0)
            return -1;
        if (!pd->data_server[0]) {
            snprintf(err, errlen, "address.tag needs the device's protocol_address.data_server");
            return -1;
        }
        char server[256];
        snprintf(server, sizeof server, "%s", pd->data_server);
        snprintf(text, sizeof text, "\\\\%s\\%s", strip_leading_backslashes(server), given);
        kind = PI_ADDR_POINT;
    } else if ((rc = opt_string(m, "path", "address", given, sizeof given, err, errlen)) != 0) {
        if (rc < 0)
            return -1;
        bool absolute = strncmp(given, "\\\\", 2) == 0;
        if (strchr(given, '|')) {
            if (absolute) {
                snprintf(text, sizeof text, "%s", given);
            } else {
                if (!pd->af_database[0]) {
                    snprintf(err, errlen, "a relative attribute path needs the device's "
                                          "protocol_address.af_database");
                    return -1;
                }
                char db[512];
                snprintf(db, sizeof db, "%s", pd->af_database);
                size_t n = strlen(db);
                while (n && db[n - 1] == '\\')
                    db[--n] = '\0';
                snprintf(text, sizeof text, "%s\\%s", db, given);
            }
            kind = PI_ADDR_ATTRIBUTE;
        } else if (absolute) {
            snprintf(text, sizeof text, "%s", given);
            kind = PI_ADDR_POINT;
        } else {
            snprintf(err, errlen, "address.path '%s' must start with \\\\<server>", given);
            return -1;
        }
    } else if ((rc = opt_string(m, "web_id", "address", given, sizeof given, err, errlen)) !=
               0) {
        if (rc < 0)
            return -1;
        snprintf(text, sizeof text, "%s", given);
        kind = PI_ADDR_WEB_ID;
    } else {
        snprintf(err, errlen, "%s", exactly_one);
        return -1;
    }
    if (pt->mode == TDOT_MODE_TYPED) {
        if (pt->datatype == TDOT_DT_NONE) {
            snprintf(err, errlen, "datatype is required in typed mode");
            return -1;
        }
        if (pt->datatype == TDOT_DT_BYTES) {
            snprintf(err, errlen, "datatype 'bytes' is not supported (use raw mode)");
            return -1;
        }
    }
    size_t n = strlen(text);
    pi_point_t *pp = calloc(1, sizeof *pp + n + 1);
    if (!pp) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    pp->kind = kind;
    memcpy(pp->text, text, n + 1);
    cJSON *addr = cJSON_CreateObject();
    cJSON_AddStringToObject(addr, "path", text);
    char *json = cJSON_PrintUnformatted(addr);
    cJSON_Delete(addr);
    free(pt->proto);
    free(pt->addr_json);
    pt->proto = pp;
    pt->addr_json = json;
    return 0;
}

/* A device: its protocol_address over the connection settings, and its points. */
static int configure_device(const pi_settings_t *conn, tdot_device_t *dev, char *err,
                            size_t errlen) {
    const toml_table_t *m = dev->protocol_address;
    const char *what = "protocol_address";
    if (!m) {
        snprintf(err, errlen, "protocol_address.base_url is required");
        return -1;
    }
    if (check_keys(m, DEVICE_KEYS, what, err, errlen) != 0)
        return -1;
    pi_device_t *pd = calloc(1, sizeof *pd);
    if (!pd) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    char delivery[32] = "channel";
    int rc = opt_string(m, "base_url", what, pd->base_url, sizeof pd->base_url, err, errlen);
    if (rc == 0)
        snprintf(err, errlen, "protocol_address.base_url is required");
    if (rc <= 0)
        goto fail;
    if (strncmp(pd->base_url, "https://", 8) != 0 && strncmp(pd->base_url, "http://", 7) != 0) {
        snprintf(err, errlen, "protocol_address.base_url '%s' must start with https:// or http://",
                 pd->base_url);
        goto fail;
    }
    if (opt_string(m, "delivery", what, delivery, sizeof delivery, err, errlen) < 0)
        goto fail;
    if (strcmp(delivery, "channel") == 0)
        pd->delivery = PI_DELIVERY_CHANNEL;
    else if (strcmp(delivery, "recorded") == 0)
        pd->delivery = PI_DELIVERY_RECORDED;
    else if (strcmp(delivery, "snapshot") == 0)
        pd->delivery = PI_DELIVERY_SNAPSHOT;
    else {
        snprintf(err, errlen,
                 "protocol_address.delivery '%s' is not one of channel, recorded, snapshot",
                 delivery);
        goto fail;
    }
    pd->settings = *conn;
    if (apply(&pd->settings, m, what, err, errlen) != 0)
        goto fail;
    /* A password or token goes in every request's header, and a client certificate needs TLS:
     * plain HTTP is for servers without authentication only. */
    if (pd->settings.auth != PI_AUTH_NONE && strncmp(pd->base_url, "http://", 7) == 0) {
        static const char *const methods[] = {"none", "basic", "bearer", "certificate"};
        snprintf(err, errlen,
                 "protocol_address.base_url '%s': auth method '%s' needs https://, credentials "
                 "are never sent over plain HTTP",
                 pd->base_url, methods[pd->settings.auth]);
        goto fail;
    }
    if (opt_string(m, "data_server", what, pd->data_server, sizeof pd->data_server, err,
                   errlen) < 0 ||
        opt_string(m, "af_database", what, pd->af_database, sizeof pd->af_database, err,
                   errlen) < 0)
        goto fail;
    size_t n = strlen(pd->base_url);
    while (n && pd->base_url[n - 1] == '/')
        pd->base_url[--n] = '\0';
    for (size_t j = 0; j < dev->npoints; j++) {
        char why[256];
        if (configure_point(pd, &dev->points[j], why, sizeof why) != 0) {
            snprintf(err, errlen, "point '%s': %s", dev->points[j].id, why);
            goto fail;
        }
    }
    free(dev->proto);
    dev->proto = pd;
    return 0;
fail:
    free(pd);
    return -1;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err, size_t errlen) {
    pi_state_t *st = self->state;
    pi_settings_t conn;
    pi_settings_default(&conn);
    if (cfg->connection) {
        if (check_keys(cfg->connection, CONNECTION_KEYS, "connection", err, errlen) != 0 ||
            apply(&conn, cfg->connection, "connection", err, errlen) != 0)
            return -1;
    }
    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_device_t *dev = &cfg->devices[i];
        char why[400];
        if (configure_device(&conn, dev, why, sizeof why) != 0) {
            snprintf(err, errlen, "device '%s': %s", dev->name, why);
            return -1;
        }
    }
    snprintf(st->service, sizeof st->service, "%s", cfg->service_name);
    st->default_interval_s =
        cfg->sampling_interval_s >= 0 ? cfg->sampling_interval_s : cfg->poll_interval_s;
    if (st->default_interval_s <= 0)
        st->default_interval_s = 10;
    return 0;
}

/* ---- samples (spec §4, §5.1; sample.rs) ---------------------------------------------------- */

static const pi_point_t *point_of(const tdot_point_t *pt) { return pt->proto; }

/* Take PI's unit for a point that configures none (spec §4 "Units"). */
static void take_unit(tdot_point_t *pt, const char *units) {
    pi_point_t *pp = pt->proto;
    if (pt->unit || !units || !units[0] || !pp)
        return;
    pt->unit = strdup(units);
    pp->unit_from_pi = pt->unit != NULL;
}

void pi_build_sample(tdot_point_t *pt, const pi_stream_t *stream, const cJSON *pi,
                     double source_ms, bool replayed, tdot_sample_t *s, char **addr) {
    tdot_sample_init(s);
    pi_decoded_t d;
    pi_decode(pi, pt->datatype, pt->mode, &d);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "web_id", stream->web_id);
    cJSON_AddStringToObject(a, "path", point_of(pt)->text);
    if (stream->point_type)
        cJSON_AddStringToObject(a, "point_type", stream->point_type);
    if (d.timestamp)
        cJSON_AddStringToObject(a, "timestamp", d.timestamp);
    if (d.flags) {
        cJSON *flags = cJSON_AddArrayToObject(a, "flags");
        if (d.flags & PI_FLAG_QUESTIONABLE)
            cJSON_AddItemToArray(flags, cJSON_CreateString("questionable"));
        if (d.flags & PI_FLAG_SUBSTITUTED)
            cJSON_AddItemToArray(flags, cJSON_CreateString("substituted"));
        if (d.flags & PI_FLAG_ANNOTATED)
            cJSON_AddItemToArray(flags, cJSON_CreateString("annotated"));
    }
    *addr = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    s->addr_json = *addr;
    if (d.raw) {
        size_t n = strlen(d.raw);
        if (n > TDOT_RAW_MAX)
            n = TDOT_RAW_MAX;
        memcpy(s->raw, d.raw, n);
        s->raw_len = n;
    }
    s->raw_group = 1;
    s->quality = d.quality;
    if (d.quality == TDOT_Q_BAD)
        snprintf(s->error, sizeof s->error, "%s", d.error);
    else if (d.has_value)
        s->value = d.value;
    take_unit(pt, stream->units);
    take_unit(pt, d.units);
    s->source_ts_ms = source_ms;
    s->replayed = replayed && source_ms > 0;
    pi_decoded_free(&d);
}

/* ---- WebId resolution (resolve.rs) ---------------------------------------------------------- */

static char *text_of(const cJSON *v, const char *key) {
    const cJSON *t = cJSON_IsObject(v) ? cJSON_GetObjectItemCaseSensitive(v, key) : NULL;
    return cJSON_IsString(t) && t->valuestring[0] ? strdup(t->valuestring) : NULL;
}

static void stream_clear(pi_stream_t *s) {
    free(s->web_id);
    free(s->point_type);
    free(s->units);
    free(s->digital_set);
    for (size_t i = 0; i < s->nstates; i++)
        free(s->states[i].name);
    free(s->states);
    free(s->addr);
    memset(s, 0, sizeof *s);
}

static bool stream_is_digital(const pi_stream_t *s) {
    return s->point_type &&
           (strcmp(s->point_type, "Digital") == 0 || strcmp(s->point_type, "EnumerationValue") == 0);
}

/* Look up the points idx[0..n) (spec §3.3). A point the server does not have -- any 4xx other
 * than 401/403, or an answer without a WebId -- stays unresolved. 0, or -1 with *e. */
static int resolve_points(pi_live_t *live, tdot_device_t *dev, const size_t *idx, size_t n,
                          pi_error_t *e) {
    pi_req_t *reqs = calloc(n ? n : 1, sizeof *reqs);
    if (!reqs) {
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        const pi_point_t *pp = dev->points[idx[i]].proto;
        if (pp->kind == PI_ADDR_WEB_ID)
            continue;
        bool point = pp->kind == PI_ADDR_POINT;
        pi_buf_t q = {0};
        pi_buf_pair(&q, "path", pp->text);
        pi_buf_pair(&q, "selectedFields",
                    point ? "WebId;Name;PointType;EngineeringUnits;DigitalSetName"
                          : "WebId;Name;Type;DefaultUnitsNameAbbreviation");
        reqs[i].url = pi_url(&live->client, point ? "points" : "attributes", q.s ? q.s : "");
        pi_buf_free(&q);
    }
    pi_http_run(&live->client, reqs, n, true);
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        tdot_point_t *pt = &dev->points[idx[i]];
        const pi_point_t *pp = pt->proto;
        pi_stream_t *s = &live->streams[idx[i]];
        stream_clear(s);
        if (pp->kind == PI_ADDR_WEB_ID) {
            s->web_id = strdup(pp->text);
            s->resolved = s->web_id != NULL;
            continue;
        }
        pi_req_t *r = &reqs[i];
        if (r->err.kind != PI_E_NONE) {
            if (r->err.kind == PI_E_STATUS && r->err.code >= 400 && r->err.code < 500)
                continue;
            if (rc == 0) {
                *e = r->err;
                rc = -1;
            }
            continue;
        }
        s->web_id = text_of(r->json, "WebId");
        if (!s->web_id)
            continue;
        s->resolved = true;
        if (pp->kind == PI_ADDR_POINT) {
            s->point_type = text_of(r->json, "PointType");
            s->units = text_of(r->json, "EngineeringUnits");
            s->digital_set = text_of(r->json, "DigitalSetName");
        } else {
            s->point_type = text_of(r->json, "Type");
            s->units = text_of(r->json, "DefaultUnitsNameAbbreviation");
        }
    }
    for (size_t i = 0; i < n; i++)
        pi_req_free(&reqs[i]);
    free(reqs);
    return rc;
}

/* The server of a PI point path \\server\tag, into dst. False when the path has none. */
static bool server_of(const char *path, char *dst, size_t cap) {
    if (strncmp(path, "\\\\", 2) != 0)
        return false;
    const char *p = path + 2;
    size_t n = strcspn(p, "\\");
    if (n == 0 || n >= cap)
        return false;
    memcpy(dst, p, n);
    dst[n] = '\0';
    return true;
}

/* Digital sets, looked up once per connection and shared by the points that use them. */
typedef struct {
    char server[256], set[256]; /* upper case */
    bool found;
    pi_digital_state_t *states;
    size_t nstates;
} pi_set_cache_t;

static void upper(char *dst, size_t cap, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        dst[i] = (char)toupper((unsigned char)s[i]);
    dst[i] = '\0';
}

static int fetch_set(pi_client_t *c, const char *server, const char *set, pi_set_cache_t *out,
                     pi_error_t *e) {
    char path[300];
    snprintf(path, sizeof path, "\\\\%s", server);
    pi_buf_t q = {0};
    pi_buf_pair(&q, "path", path);
    pi_buf_pair(&q, "selectedFields", "WebId");
    cJSON *doc = NULL;
    int rc = pi_http_get(c, "dataservers", q.s ? q.s : "", &doc, e);
    pi_buf_free(&q);
    if (rc != 0)
        return e->kind == PI_E_STATUS && e->code >= 400 && e->code < 500 ? 0 : -1;
    char *server_id = text_of(doc, "WebId");
    cJSON_Delete(doc);
    if (!server_id)
        return 0;
    char sets_path[512];
    snprintf(sets_path, sizeof sets_path, "dataservers/%s/enumerationsets", server_id);
    free(server_id);
    pi_buf_t q2 = {0};
    pi_buf_pair(&q2, "selectedFields", "Items.WebId;Items.Name");
    rc = pi_http_get(c, sets_path, q2.s ? q2.s : "", &doc, e);
    pi_buf_free(&q2);
    if (rc != 0)
        return -1;
    char *set_id = NULL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "Items");
    const cJSON *item;
    cJSON_ArrayForEach(item, pi_array(items)) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "Name");
        if (cJSON_IsString(name) && strcasecmp(name->valuestring, set) == 0) {
            set_id = text_of(item, "WebId");
            break;
        }
    }
    cJSON_Delete(doc);
    if (!set_id)
        return 0;
    char values_path[512];
    snprintf(values_path, sizeof values_path, "enumerationsets/%s/values", set_id);
    free(set_id);
    pi_buf_t q3 = {0};
    pi_buf_pair(&q3, "selectedFields", "Items.Name;Items.Value");
    rc = pi_http_get(c, values_path, q3.s ? q3.s : "", &doc, e);
    pi_buf_free(&q3);
    if (rc != 0)
        return -1;
    items = cJSON_GetObjectItemCaseSensitive(doc, "Items");
    size_t count = cJSON_IsArray(items) ? (size_t)cJSON_GetArraySize(items) : 0;
    out->states = calloc(count ? count : 1, sizeof *out->states);
    const cJSON *list = out->states ? pi_array(items) : NULL;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "Name");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "Value");
        if (!cJSON_IsString(name) || !name->valuestring[0] || !cJSON_IsNumber(value) ||
            value->valuedouble != trunc(value->valuedouble) || fabs(value->valuedouble) > 9e18)
            continue;
        out->states[out->nstates].name = strdup(name->valuestring);
        out->states[out->nstates].code = (long long)value->valuedouble;
        if (out->states[out->nstates].name)
            out->nstates++;
    }
    cJSON_Delete(doc);
    out->found = true;
    return 0;
}

/* Fetch the digital sets of the resolved points that name one. 0, or -1 with *e. */
static int resolve_sets(pi_live_t *live, tdot_device_t *dev, pi_error_t *e) {
    pi_set_cache_t *cache = NULL;
    size_t ncache = 0;
    int rc = 0;
    for (size_t k = 0; k < dev->npoints && rc == 0; k++) {
        pi_stream_t *s = &live->streams[k];
        char server[256];
        if (!s->resolved || !s->digital_set ||
            !server_of(point_of(&dev->points[k])->text, server, sizeof server))
            continue;
        char su[256], tu[256];
        upper(su, sizeof su, server);
        upper(tu, sizeof tu, s->digital_set);
        pi_set_cache_t *hit = NULL;
        for (size_t i = 0; i < ncache && !hit; i++)
            if (strcmp(cache[i].server, su) == 0 && strcmp(cache[i].set, tu) == 0)
                hit = &cache[i];
        if (!hit) {
            pi_set_cache_t *grown = realloc(cache, (ncache + 1) * sizeof *grown);
            if (!grown) {
                e->kind = PI_E_TRANSPORT;
                snprintf(e->text, sizeof e->text, "out of memory");
                rc = -1;
                break;
            }
            cache = grown;
            hit = &cache[ncache++];
            memset(hit, 0, sizeof *hit);
            snprintf(hit->server, sizeof hit->server, "%s", su);
            snprintf(hit->set, sizeof hit->set, "%s", tu);
            if (fetch_set(&live->client, server, s->digital_set, hit, e) != 0) {
                rc = -1;
                break;
            }
        }
        if (!hit->found)
            continue;
        s->states = calloc(hit->nstates ? hit->nstates : 1, sizeof *s->states);
        if (!s->states)
            continue;
        s->has_states = true;
        for (size_t i = 0; i < hit->nstates; i++) {
            s->states[i].name = strdup(hit->states[i].name);
            s->states[i].code = hit->states[i].code;
            if (s->states[i].name)
                s->nstates++;
        }
    }
    for (size_t i = 0; i < ncache; i++) {
        for (size_t j = 0; j < cache[i].nstates; j++)
            free(cache[i].states[j].name);
        free(cache[i].states);
    }
    free(cache);
    return rc;
}

/* ---- connect / disconnect (spec §7) -------------------------------------------------------- */

static void free_live(pi_live_t *live, size_t npoints) {
    if (!live)
        return;
    pi_push_stop(live->push);
    live->push = NULL;
    if (live->streams)
        for (size_t k = 0; k < npoints; k++)
            stream_clear(&live->streams[k]);
    free(live->streams);
    pi_client_free(&live->client);
    pi_creds_free(&live->creds);
    free(live);
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self;
    pi_device_t *pd = dev->proto;
    if (!pd || !pd->live)
        return;
    free_live(pd->live, dev->npoints);
    pd->live = NULL;
}

/* Units from PI (spec §4): what the points that configure none got last time is looked up
 * again. */
static void reset_units(tdot_device_t *dev) {
    for (size_t k = 0; k < dev->npoints; k++) {
        tdot_point_t *pt = &dev->points[k];
        pi_point_t *pp = pt->proto;
        if (pp && pp->unit_from_pi) {
            free(pt->unit);
            pt->unit = NULL;
            pp->unit_from_pi = false;
        }
    }
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev, char *err,
                          size_t errlen) {
    disconnect_device(self, dev);
    pi_device_t *pd = dev->proto;
    if (!pd) {
        snprintf(err, errlen, "device is not configured");
        return -1;
    }
    const pi_settings_t *s = &pd->settings;
    if (strncmp(pd->base_url, "http://", 7) == 0)
        pi_log("warn", "device %s: base_url is plain HTTP: readings and writes are not encrypted",
               dev->name);
    if (s->insecure_skip_verify && strncmp(pd->base_url, "https://", 8) == 0)
        pi_log("warn", "device %s: tls.insecure_skip_verify is set: the server's certificate is "
                       "not verified",
               dev->name);
    pi_live_t *live = calloc(1, sizeof *live);
    if (!live) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    atomic_init(&live->chunk, s->max_web_ids_per_request ? s->max_web_ids_per_request : 1);
    atomic_init(&live->chunk_warned, false);
    if (pi_creds_init(&live->creds, s, err, errlen) != 0) {
        free_live(live, 0);
        return -1;
    }
    if (pi_client_init(&live->client, s, pd->base_url, &live->creds, &live->chunk,
                       &live->chunk_warned, dev->name, err, errlen) != 0) {
        free_live(live, 0);
        return -1;
    }
    live->streams = calloc(dev->npoints ? dev->npoints : 1, sizeof *live->streams);
    size_t *all = calloc(dev->npoints ? dev->npoints : 1, sizeof *all);
    if (!live->streams || !all) {
        free(all);
        free_live(live, 0);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    /* The root and the credentials first. */
    cJSON *root = NULL;
    pi_error_t e;
    if (pi_http_get(&live->client, "", "", &root, &e) != 0) {
        pi_error_format(&e, err, errlen);
        free(all);
        free_live(live, dev->npoints);
        return -1;
    }
    cJSON_Delete(root);
    for (size_t k = 0; k < dev->npoints; k++)
        all[k] = k;
    int rc = resolve_points(live, dev, all, dev->npoints, &e);
    free(all);
    if (rc == 0)
        rc = resolve_sets(live, dev, &e);
    if (rc != 0) {
        pi_error_format(&e, err, errlen);
        free_live(live, dev->npoints);
        return -1;
    }
    reset_units(dev);
    size_t resolved = 0;
    for (size_t k = 0; k < dev->npoints; k++) {
        if (live->streams[k].resolved) {
            resolved++;
            take_unit(&dev->points[k], live->streams[k].units);
        } else {
            pi_log("warn", "device %s: point %s: not found: %s", dev->name, dev->points[k].id,
                   point_of(&dev->points[k])->text);
        }
    }
    live->next_retry = mono() + s->resolve_retry_s;
    pi_log("info", "device %s: connected to %s (%zu point(s) resolved, %zu unresolved)",
           dev->name, pd->base_url, resolved, dev->npoints - resolved);
    pd->live = live;
    return 0;
}

/* Look the unresolved points of a device up again once resolve_retry has passed. Returns
 * whether any was found. */
static bool retry_unresolved(tdot_device_t *dev) {
    pi_device_t *pd = dev->proto;
    pi_live_t *live = pd->live;
    size_t *idx = calloc(dev->npoints ? dev->npoints : 1, sizeof *idx);
    size_t n = 0;
    for (size_t k = 0; idx && k < dev->npoints; k++)
        if (!live->streams[k].resolved)
            idx[n++] = k;
    if (n == 0 || mono() < live->next_retry) {
        free(idx);
        return false;
    }
    live->next_retry = mono() + pd->settings.resolve_retry_s;
    pi_error_t e;
    bool any = false;
    if (resolve_points(live, dev, idx, n, &e) == 0) {
        for (size_t i = 0; i < n; i++) {
            pi_stream_t *s = &live->streams[idx[i]];
            if (!s->resolved)
                continue;
            pi_log("info", "device %s: point %s: resolved %s", dev->name,
                   dev->points[idx[i]].id, point_of(&dev->points[idx[i]])->text);
            take_unit(&dev->points[idx[i]], s->units);
            any = true;
        }
    }
    free(idx);
    return any;
}

/* ---- snapshot reads (spec §5.1) ------------------------------------------------------------- */

typedef struct {
    const char *web_id;
    bool ok;
    cJSON *value; /* borrowed from a response */
    char error[TDOT_ERR_MAX];
} pi_answer_t;

static pi_answer_t *answer_for(pi_answer_t *a, size_t n, const char *web_id) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(a[i].web_id, web_id) == 0)
            return &a[i];
    return NULL;
}

static void bad_sample(tdot_sample_t *s, const char *fmt, const char *arg) {
    tdot_sample_init(s);
    tdot_sample_bad(s, fmt, arg);
}

static size_t read_points(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *const *pts,
                          size_t n, tdot_sample_t *out, int *rc) {
    pi_device_t *pd = dev->proto;
    pi_live_t *live = pd ? pd->live : NULL;
    if (!live) {
        for (size_t k = 0; k < n; k++) {
            bad_sample(&out[k], "%s", "not connected");
            rc[k] = 0;
        }
        if (n)
            rc[0] = -1;
        return n ? 1 : 0;
    }
    if (retry_unresolved(dev) && pd->delivery != PI_DELIVERY_SNAPSHOT && self->subscribe_device)
        live->resubscribe = true;

    /* The unique WebIds of the resolved points asked for. */
    const char **ids = calloc(n ? n : 1, sizeof *ids);
    pi_answer_t *answers = calloc(n ? n : 1, sizeof *answers);
    size_t nids = 0;
    for (size_t k = 0; ids && k < n; k++) {
        const pi_stream_t *s = &live->streams[pts[k] - dev->points];
        if (!s->resolved || answer_for(answers, nids, s->web_id))
            continue;
        answers[nids].web_id = s->web_id;
        ids[nids++] = s->web_id;
    }
    /* All chunks in flight together; a chunk the server finds too long is halved and sent
     * again. */
    size_t *starts = calloc(nids ? nids : 1, sizeof *starts);
    size_t *lens = calloc(nids ? nids : 1, sizeof *lens);
    size_t npending = 0;
    if (starts && lens && nids) {
        size_t *tmp = calloc(nids, sizeof *tmp);
        size_t nchunks = tmp ? pi_chunks(ids, nids, atomic_load(&live->chunk),
                                         pd->settings.max_url_bytes, strlen(pd->base_url) + 120,
                                         tmp)
                             : 0;
        for (size_t c = 0; c < nchunks; c++) {
            starts[c] = tmp[c];
            lens[c] = (c + 1 < nchunks ? tmp[c + 1] : nids) - tmp[c];
        }
        npending = nchunks;
        free(tmp);
    }
    cJSON **docs = NULL;
    size_t ndocs = 0;
    pi_error_t failure;
    memset(&failure, 0, sizeof failure);
    while (npending > 0) {
        pi_req_t *reqs = calloc(npending, sizeof *reqs);
        if (!reqs)
            break;
        for (size_t c = 0; c < npending; c++) {
            pi_buf_t q = {0};
            for (size_t i = starts[c]; i < starts[c] + lens[c]; i++)
                pi_buf_pair(&q, "webId", ids[i]);
            pi_buf_pair(&q, "selectedFields", "Items.WebId;Items.Value;Items.Errors");
            reqs[c].url = pi_url(&live->client, "streamsets/value", q.s ? q.s : "");
            pi_buf_free(&q);
        }
        pi_http_run(&live->client, reqs, npending, true);
        size_t done = npending;
        size_t *next_starts = calloc(2 * done, sizeof *next_starts);
        size_t *next_lens = calloc(2 * done, sizeof *next_lens);
        npending = 0;
        for (size_t c = 0; c < done; c++) {
            pi_req_t *r = &reqs[c];
            if (r->err.kind == PI_E_NONE) {
                const cJSON *items = cJSON_GetObjectItemCaseSensitive(r->json, "Items");
                const cJSON *item;
                cJSON_ArrayForEach(item, pi_array(items)) {
                    const cJSON *web_id = cJSON_GetObjectItemCaseSensitive(item, "WebId");
                    pi_answer_t *a =
                        cJSON_IsString(web_id) ? answer_for(answers, nids, web_id->valuestring)
                                               : NULL;
                    if (!a)
                        continue;
                    cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "Value");
                    a->ok = false;
                    a->value = NULL;
                    if (pi_errors_of(item, a->error, sizeof a->error)) {
                    } else if (value) {
                        a->ok = true;
                        a->value = value;
                    } else {
                        snprintf(a->error, sizeof a->error, "no value");
                    }
                }
                cJSON **grown = realloc(docs, (ndocs + 1) * sizeof *grown);
                if (grown) {
                    docs = grown;
                    docs[ndocs++] = r->json;
                    r->json = NULL;
                }
                continue;
            }
            if (r->err.kind == PI_E_TOO_LONG && next_starts && next_lens &&
                pi_chunk_halve(&live->client, lens[c], r->err.code)) {
                size_t half = (lens[c] + 1) / 2;
                next_starts[npending] = starts[c];
                next_lens[npending++] = half;
                next_starts[npending] = starts[c] + half;
                next_lens[npending++] = lens[c] - half;
                continue;
            }
            char why[TDOT_REASON_MAX];
            pi_error_format(&r->err, why, sizeof why);
            for (size_t i = starts[c]; i < starts[c] + lens[c]; i++) {
                pi_answer_t *a = &answers[i];
                a->ok = false;
                snprintf(a->error, sizeof a->error, "%s", why);
            }
            if (pi_error_is_link_failure(&r->err) && failure.kind == PI_E_NONE)
                failure = r->err;
        }
        for (size_t c = 0; c < done; c++)
            pi_req_free(&reqs[c]);
        free(reqs);
        free(starts);
        free(lens);
        starts = next_starts;
        lens = next_lens;
    }
    free(starts);
    free(lens);

    for (size_t k = 0; k < n; k++) {
        tdot_point_t *pt = pts[k];
        pi_stream_t *s = &live->streams[pt - dev->points];
        rc[k] = 0;
        if (!s->resolved) {
            bad_sample(&out[k], "not found: %s", point_of(pt)->text);
            continue;
        }
        pi_answer_t *a = answer_for(answers, nids, s->web_id);
        if (a && a->ok) {
            char *addr = NULL;
            pi_build_sample(pt, s, a->value, 0, false, &out[k], &addr);
            free(s->addr);
            s->addr = addr; /* the sample points at it until the next read */
        } else {
            bad_sample(&out[k], "%s", a && a->error[0] ? a->error : "no value");
        }
    }
    /* A link failure ends the pass after every sample was handed over (Rust's LinkLost). */
    if (failure.kind != PI_E_NONE && n)
        rc[n - 1] = -1;
    for (size_t i = 0; i < ndocs; i++)
        cJSON_Delete(docs[i]);
    free(docs);
    free(answers);
    free(ids);
    return n;
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      tdot_sample_t *out) {
    tdot_point_t *pts[1] = {pt};
    int rc = 0;
    tdot_sample_init(out);
    read_points(self, dev, pts, 1, out, &rc);
    return rc;
}

/* ---- writes (spec §6) ----------------------------------------------------------------------- */

/* The digital state a write names: a state name (exact, then any case) or a code. */
static const pi_digital_state_t *digital_state(const pi_stream_t *s, const tdot_value_t *v) {
    if (v->kind == TDOT_VAL_STR) {
        for (size_t i = 0; i < s->nstates; i++)
            if (strcmp(s->states[i].name, v->str) == 0)
                return &s->states[i];
        for (size_t i = 0; i < s->nstates; i++)
            if (strcasecmp(s->states[i].name, v->str) == 0)
                return &s->states[i];
    } else if (v->kind == TDOT_VAL_NUM && v->num == trunc(v->num) && fabs(v->num) < 9e18) {
        for (size_t i = 0; i < s->nstates; i++)
            if (s->states[i].code == (long long)v->num)
                return &s->states[i];
    }
    return NULL;
}

static int write_point(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                       const tdot_value_t *value, char *err, size_t errlen) {
    (void)self;
    pi_device_t *pd = dev->proto;
    pi_live_t *live = pd ? pd->live : NULL;
    if (!live) {
        snprintf(err, errlen, "not connected");
        return -1;
    }
    const pi_stream_t *s = &live->streams[pt - dev->points];
    if (!s->resolved) {
        snprintf(err, errlen, "not found: %s", point_of(pt)->text);
        return -1;
    }
    cJSON *v = NULL;
    if (s->has_states) {
        const pi_digital_state_t *state = digital_state(s, value);
        if (!state) {
            cJSON *requested = pi_value_json(value);
            char *shown = pi_canonical(requested);
            cJSON_Delete(requested);
            snprintf(err, errlen, "%s is not a state of the digital set %s", shown ? shown : "?",
                     s->digital_set ? s->digital_set : "of the point");
            free(shown);
            return -1;
        }
        v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "Name", state->name);
        cJSON_AddNumberToObject(v, "Value", (double)state->code);
        cJSON_AddFalseToObject(v, "IsSystem");
    } else if (stream_is_digital(s)) {
        /* An AF enumeration attribute: PI checks the state itself. */
        v = pi_value_json(value);
    } else {
        v = pi_encode_write(value, pt->mode == TDOT_MODE_TYPED ? pt->datatype : TDOT_DT_NONE,
                            err, errlen);
        if (!v)
            return -1;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "Value", v);
    char *text = pi_canonical(body);
    cJSON_Delete(body);
    pi_buf_t q = {0};
    pi_buf_pair(&q, "updateOption", pd->settings.update_option);
    pi_buf_pair(&q, "bufferOption", pd->settings.buffer_option);
    char path[512];
    snprintf(path, sizeof path, "streams/%s/value", s->web_id);
    long status = 0;
    pi_error_t e;
    int rc = text ? pi_http_post(&live->client, path, q.s ? q.s : "", text, &status, &e) : -1;
    if (!text)
        snprintf(err, errlen, "out of memory");
    else if (rc != 0)
        pi_error_format(&e, err, errlen);
    pi_buf_free(&q);
    free(text);
    return rc;
}

/* ---- push delivery (spec §5.2-§5.4) -------------------------------------------------------- */

static int subscribe_device(tdot_connector_t *self, tdot_device_t *dev, char *err,
                            size_t errlen) {
    pi_state_t *st = self->state;
    pi_device_t *pd = dev->proto;
    pi_live_t *live = pd ? pd->live : NULL;
    if (!live) {
        snprintf(err, errlen, "not connected");
        return -1;
    }
    pi_push_stop(live->push);
    live->push = NULL;
    live->resubscribe = false;
    if (pd->delivery == PI_DELIVERY_SNAPSHOT)
        return 0; /* pushes_point is false: the runtime polls */
    size_t *targets = calloc(dev->npoints ? dev->npoints : 1, sizeof *targets);
    if (!targets) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t n = 0;
    double interval = 0;
    for (size_t k = 0; k < dev->npoints; k++) {
        const tdot_point_t *pt = &dev->points[k];
        /* A point the server does not have stays on the polling schedule, where every read
         * reports it `not found`; once a retry resolves it, the device subscribes again. */
        if (!(pt->access & TDOT_ACCESS_READ) || !pt->subscribe || !live->streams[k].resolved)
            continue;
        targets[n++] = k;
        if (pt->sampling_interval_s > 0 && (interval == 0 || pt->sampling_interval_s < interval))
            interval = pt->sampling_interval_s;
    }
    if (n == 0) {
        free(targets);
        return 0;
    }
    if (interval <= 0)
        interval = st->default_interval_s;
    char path[1600];
    snprintf(path, sizeof path, "%s/%s/%s.json", pd->settings.state_dir, st->service, dev->name);
    pi_push_args_t a = {.dev = dev,
                        .pd = pd,
                        .live = live,
                        .points = targets,
                        .n = n,
                        .state_file = path,
                        .interval_s = interval,
                        .recorded = pd->delivery == PI_DELIVERY_RECORDED || live->fallback};
    pi_error_t e;
    bool refused = false;
    int rc = pi_push_start(&a, &live->push, &e, &refused);
    if (rc != 0 && refused) {
        pi_log("warn",
               "device %s: the server refused the Channels WebSocket (HTTP %ld%s%s); using "
               "recorded polling until the next reconnect",
               dev->name, e.code, e.text[0] ? ": " : "", e.text);
        live->fallback = true;
        a.recorded = true;
        rc = pi_push_start(&a, &live->push, &e, &refused);
    }
    if (rc != 0) {
        if (refused)
            snprintf(err, errlen, "HTTP %ld", e.code);
        else
            pi_error_format(&e, err, errlen);
        live->push = NULL;
        free(targets);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        dev->points[targets[i]].subscribed = true;
    free(targets);
    return 0;
}

static int drain_subscriptions(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_sample_sink_t sink, void *sink_ctx) {
    (void)self;
    pi_device_t *pd = dev->proto;
    pi_live_t *live = pd ? pd->live : NULL;
    if (!live || !live->push)
        return 0;
    if (pi_push_drain(live->push, dev, live, sink, sink_ctx) != 0)
        return -1;
    if (live->resubscribe) {
        pi_log("info", "device %s: points were resolved after a retry; subscribing again",
               dev->name);
        return -1;
    }
    return 0;
}

/* ---- the rest of the vtable ----------------------------------------------------------------- */

static char *device_info(tdot_connector_t *self, const tdot_device_t *dev) {
    (void)self;
    const pi_device_t *pd = dev->proto;
    if (!pd)
        return NULL;
    /* never the credentials (spec §10) */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "base_url", pd->base_url);
    cJSON_AddStringToObject(obj, "delivery", pi_delivery_name(pd->delivery));
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

static void destroy(tdot_connector_t *self) {
    free(self->state);
    free(self);
}

tdot_connector_t *tdot_connector_aveva_pi_new(void) {
    tdot_connector_t *c = calloc(1, sizeof *c);
    pi_state_t *st = calloc(1, sizeof *st);
    if (!c || !st) {
        free(c);
        free(st);
        return NULL;
    }
    c->protocol = PI_PROTOCOL;
    c->capabilities_json = CAPABILITIES;
    c->local_only_settings = LOCAL_ONLY_SETTINGS;
    c->state = st;
    c->configure = configure;
    c->connect_device = connect_device;
    c->read_point = read_point;
    c->read_points = read_points;
    c->write_point = write_point;
    c->subscribe_device = subscribe_device;
    c->drain_subscriptions = drain_subscriptions;
    c->disconnect_device = disconnect_device;
    c->device_info = device_info;
    c->destroy = destroy;
    return c;
}
