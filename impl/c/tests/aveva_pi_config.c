/* AVEVA PI: the configuration rules of spec §3, the scenarios of
 * impl/rust/crates/connector-aveva-pi/tests/config.rs: every load error names the device or
 * point and the key, inline secrets are refused, device overrides replace the connection
 * value, and the capability descriptor and local-only settings match the Rust module's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "pi_connector.h"
#include "tedge_dot/config.h"
#include "tedge_dot/connector.h"

static int failures;

#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

static char g_dir[64];

typedef struct {
    tdot_config_t *cfg;
    tdot_connector_t *conn;
    char err[512];
} loaded_t;

static void unload(loaded_t *l) {
    if (l->conn)
        l->conn->destroy(l->conn);
    if (l->cfg)
        tdot_config_free(l->cfg);
    l->conn = NULL;
    l->cfg = NULL;
}

/* Load a configuration; 0, or -1 with l->err (the loader's or the module's). */
static int load(const char *text, loaded_t *l) {
    memset(l, 0, sizeof *l);
    char path[128];
    snprintf(path, sizeof path, "%s/pi.toml", g_dir);
    FILE *fp = fopen(path, "w");
    fputs(text, fp);
    fclose(fp);
    l->cfg = tdot_config_load(path, l->err, sizeof l->err);
    if (!l->cfg)
        return -1;
    l->conn = tdot_connector_factory("aveva-pi");
    if (!l->conn) {
        snprintf(l->err, sizeof l->err, "no aveva-pi module");
        return -1;
    }
    return tdot_connector_configure(l->conn, l->cfg, l->err, sizeof l->err);
}

static char *device(const char *address, const char *point_address, const char *extra) {
    static char buf[4096];
    snprintf(buf, sizeof buf,
             "[connector]\nprotocol = \"aveva-pi\"\n\n[connection]\n%s\n\n[[device]]\n"
             "name = \"pi\"\nprotocol_address = %s\n\n  [[device.point]]\n  id = \"p\"\n"
             "  datatype = \"float64\"\n  address = %s\n",
             extra, address, point_address);
    return buf;
}

static const char ADDR[] = "{ base_url = \"https://pi.example.com/piwebapi/\", data_server = "
                           "\"PISRV01\", af_database = '\\\\AF\\Plant' }";

static const pi_device_t *pd(const loaded_t *l) { return l->cfg->devices[0].proto; }
static const pi_point_t *pp(const loaded_t *l) { return l->cfg->devices[0].points[0].proto; }

/* Expect a load error containing every one of the NULL-terminated fragments. */
static void expect_error(const char *what, const char *text, ...) {
    loaded_t l;
    int rc = load(text, &l);
    CHECK(rc != 0, "%s: loaded", what);
    va_list ap;
    va_start(ap, text);
    for (const char *frag = va_arg(ap, const char *); frag; frag = va_arg(ap, const char *))
        CHECK(rc != 0 && strstr(l.err, frag), "%s: '%s' does not contain '%s'", what, l.err,
              frag);
    va_end(ap);
    unload(&l);
}

static void addresses_resolve_to_full_paths(void) {
    loaded_t l;
    CHECK(load(device(ADDR, "{ tag = \"SINUSOID\" }", ""), &l) == 0, "tag: %s", l.err);
    if (l.cfg) {
        CHECK(strcmp(pd(&l)->base_url, "https://pi.example.com/piwebapi") == 0, "base_url %s",
              pd(&l)->base_url);
        CHECK(pp(&l)->kind == PI_ADDR_POINT && strcmp(pp(&l)->text, "\\\\PISRV01\\SINUSOID") == 0,
              "tag path %s", pp(&l)->text);
        CHECK(pd(&l)->delivery == PI_DELIVERY_CHANNEL, "default delivery");
        CHECK(strcmp(l.cfg->devices[0].points[0].addr_json,
                     "{\"path\":\"\\\\\\\\PISRV01\\\\SINUSOID\"}") == 0,
              "addr %s", l.cfg->devices[0].points[0].addr_json);
    }
    unload(&l);
    CHECK(load(device(ADDR, "{ path = 'Line1\\Pump01|Pressure' }", ""), &l) == 0, "rel: %s",
          l.err);
    if (l.cfg)
        CHECK(pp(&l)->kind == PI_ADDR_ATTRIBUTE &&
                  strcmp(pp(&l)->text, "\\\\AF\\Plant\\Line1\\Pump01|Pressure") == 0,
              "relative attribute %s", pp(&l)->text);
    unload(&l);
    CHECK(load(device(ADDR, "{ path = '\\\\AF\\Plant\\Pump01|Pressure' }", ""), &l) == 0,
          "abs: %s", l.err);
    if (l.cfg)
        CHECK(pp(&l)->kind == PI_ADDR_ATTRIBUTE, "absolute attribute");
    unload(&l);
    CHECK(load(device(ADDR, "{ web_id = \"F1DPabc\" }", ""), &l) == 0, "web_id: %s", l.err);
    if (l.cfg)
        CHECK(pp(&l)->kind == PI_ADDR_WEB_ID && strcmp(pp(&l)->text, "F1DPabc") == 0, "web_id");
    unload(&l);
}

static void address_errors(void) {
    expect_error("tag without data_server",
                 device("{ base_url = \"https://pi/piwebapi\" }", "{ tag = \"SINUSOID\" }", ""),
                 "device 'pi'", "point 'p'", "data_server", NULL);
    expect_error("two address keys", device(ADDR, "{ tag = \"A\", web_id = \"F1DP\" }", ""),
                 "point 'p'", "exactly one", NULL);
    expect_error("relative attribute",
                 device("{ base_url = \"https://pi/piwebapi\" }", "{ path = 'Pump|P' }", ""),
                 "af_database", NULL);
    expect_error("relative point", device(ADDR, "{ path = 'SINUSOID' }", ""),
                 "address.path 'SINUSOID' must start with \\\\<server>", NULL);
    expect_error("unknown address key", device(ADDR, "{ node = \"x\" }", ""),
                 "address: unknown key 'node'", NULL);
}

static void inline_secrets_are_refused(void) {
    expect_error("password",
                 device(ADDR, "{ tag = \"A\" }",
                        "auth = { method = \"basic\", username = \"u\", password = \"secret\" }"),
                 "connection.auth.password is not allowed", "connection.auth.password_file",
                 NULL);
    expect_error("token", device(ADDR, "{ tag = \"A\" }", "auth = { method = \"bearer\", token = \"t\" }"),
                 "auth.token", "token_file", NULL);
}

/* Credentials are never sent over plain HTTP; a server without authentication may use it. */
static void credentials_need_https(void) {
    static const char HTTP[] = "{ base_url = \"http://pi/piwebapi\", data_server = \"S\" }";
    expect_error("basic over http",
                 device(HTTP, "{ tag = \"A\" }",
                        "auth = { method = \"basic\", username = \"u\", password_file = \"/p\" }"),
                 "auth method 'basic' needs https://", NULL);
    expect_error("bearer over http",
                 device(HTTP, "{ tag = \"A\" }", "auth = { method = \"bearer\", token_file = \"/t\" }"),
                 "auth method 'bearer' needs https://", NULL);
    expect_error("certificate over http",
                 device(HTTP, "{ tag = \"A\" }",
                        "auth = { method = \"certificate\", cert_file = \"/c\", key_file = \"/k\" }"),
                 "auth method 'certificate' needs https://", NULL);
    loaded_t l;
    CHECK(load(device(HTTP, "{ tag = \"A\" }", ""), &l) == 0, "no auth over http: %s", l.err);
    unload(&l);
}

static void auth_methods(void) {
    loaded_t l;
    CHECK(load(device(ADDR, "{ tag = \"A\" }",
                      "auth = { method = \"bearer\", token_file = \"/run/t\" }"),
               &l) == 0,
          "bearer: %s", l.err);
    if (l.cfg)
        CHECK(pd(&l)->settings.auth == PI_AUTH_BEARER &&
                  strcmp(pd(&l)->settings.token_file, "/run/t") == 0,
              "bearer token file");
    unload(&l);
    expect_error("certificate without key",
                 device(ADDR, "{ tag = \"A\" }", "auth = { method = \"certificate\", cert_file = \"/c\" }"),
                 "connection.auth.key_file is required for method 'certificate'", NULL);
    expect_error("kerberos", device(ADDR, "{ tag = \"A\" }", "auth = { method = \"kerberos\" }"),
                 "kerberos", "is not one of none, basic, bearer, certificate", NULL);
    expect_error("basic without username",
                 device(ADDR, "{ tag = \"A\" }", "auth = { method = \"basic\", password_file = \"/p\" }"),
                 "connection.auth.username is required for method 'basic'", NULL);
}

static void device_overrides_the_connection(void) {
    loaded_t l;
    const char *address = "{ base_url = \"http://pi/piwebapi\", data_server = \"S\", delivery = "
                          "\"recorded\", request_timeout = \"2s\", backfill = false }";
    CHECK(load(device(address, "{ tag = \"A\" }", "request_timeout = \"20s\""), &l) == 0, "%s",
          l.err);
    if (l.cfg) {
        CHECK(pd(&l)->settings.request_timeout_s == 2, "request_timeout %g",
              pd(&l)->settings.request_timeout_s);
        CHECK(!pd(&l)->settings.backfill, "backfill = false");
        CHECK(pd(&l)->delivery == PI_DELIVERY_RECORDED, "delivery");
    }
    unload(&l);
    /* defaults, and a [connection] table every device inherits */
    CHECK(load(device(ADDR, "{ tag = \"A\" }",
                      "backfill = { page_size = 500, live_buffer = 20 }\n"
                      "channel = { heartbeat_rate = 0, polling_interval = \"200ms\" }\n"
                      "update_option = \"Insert\""),
               &l) == 0,
          "%s", l.err);
    if (l.cfg) {
        const pi_settings_t *s = &pd(&l)->settings;
        CHECK(s->page_size == 500 && s->live_buffer == 20 && s->max_events == 10000 &&
                  s->max_age_s == 3600 && s->cursor_flush_s == 10 && s->backfill,
              "backfill");
        CHECK(s->heartbeat_rate == 0 && s->polling_interval_s == 0.2 && s->include_initial_values,
              "channel");
        CHECK(strcmp(s->update_option, "Insert") == 0 &&
                  strcmp(s->buffer_option, "BufferIfPossible") == 0,
              "options");
        CHECK(s->request_timeout_s == 10 && s->connect_timeout_s == 5 && s->resolve_retry_s == 300 &&
                  s->max_web_ids_per_request == 50 && s->max_url_bytes == 8192 &&
                  s->max_concurrent_requests == 4 &&
                  strcmp(s->state_dir, "/var/lib/tedge-dot/aveva-pi") == 0,
              "defaults");
    }
    unload(&l);
}

static void unknown_keys_and_bad_values(void) {
    expect_error("unknown connection key", device(ADDR, "{ tag = \"A\" }", "colour = \"red\""),
                 "connection: unknown key 'colour'", NULL);
    expect_error("delivery",
                 device("{ base_url = \"https://pi\", delivery = \"carrier-pigeon\" }",
                        "{ web_id = \"W\" }", ""),
                 "protocol_address.delivery 'carrier-pigeon' is not one of channel, recorded, "
                 "snapshot",
                 NULL);
    expect_error("base_url scheme", device("{ base_url = \"ftp://pi\" }", "{ web_id = \"W\" }", ""),
                 "base_url", NULL);
    expect_error("base_url missing", device("{ data_server = \"S\" }", "{ web_id = \"W\" }", ""),
                 "protocol_address.base_url is required", NULL);
    expect_error("page_size", device(ADDR, "{ tag = \"A\" }", "backfill = { page_size = 200000 }"),
                 "connection.backfill.page_size must be at most 150000", NULL);
    expect_error("duration", device(ADDR, "{ tag = \"A\" }", "request_timeout = \"soon\""),
                 "connection.request_timeout: 'soon' is not a duration", NULL);
    expect_error("positive", device(ADDR, "{ tag = \"A\" }", "max_url_bytes = 0"),
                 "connection.max_url_bytes must be a positive integer", NULL);
    expect_error("update_option", device(ADDR, "{ tag = \"A\" }", "update_option = \"Merge\""),
                 "connection.update_option 'Merge' is not one of Replace, Insert", NULL);
    expect_error("update_option per device",
                 device("{ base_url = \"https://pi\", update_option = \"Insert\" }",
                        "{ web_id = \"W\" }", ""),
                 "protocol_address: unknown key 'update_option'", NULL);
    expect_error("backfill type", device(ADDR, "{ tag = \"A\" }", "backfill = 3"),
                 "connection.backfill must be a table or false", NULL);
    expect_error("tls key", device(ADDR, "{ tag = \"A\" }", "tls = { verify = false }"),
                 "connection.tls: unknown key 'verify'", NULL);
}

static void descriptor_and_local_only(void) {
    tdot_connector_t *c = tdot_connector_factory("aveva-pi");
    CHECK(c != NULL, "factory");
    if (!c)
        return;
    static const char *const keys[] = {"password_file", "token_file", "cert_file", "key_file",
                                       "ca_file", "insecure_skip_verify", "state_dir"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        bool found = false;
        for (const char *const *k = c->local_only_settings; k && *k; k++)
            found = found || strcmp(*k, keys[i]) == 0;
        CHECK(found, "local-only %s", keys[i]);
    }
    cJSON *caps = cJSON_Parse(c->capabilities_json);
    CHECK(caps != NULL, "descriptor is JSON");
    CHECK(strcmp(cJSON_GetObjectItem(caps, "protocol")->valuestring, "aveva-pi") == 0, "protocol");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(caps, "subscribe")), "subscribe");
    const cJSON *verbs = cJSON_GetObjectItem(caps, "command_verbs");
    CHECK(cJSON_GetArraySize(verbs) == 1 &&
              strcmp(cJSON_GetArrayItem(verbs, 0)->valuestring, "write") == 0,
          "verbs");
    cJSON_Delete(caps);
    CHECK(c->subscribe_device && c->drain_subscriptions && c->read_points && c->device_info,
          "hooks");
    c->destroy(c);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/tdot-aveva-pi-XXXXXX");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    addresses_resolve_to_full_paths();
    address_errors();
    inline_secrets_are_refused();
    auth_methods();
    credentials_need_https();
    device_overrides_the_connection();
    unknown_keys_and_bad_values();
    descriptor_and_local_only();
    char path[128];
    snprintf(path, sizeof path, "%s/pi.toml", g_dir);
    unlink(path);
    rmdir(g_dir);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("aveva-pi-config: ok\n");
    return failures ? 1 : 0;
}
