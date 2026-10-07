/* tedge-dot — OPC UA connector on open62541 (MPL-2.0).
 * Mirrors impl/rust/crates/connector-opcua: client sessions per device, node-id
 * addressed points ("ns=2;s=Temperature"), typed reads/writes, quality "bad"
 * on Bad status codes, and monitored-item push delivery.
 */
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/certificategroup_default.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/platform_util.h>

#include "cjson/cJSON.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/decode.h"
#include "ua_pki.h"
#include "ua_sampling.h"

/* Per-point parsed address (pt->proto, flat). */
typedef struct {
    char node_id[160]; /* textual "ns=2;s=Temperature" */
} ua_point_t;

/* Samples that arrived by subscription since the last drain.
 *
 * open62541 delivers data changes through a callback fired from inside
 * UA_Client_run_iterate(), which drain_subscriptions() calls on the runtime
 * thread -- so producer and consumer are the same thread and the queue needs no
 * locking. It is a ring rather than a single slot per point so that a burst of
 * changes between two ticks is delivered as the separate value changes it was,
 * not collapsed into the latest one. */
#define UA_PUSH_QUEUE_LEN 256

typedef struct {
    tdot_point_t *pt;
    tdot_sample_t sample;
} ua_pending_t;

/* Security policies (spec §3.1). The key-length window is the one
 * async-opcua applies, so both builds accept the same server certificates. */
typedef struct {
    const char *name; /* configuration name == URI fragment */
    bool deprecated;
    size_t min_bits, max_bits;
} ua_policy_t;

#define UA_POLICY_URI_PREFIX "http://opcfoundation.org/UA/SecurityPolicy#"

static const ua_policy_t POLICIES[] = {
    {"None", false, 0, SIZE_MAX},
    {"Basic128Rsa15", true, 1024, 2048},
    {"Basic256", true, 1024, 2048},
    {"Basic256Sha256", false, 2048, 4096},
    {"Aes128_Sha256_RsaOaep", false, 2048, 4096},
    {"Aes256_Sha256_RsaPss", false, 2048, 4096},
};
#define UA_NPOLICIES (sizeof POLICIES / sizeof POLICIES[0])

typedef enum { UA_ID_ANONYMOUS, UA_ID_USERNAME, UA_ID_X509 } ua_identity_t;

static const char *identity_kind(ua_identity_t id) {
    switch (id) {
    case UA_ID_USERNAME: return "username";
    case UA_ID_X509: return "certificate";
    default: return "anonymous";
    }
}

static const char *mode_name(UA_MessageSecurityMode m) {
    switch (m) {
    case UA_MESSAGESECURITYMODE_NONE: return "none";
    case UA_MESSAGESECURITYMODE_SIGN: return "sign";
    case UA_MESSAGESECURITYMODE_SIGNANDENCRYPT: return "sign_and_encrypt";
    default: return "invalid";
    }
}

/* Credentials, owned by the connector state (never by dev->proto, which the
 * config loader frees without wiping): released and wiped on the next
 * configure() and in destroy(). */
typedef struct {
    char user[256];
    char *password;          /* NUL-terminated */
    UA_ByteString x509_cert; /* DER */
    UA_ByteString x509_key;  /* PEM + NUL */
} ua_secret_t;

/* Per-device state (dev->proto, flat; client freed on disconnect). */
typedef struct {
    char endpoint[256];
    UA_Client *client; /* NULL when disconnected */

    /* Security (spec §3). */
    const ua_policy_t *policy;
    UA_MessageSecurityMode mode;
    ua_identity_t identity;
    size_t secret; /* index into ua_state_t.secrets when identity != anonymous */
    bool trust_any;
    bool allow_plaintext;
    /* What the link status `info` reports about the last attempt. */
    char server_thumbprint[41];
    const char *server_certificate; /* NULL, "trusted" or "not_verified" */

    /* Push delivery state, all reset on (re)connect. */
    UA_UInt32 sub_id;
    bool subscribed;
    /* Set from open62541's callbacks when the subscription stops existing --
     * deleted by the server, killed with the session, or gone quiet past its
     * keep-alive. open62541 does NOT surface any of these through
     * UA_Client_run_iterate's return code (it keeps returning GOOD), so
     * without this flag a dead subscription would look exactly like a device
     * that simply has nothing new to report: the points stay off the polling
     * schedule and the device goes silent for good behind a healthy-looking
     * `connected` link. */
    bool sub_lost;
    ua_pending_t queue[UA_PUSH_QUEUE_LEN];
    size_t head; /* next slot to write */
    size_t tail; /* next slot to read */
    unsigned long dropped; /* overruns since the last warning */
} ua_device_t;

typedef struct {
    char application_name[128];
    char application_uri[256];
    int connect_timeout_s;
    int request_timeout_s;

    /* PKI (spec §4). */
    char pki_root[UA_PKI_PATH_MAX];
    char cert_path[UA_PKI_PATH_MAX];
    char key_path[UA_PKI_PATH_MAX];
    bool explicit_cert;
    bool create_certificate;
    /* The application instance certificate, when a device is secured:
     * loaded (or generated) at configure, its failure reported per device. */
    bool own_loaded;
    char own_err[400];
    UA_ByteString own_cert; /* DER */
    UA_ByteString own_key;  /* PEM + NUL */
    time_t expiry_warned;

    ua_secret_t *secrets;
    size_t nsecrets;
} ua_state_t;

/* Settings a management command may not add or change (spec §3): they name
 * files on the gateway, whose contents would be sent to a server, or relax
 * server authentication. Mirrors config::LOCAL_ONLY_SETTINGS (Rust). */
static const char *const LOCAL_ONLY_SETTINGS[] = {
    "pki_dir", "certificate", "private_key", "create_certificate", "password_file",
    "user_certificate", "user_private_key", "trust_any_server_certificate",
    "allow_plaintext_password", "allow_deprecated_security", NULL};

static const char CAPABILITIES[] =
    "{\"protocol\":\"opcua\",\"version\":\"" TDOT_VERSION "\","
    "\"modes\":[\"raw\",\"typed\"],"
    "\"datatypes\":[\"bool\",\"int8\",\"uint8\",\"int16\",\"uint16\","
    "\"int32\",\"uint32\",\"int64\",\"uint64\",\"float32\",\"float64\","
    "\"string\",\"bytes\"],"
    "\"point_kinds\":[\"variable\"],"
    "\"command_verbs\":[\"write\",\"write-batch\"],"
    "\"features\":[\"polling\",\"subscribe\"],\"subscribe\":true}";

/* ---- configuration (spec §3) ------------------------------------------- */

static void wipe_secrets(ua_state_t *st) {
    for (size_t i = 0; i < st->nsecrets; i++) {
        ua_secret_t *sec = &st->secrets[i];
        if (sec->password) {
            mbedtls_platform_zeroize(sec->password, strlen(sec->password));
            free(sec->password);
        }
        if (sec->x509_key.data)
            mbedtls_platform_zeroize(sec->x509_key.data, sec->x509_key.length);
        UA_ByteString_clear(&sec->x509_key);
        UA_ByteString_clear(&sec->x509_cert);
    }
    free(st->secrets);
    st->secrets = NULL;
    st->nsecrets = 0;
    if (st->own_key.data)
        mbedtls_platform_zeroize(st->own_key.data, st->own_key.length);
    UA_ByteString_clear(&st->own_key);
    UA_ByteString_clear(&st->own_cert);
    st->own_loaded = false;
    st->own_err[0] = '\0';
}

static ua_secret_t *new_secret(ua_state_t *st, size_t *index) {
    ua_secret_t *grown = realloc(st->secrets, (st->nsecrets + 1) * sizeof *grown);
    if (!grown)
        return NULL;
    st->secrets = grown;
    *index = st->nsecrets++;
    memset(&st->secrets[*index], 0, sizeof st->secrets[*index]);
    return &st->secrets[*index];
}

/* `dir` made absolute the way Rust's std::path::absolute does it: prefixed with
 * the working directory, symlinks left alone (so both builds print the same
 * paths). */
static void absolute_dir(const char *dir, char *out, size_t outlen) {
    char cwd[PATH_MAX];
    if (dir[0] == '/' || !getcwd(cwd, sizeof cwd))
        snprintf(out, outlen, "%s", dir);
    else if (!strcmp(dir, "."))
        snprintf(out, outlen, "%s", cwd);
    else
        snprintf(out, outlen, "%s/%s", cwd, dir[0] == '.' && dir[1] == '/' ? dir + 2 : dir);
}

/* The absolute directory of the configuration file ("" without one). */
static void config_dir(const tdot_config_t *cfg, char *out, size_t outlen) {
    out[0] = '\0';
    if (!cfg->path)
        return;
    char dir[UA_PKI_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", cfg->path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof dir, ".");
    if (!*dir)
        snprintf(dir, sizeof dir, "/");
    absolute_dir(dir, out, outlen);
}

/* `path` unchanged when absolute, joined to `base` when relative. */
static void resolve_path(const char *base, const char *path, char *out,
                         size_t outlen) {
    if (path[0] == '/' || !*base)
        snprintf(out, outlen, "%s", path);
    else
        snprintf(out, outlen, "%s/%s", base, path);
}

/* A string field: 1 when set, 0 when absent, -1 (err filled) on a wrong type. */
static int get_string(toml_table_t *tab, const char *key, char *out,
                      size_t outlen, const char *where, char *err,
                      size_t errlen) {
    if (!tab || !toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok) {
        snprintf(err, errlen, "%s%s must be a string", where, key);
        return -1;
    }
    snprintf(out, outlen, "%s", d.u.s);
    free(d.u.s);
    return 1;
}

static int get_bool(toml_table_t *tab, const char *key, bool *out,
                    const char *where, char *err, size_t errlen) {
    if (!tab || !toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_bool_in(tab, key);
    if (!d.ok) {
        snprintf(err, errlen, "%s%s must be true or false", where, key);
        return -1;
    }
    *out = d.u.b;
    return 1;
}

/* A configured name, or the full policy URI. */
static const ua_policy_t *parse_policy(const char *value) {
    size_t plen = strlen(UA_POLICY_URI_PREFIX);
    const char *name =
        strncmp(value, UA_POLICY_URI_PREFIX, plen) == 0 ? value + plen : value;
    for (size_t i = 0; i < UA_NPOLICIES; i++)
        if (!strcmp(POLICIES[i].name, name))
            return &POLICIES[i];
    return NULL;
}

static const ua_policy_t *policy_from_uri(const UA_String *uri) {
    size_t plen = strlen(UA_POLICY_URI_PREFIX);
    if (uri->length <= plen ||
        strncmp((const char *)uri->data, UA_POLICY_URI_PREFIX, plen) != 0)
        return NULL;
    for (size_t i = 0; i < UA_NPOLICIES; i++)
        if (strlen(POLICIES[i].name) == uri->length - plen &&
            !strncmp(POLICIES[i].name, (const char *)uri->data + plen,
                     uri->length - plen))
            return &POLICIES[i];
    return NULL;
}

/* Any case (the packaged default config says "None"); `signandencrypt` is a
 * spelling earlier releases accepted. */
static int parse_mode(const char *value, UA_MessageSecurityMode *out) {
    if (!strcasecmp(value, "none"))
        *out = UA_MESSAGESECURITYMODE_NONE;
    else if (!strcasecmp(value, "sign"))
        *out = UA_MESSAGESECURITYMODE_SIGN;
    else if (!strcasecmp(value, "sign_and_encrypt") || !strcasecmp(value, "signandencrypt"))
        *out = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
    else
        return -1;
    return 0;
}

/* The first line of a password file, without its line ending. The error names
 * the path only. */
static char *read_password_file(const char *path, char *err, size_t errlen) {
    size_t len = 0;
    unsigned char *buf = ua_pki_read_file(path, &len);
    if (!buf) {
        snprintf(err, errlen, "password_file '%s' cannot be read: %s", path,
                 strerror(errno));
        return NULL;
    }
    size_t line = 0;
    while (line < len && buf[line] != '\n')
        line++;
    if (line > 0 && buf[line - 1] == '\r')
        line--;
    char *pw = malloc(line + 1);
    if (pw) {
        memcpy(pw, buf, line);
        pw[line] = '\0';
    }
    mbedtls_platform_zeroize(buf, len);
    free(buf);
    return pw;
}

/* An X.509 user identity (certificate DER or PEM, key PEM). */
static int load_user_certificate(const char *cert, const char *key,
                                 ua_secret_t *sec, char *err, size_t errlen) {
    size_t len = 0;
    unsigned char *bytes = ua_pki_read_file(cert, &len);
    if (!bytes) {
        snprintf(err, errlen, "user_certificate '%s' cannot be read: %s", cert,
                 strerror(errno));
        return -1;
    }
    ua_blobs_t certs = {0};
    size_t n = ua_pki_parse_certs(bytes, len, &certs, cert);
    free(bytes);
    if (n == 0) {
        snprintf(err, errlen, "user_certificate '%s' is not a certificate", cert);
        return -1;
    }
    size_t key_len = 0;
    unsigned char *key_bytes = ua_pki_read_file(key, &key_len);
    if (!key_bytes) {
        ua_blobs_free(&certs);
        snprintf(err, errlen,
                 "user_private_key '%s' cannot be read as a PEM private key", key);
        return -1;
    }
    int rc = 0;
    if (!ua_pki_key_matches(certs.items[0].der, certs.items[0].len, key_bytes,
                            key_len)) {
        snprintf(err, errlen,
                 "user_private_key '%s' does not belong to user_certificate '%s'",
                 key, cert);
        rc = -1;
    } else {
        UA_ByteString_allocBuffer(&sec->x509_cert, certs.items[0].len);
        memcpy(sec->x509_cert.data, certs.items[0].der, certs.items[0].len);
        UA_ByteString_allocBuffer(&sec->x509_key, key_len + 1);
        memcpy(sec->x509_key.data, key_bytes, key_len);
        sec->x509_key.data[key_len] = '\0';
    }
    mbedtls_platform_zeroize(key_bytes, key_len);
    free(key_bytes);
    ua_blobs_free(&certs);
    return rc;
}

/* The [connection] switches, which a device may override. */
typedef struct {
    char policy[128];
    char mode[64];
    bool has_policy, has_mode;
    bool trust_any, allow_deprecated, allow_plaintext;
} ua_defaults_t;

/* Validate one device's security settings (spec §3.1; the rules of
 * config.rs `device_security`). Errors name the field, never a value. */
static int configure_security(ua_state_t *st, const ua_defaults_t *def,
                              tdot_device_t *dev, ua_device_t *ua,
                              const char *base, char *err, size_t errlen) {
    toml_table_t *pa = dev->protocol_address;
    char prefix[300];
    snprintf(prefix, sizeof prefix, "device '%s': ", dev->name);
    char e[400] = "";
/* Formats into its own buffer: the arguments are often `e` itself, and an
 * snprintf whose output overlaps an argument is undefined (glibc empties it). */
#define FAIL(...)                                                              \
    do {                                                                       \
        char fail_msg[sizeof e];                                               \
        snprintf(fail_msg, sizeof fail_msg, __VA_ARGS__);                      \
        snprintf(err, errlen, "%s%s", prefix, fail_msg);                       \
        return -1;                                                             \
    } while (0)

    char policy_text[128] = "", mode_text[64] = "";
    int has_policy = get_string(pa, "security_policy", policy_text,
                                sizeof policy_text, "", e, sizeof e);
    int has_mode = get_string(pa, "security_mode", mode_text, sizeof mode_text,
                              "", e, sizeof e);
    if (has_policy < 0 || has_mode < 0)
        FAIL("%s", e);
    const char *ptext = has_policy ? policy_text : def->has_policy ? def->policy : NULL;
    ua->policy = &POLICIES[0];
    if (ptext) {
        ua->policy = parse_policy(ptext);
        if (!ua->policy) {
            char names[256] = "";
            for (size_t i = 0; i < UA_NPOLICIES; i++) {
                if (i)
                    strcat(names, ", ");
                strcat(names, POLICIES[i].name);
            }
            FAIL("security_policy '%s' is not one of %s", ptext, names);
        }
    }
    bool allow_deprecated = def->allow_deprecated;
    if (get_bool(pa, "allow_deprecated_security", &allow_deprecated, "", e,
                 sizeof e) < 0)
        FAIL("%s", e);
    if (ua->policy->deprecated && !allow_deprecated)
        FAIL("security_policy '%s' is deprecated (SHA-1); set "
             "allow_deprecated_security = true to use it",
             ua->policy->name);

    /* A device that sets its own policy does not inherit the connection's
     * mode: it gets its policy's default. */
    const char *mtext = has_mode     ? mode_text
                        : has_policy ? NULL
                        : def->has_mode ? def->mode
                                        : NULL;
    bool secure = ua->policy != &POLICIES[0];
    if (!mtext)
        ua->mode = secure ? UA_MESSAGESECURITYMODE_SIGNANDENCRYPT
                          : UA_MESSAGESECURITYMODE_NONE;
    else if (parse_mode(mtext, &ua->mode) != 0)
        FAIL("security_mode '%s' is not one of none, sign, sign_and_encrypt", mtext);
    if (!secure && ua->mode != UA_MESSAGESECURITYMODE_NONE)
        FAIL("security_mode '%s' needs a security_policy other than None",
             mode_name(ua->mode));
    if (secure && ua->mode == UA_MESSAGESECURITYMODE_NONE)
        FAIL("security_mode 'none' cannot be used with security_policy '%s' "
             "(use sign or sign_and_encrypt)",
             ua->policy->name);

    ua->trust_any = def->trust_any;
    ua->allow_plaintext = def->allow_plaintext;
    if (get_bool(pa, "trust_any_server_certificate", &ua->trust_any, "", e,
                 sizeof e) < 0 ||
        get_bool(pa, "allow_plaintext_password", &ua->allow_plaintext, "", e,
                 sizeof e) < 0)
        FAIL("%s", e);

    /* Identity: exactly one of anonymous, username, X.509. */
    char user[256] = "", pw_file[UA_PKI_PATH_MAX] = "";
    char ucert[UA_PKI_PATH_MAX] = "", ukey[UA_PKI_PATH_MAX] = "";
    int has_user = get_string(pa, "user", user, sizeof user, "", e, sizeof e);
    if (has_user < 0)
        FAIL("%s", e);
    bool has_password = pa && toml_key_exists(pa, "password");
    if (has_password) {
        /* Type check only: the value is read again below, where it is kept. */
        toml_datum_t d = toml_string_in(pa, "password");
        if (!d.ok)
            FAIL("a password must be a string (or use password_file)");
        mbedtls_platform_zeroize(d.u.s, strlen(d.u.s));
        free(d.u.s);
    }
    int has_pw_file = get_string(pa, "password_file", pw_file, sizeof pw_file,
                                 "", e, sizeof e);
    int has_ucert = get_string(pa, "user_certificate", ucert, sizeof ucert, "",
                               e, sizeof e);
    int has_ukey = get_string(pa, "user_private_key", ukey, sizeof ukey, "", e,
                              sizeof e);
    if (has_pw_file < 0 || has_ucert < 0 || has_ukey < 0)
        FAIL("%s", e);
    bool username_fields = has_user || has_password || has_pw_file;
    bool cert_fields = has_ucert || has_ukey;
    if (username_fields && cert_fields)
        FAIL("user/password/password_file and user_certificate/user_private_key "
             "are both set; use one identity");
    ua->identity = UA_ID_ANONYMOUS;
    if (cert_fields) {
        if (!has_ukey)
            FAIL("user_certificate needs user_private_key");
        if (!has_ucert)
            FAIL("user_private_key needs user_certificate");
        char cpath[UA_PKI_PATH_MAX], kpath[UA_PKI_PATH_MAX];
        resolve_path(base, ucert, cpath, sizeof cpath);
        resolve_path(base, ukey, kpath, sizeof kpath);
        if (ua_pki_check_key_mode(kpath, "user_private_key", e, sizeof e) != 0)
            FAIL("%s", e);
        ua_secret_t *sec = new_secret(st, &ua->secret);
        if (!sec)
            FAIL("out of memory");
        if (load_user_certificate(cpath, kpath, sec, e, sizeof e) != 0)
            FAIL("%s", e);
        ua->identity = UA_ID_X509;
    } else if (!has_user) {
        if (has_password || has_pw_file)
            FAIL("password/password_file needs user");
    } else {
        if (has_password && has_pw_file)
            FAIL("password and password_file are both set; use one");
        ua_secret_t *sec = new_secret(st, &ua->secret);
        if (!sec)
            FAIL("out of memory");
        snprintf(sec->user, sizeof sec->user, "%s", user);
        if (has_password) {
            toml_datum_t d = toml_string_in(pa, "password");
            sec->password = strdup(d.u.s);
            mbedtls_platform_zeroize(d.u.s, strlen(d.u.s));
            free(d.u.s);
            if (!sec->password)
                FAIL("out of memory");
        } else if (has_pw_file) {
            char path[UA_PKI_PATH_MAX];
            resolve_path(base, pw_file, path, sizeof path);
            sec->password = read_password_file(path, e, sizeof e);
            if (!sec->password)
                FAIL("%s", e);
        } else {
            sec->password = strdup("");
        }
        ua->identity = UA_ID_USERNAME;
    }
#undef FAIL
    return 0;
}

/* Warn (at most daily) while the application certificate is close to or past
 * its expiry. */
static void warn_expiry(ua_state_t *st) {
    if (!st->own_loaded)
        return;
    time_t now = time(NULL);
    if (st->expiry_warned && now - st->expiry_warned < 86400)
        return;
    ua_cert_info_t info;
    if (ua_pki_cert_info(st->own_cert.data, st->own_cert.length, &info) != 0)
        return;
    if (now > info.not_after_t)
        fprintf(stderr,
                "warn  the application certificate %s expired on %s; secured "
                "devices cannot connect\n",
                st->cert_path, info.not_after);
    else if (info.not_after_t - now <= UA_PKI_EXPIRY_WARNING_DAYS * 86400L)
        fprintf(stderr,
                "warn  the application certificate %s expires in %ld day(s), "
                "on %s; renew it with `tedge-dot pki create --force`\n",
                st->cert_path, (long)((info.not_after_t - now) / 86400),
                info.not_after);
    else
        return;
    st->expiry_warned = now;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                     size_t errlen) {
    ua_state_t *st = self->state;
    wipe_secrets(st);
    st->expiry_warned = 0;
    snprintf(st->application_name, sizeof st->application_name, "tedge-dot");
    snprintf(st->application_uri, sizeof st->application_uri, "urn:tedge-dot");
    st->connect_timeout_s = 15;
    /* connector.operation_timeout is the contract-level bound on one protocol
     * call (§8.1); open62541's client timeout is where it actually takes
     * effect, since this runtime cannot cancel a call in flight. The
     * opcua-specific [connection] request_timeout_s still wins when set, so an
     * existing config keeps its tuning. */
    st->request_timeout_s = (int)(cfg->operation_timeout_s + 0.5);
    if (st->request_timeout_s < 1)
        st->request_timeout_s = 1;

    char base[UA_PKI_PATH_MAX];
    config_dir(cfg, base, sizeof base);
    ua_defaults_t def = {0};
    char pki_dir[UA_PKI_PATH_MAX] = UA_PKI_DEFAULT_DIR;
    char cert[UA_PKI_PATH_MAX] = "", key[UA_PKI_PATH_MAX] = "";
    st->create_certificate = true;
    toml_table_t *c = cfg->connection;
    if (c) {
        toml_datum_t d;
        const char *w = "[connection]: ";
        if (get_string(c, "application_name", st->application_name,
                       sizeof st->application_name, w, err, errlen) < 0 ||
            get_string(c, "application_uri", st->application_uri,
                       sizeof st->application_uri, w, err, errlen) < 0 ||
            get_string(c, "pki_dir", pki_dir, sizeof pki_dir, w, err, errlen) < 0 ||
            get_string(c, "certificate", cert, sizeof cert, w, err, errlen) < 0 ||
            get_string(c, "private_key", key, sizeof key, w, err, errlen) < 0 ||
            get_bool(c, "create_certificate", &st->create_certificate, w, err,
                     errlen) < 0 ||
            get_bool(c, "trust_any_server_certificate", &def.trust_any, w, err,
                     errlen) < 0 ||
            get_bool(c, "allow_deprecated_security", &def.allow_deprecated, w,
                     err, errlen) < 0 ||
            get_bool(c, "allow_plaintext_password", &def.allow_plaintext, w, err,
                     errlen) < 0)
            return -1;
        int hp = get_string(c, "security_policy", def.policy, sizeof def.policy,
                            w, err, errlen);
        int hm = get_string(c, "security_mode", def.mode, sizeof def.mode, w,
                            err, errlen);
        if (hp < 0 || hm < 0)
            return -1;
        def.has_policy = hp > 0;
        def.has_mode = hm > 0;
        if ((d = toml_int_in(c, "connect_timeout_s")).ok)
            st->connect_timeout_s = (int)d.u.i;
        if ((d = toml_int_in(c, "request_timeout_s")).ok)
            st->request_timeout_s = (int)d.u.i;
    }
    resolve_path(base, pki_dir, st->pki_root, sizeof st->pki_root);
    st->explicit_cert = *cert || *key;
    if (*cert && *key) {
        resolve_path(base, cert, st->cert_path, sizeof st->cert_path);
        resolve_path(base, key, st->key_path, sizeof st->key_path);
    } else {
        snprintf(st->cert_path, sizeof st->cert_path, "%s/%s", st->pki_root,
                 UA_PKI_OWN_CERT);
        snprintf(st->key_path, sizeof st->key_path, "%s/%s", st->pki_root,
                 UA_PKI_OWN_KEY);
    }

    bool any_secure = false;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_device_t *dev = &cfg->devices[i];
        ua_device_t *ua = calloc(1, sizeof *ua);
        dev->proto = ua;

        toml_datum_t d = toml_string_in(dev->protocol_address, "endpoint");
        if (!d.ok) {
            snprintf(err, errlen, "device %s: protocol_address requires "
                                  "endpoint",
                     dev->name);
            return -1;
        }
        snprintf(ua->endpoint, sizeof ua->endpoint, "%s", d.u.s);
        free(d.u.s);

        if (configure_security(st, &def, dev, ua, base, err, errlen) != 0)
            return -1;
        if (ua->policy != &POLICIES[0])
            any_secure = true;

        for (size_t j = 0; j < dev->npoints; j++) {
            tdot_point_t *pt = &dev->points[j];
            /* Two address forms, like the Rust module: `node_id = "ns=2;s=X"`
             * or structured `namespace = 2, identifier = "X" | 1001`. */
            ua_point_t *up = calloc(1, sizeof *up);
            pt->proto = up;
            toml_datum_t nd = toml_string_in(pt->address, "node_id");
            if (nd.ok) {
                snprintf(up->node_id, sizeof up->node_id, "%s", nd.u.s);
                free(nd.u.s);
            } else {
                toml_datum_t ns = toml_int_in(pt->address, "namespace");
                toml_datum_t sid = toml_string_in(pt->address, "identifier");
                toml_datum_t iid = toml_int_in(pt->address, "identifier");
                if (!ns.ok || (!sid.ok && !iid.ok)) {
                    if (sid.ok)
                        free(sid.u.s);
                    snprintf(err, errlen,
                             "point %s/%s: address requires node_id, or "
                             "namespace + identifier",
                             dev->name, pt->id);
                    return -1;
                }
                if (sid.ok) {
                    snprintf(up->node_id, sizeof up->node_id, "ns=%d;s=%s",
                             (int)ns.u.i, sid.u.s);
                    free(sid.u.s);
                } else {
                    snprintf(up->node_id, sizeof up->node_id, "ns=%d;i=%lld",
                             (int)ns.u.i, (long long)iid.u.i);
                }
            }

            cJSON *addr = cJSON_CreateObject();
            cJSON_AddStringToObject(addr, "node_id", up->node_id);
            pt->addr_json = cJSON_PrintUnformatted(addr);
            cJSON_Delete(addr);
        }
    }

    /* Only a configuration with a secured device needs (and may create) a
     * certificate. A failure is reported by each secured device's link. */
    if (any_secure) {
        if (st->explicit_cert && !(*cert && *key)) {
            snprintf(st->own_err, sizeof st->own_err,
                     "certificate and private_key must be set together");
        } else {
            ua_cert_request_t req = {
                .application_name = st->application_name,
                .application_uri = st->application_uri,
                .days = UA_PKI_DEFAULT_DAYS,
            };
            bool generated = false;
            if (ua_pki_load_or_create_own(st->pki_root, st->cert_path,
                                          st->key_path, st->explicit_cert,
                                          st->create_certificate, &req,
                                          &st->own_cert, &st->own_key,
                                          &generated, st->own_err,
                                          sizeof st->own_err) == 0) {
                st->own_loaded = true;
                if (generated) {
                    char tp[41];
                    ua_pki_thumbprint(st->own_cert.data, st->own_cert.length, tp);
                    fprintf(stderr,
                            "info  generated the application certificate %s "
                            "(thumbprint %s)\n",
                            st->cert_path, tp);
                }
            }
        }
        if (!st->own_loaded)
            fprintf(stderr, "warn  application certificate: %s\n", st->own_err);
        warn_expiry(st);
    }
    return 0;
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self;
    ua_device_t *ua = dev->proto;
    if (ua && ua->client) {
        UA_Client_disconnect(ua->client);
        UA_Client_delete(ua->client);
        ua->client = NULL;
    }
    if (ua) {
        /* The subscription died with the session. Drop the id and anything
         * still queued so a reconnect cannot deliver samples belonging to the
         * previous session, or reuse its subscription id. The runtime has
         * already cleared pt->subscribed for every point. */
        ua->sub_id = 0;
        ua->subscribed = false;
        ua->sub_lost = false;
        ua->head = ua->tail = 0;
        ua->dropped = 0;
    }
}

/* ---- secured connect (spec §5-§8) ------------------------------------- */

/* Reason prefixes (spec §8; the Rust module's security.rs). */
#define R_UNTRUSTED "certificate untrusted:"
#define R_INVALID "certificate invalid:"
#define R_REVOKED "certificate revoked:"
#define R_NO_ENDPOINT "no matching endpoint:"
#define R_IDENTITY_REJECTED "identity rejected:"
#define R_IDENTITY_UNSUPPORTED "identity unsupported:"
#define R_PLAINTEXT "plaintext password refused:"
#define R_APPCERT "application certificate:"
/* The server refused *our* certificate. Told apart from R_UNTRUSTED by
 * ordering, not by status code: both directions of distrust arrive as
 * BadSecurityChecksFailed / BadCertificateUntrusted, so only the fact that our
 * own check of the server already passed (or was skipped) says whose
 * judgement failed. */
#define R_APPCERT_REJECTED "application certificate rejected:"

static const char *reason_category(UA_StatusCode rc) {
    switch (rc & 0xFFFF0000u) {
    case UA_STATUSCODE_BADCERTIFICATEUNTRUSTED:
    case UA_STATUSCODE_BADSECURITYCHECKSFAILED:
        return R_UNTRUSTED;
    case UA_STATUSCODE_BADCERTIFICATETIMEINVALID:
    case UA_STATUSCODE_BADCERTIFICATEISSUERTIMEINVALID:
    case UA_STATUSCODE_BADCERTIFICATEHOSTNAMEINVALID:
    case UA_STATUSCODE_BADCERTIFICATEURIINVALID:
    case UA_STATUSCODE_BADCERTIFICATEINVALID:
    case UA_STATUSCODE_BADCERTIFICATEPOLICYCHECKFAILED:
    case UA_STATUSCODE_BADCERTIFICATEUSENOTALLOWED:
    case UA_STATUSCODE_BADCERTIFICATEISSUERUSENOTALLOWED:
    case UA_STATUSCODE_BADCERTIFICATECHAININCOMPLETE:
        return R_INVALID;
    case UA_STATUSCODE_BADCERTIFICATEREVOKED:
    case UA_STATUSCODE_BADCERTIFICATEISSUERREVOKED:
    case UA_STATUSCODE_BADCERTIFICATEREVOCATIONUNKNOWN:
    case UA_STATUSCODE_BADCERTIFICATEISSUERREVOCATIONUNKNOWN:
        return R_REVOKED;
    case UA_STATUSCODE_BADIDENTITYTOKENREJECTED:
    case UA_STATUSCODE_BADIDENTITYTOKENINVALID:
    case UA_STATUSCODE_BADUSERACCESSDENIED:
        return R_IDENTITY_REJECTED;
    default:
        return NULL;
    }
}

static const char *reason_text(UA_StatusCode rc) {
    switch (rc & 0xFFFF0000u) {
    case UA_STATUSCODE_BADCERTIFICATEUNTRUSTED:
    case UA_STATUSCODE_BADSECURITYCHECKSFAILED:
        return "the server certificate is not trusted";
    case UA_STATUSCODE_BADCERTIFICATETIMEINVALID:
        return "the server certificate is expired or not yet valid";
    case UA_STATUSCODE_BADCERTIFICATEISSUERTIMEINVALID:
        return "a CA certificate on the chain is expired or not yet valid";
    case UA_STATUSCODE_BADCERTIFICATEHOSTNAMEINVALID:
        return "the server certificate does not name the endpoint host";
    case UA_STATUSCODE_BADCERTIFICATEURIINVALID:
        return "the server certificate's application URI does not match the server";
    case UA_STATUSCODE_BADCERTIFICATEPOLICYCHECKFAILED:
        return "the server certificate's key does not meet the security policy";
    case UA_STATUSCODE_BADCERTIFICATEREVOKED:
        return "the server certificate is revoked";
    case UA_STATUSCODE_BADCERTIFICATEISSUERREVOKED:
        return "a CA certificate on the chain is revoked";
    case UA_STATUSCODE_BADCERTIFICATEREVOCATIONUNKNOWN:
        return "revocation unknown: no CRL for the issuing CA (add its CRL, an "
               "empty one if it revoked nothing)";
    case UA_STATUSCODE_BADCERTIFICATEISSUERREVOCATIONUNKNOWN:
        return "revocation unknown: no CRL for a CA on the chain (add its CRL, an "
               "empty one if it revoked nothing)";
    case UA_STATUSCODE_BADIDENTITYTOKENREJECTED:
    case UA_STATUSCODE_BADIDENTITYTOKENINVALID:
    case UA_STATUSCODE_BADUSERACCESSDENIED:
        return "the server rejected the user identity";
    default:
        return "the server certificate is invalid";
    }
}

/* The length of the first DER object (a certificate chain is concatenated
 * DER; the application instance certificate comes first). */
static size_t first_der_len(const UA_ByteString *b) {
    const UA_Byte *p = b->data;
    size_t n = b->length;
    if (n < 2 || p[0] != 0x30)
        return n;
    size_t hdr = 2, len = p[1];
    if (len & 0x80) {
        size_t k = len & 0x7f;
        if (k == 0 || k > 4 || n < 2 + k)
            return n;
        len = 0;
        for (size_t i = 0; i < k; i++)
            len = (len << 8) | p[2 + i];
        hdr += k;
    }
    return hdr + len <= n ? hdr + len : n;
}

/* The certificate verification the session runs on the channel's server
 * certificate: the same trust decision as the pre-check (so a certificate
 * that changed between the two is still caught), with the host name and URI
 * already checked on the endpoint. */
typedef struct {
    char root[UA_PKI_PATH_MAX];
    size_t min_bits, max_bits;
    UA_StatusCode last; /* what the session's check concluded */
} ua_verify_ctx_t;

static UA_StatusCode verify_server_certificate(UA_CertificateGroup *g,
                                               const UA_ByteString *cert) {
    ua_verify_ctx_t *ctx = g->context;
    if (!ctx || !cert)
        return UA_STATUSCODE_BADINTERNALERROR;
    ctx->last = ua_pki_validate(ctx->root, cert->data, first_der_len(cert),
                                ctx->min_bits, ctx->max_bits, NULL, NULL);
    return ctx->last;
}

static void clear_verify(UA_CertificateGroup *g) {
    free(g->context);
    g->context = NULL;
}

static void install_verification(UA_ClientConfig *cc, const char *root,
                                 const ua_policy_t *policy, bool trust_any) {
    if (cc->certificateVerification.clear)
        cc->certificateVerification.clear(&cc->certificateVerification);
    const UA_Logger *logging = cc->certificateVerification.logging;
    memset(&cc->certificateVerification, 0, sizeof cc->certificateVerification);
    cc->certificateVerification.logging = logging ? logging : cc->logging;
    if (trust_any) {
        UA_CertificateGroup_AcceptAll(&cc->certificateVerification);
        return;
    }
    ua_verify_ctx_t *ctx = calloc(1, sizeof *ctx);
    if (ctx) {
        snprintf(ctx->root, sizeof ctx->root, "%s", root);
        ctx->min_bits = policy->min_bits;
        ctx->max_bits = policy->max_bits;
    }
    cc->certificateVerification.context = ctx;
    cc->certificateVerification.verifyCertificate = verify_server_certificate;
    cc->certificateVerification.clear = clear_verify;
}

/* Never prompt for a key password: the keys tedge-dot handles are not
 * encrypted, and a service has no terminal. */
static UA_StatusCode no_key_password(UA_ClientConfig *cc, UA_ByteString *pw) {
    (void)cc;
    (void)pw;
    return UA_STATUSCODE_BADSECURITYCHECKSFAILED;
}

/* The host of an `opc.tcp://host:port/path` URL (IPv6 brackets stripped). */
static void url_host(const char *url, char *out, size_t outlen) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *end = p + strcspn(p, "/");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at)
        p = at + 1;
    if (*p == '[') {
        const char *close = memchr(p, ']', (size_t)(end - p));
        if (close)
            snprintf(out, outlen, "%.*s", (int)(close - p - 1), p + 1);
        else
            snprintf(out, outlen, "%.*s", (int)(end - p), p);
        return;
    }
    const char *colon = NULL;
    for (const char *q = p; q < end; q++)
        if (*q == ':')
            colon = q;
    snprintf(out, outlen, "%.*s", (int)((colon ? colon : end) - p), p);
}

static UA_UserTokenType token_type(ua_identity_t id) {
    switch (id) {
    case UA_ID_USERNAME: return UA_USERTOKENTYPE_USERNAME;
    case UA_ID_X509: return UA_USERTOKENTYPE_CERTIFICATE;
    default: return UA_USERTOKENTYPE_ANONYMOUS;
    }
}

static const UA_UserTokenPolicy *find_token(const UA_EndpointDescription *e,
                                            UA_UserTokenType type) {
    for (size_t i = 0; i < e->userIdentityTokensSize; i++)
        if (e->userIdentityTokens[i].tokenType == type)
            return &e->userIdentityTokens[i];
    return NULL;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The endpoint a device connects to (spec "Endpoint selection"; the rules of
 * security.rs `select_endpoint`), or NULL with the reason in err. */
static const UA_EndpointDescription *
select_endpoint(const UA_EndpointDescription *eps, size_t n,
                const ua_device_t *ua, char *err, size_t errlen) {
    const UA_EndpointDescription *best = NULL;
    bool any_match = false;
    for (size_t i = 0; i < n; i++) {
        const UA_EndpointDescription *e = &eps[i];
        if (policy_from_uri(&e->securityPolicyUri) != ua->policy ||
            e->securityMode != ua->mode)
            continue;
        any_match = true;
        /* A token policy of the identity's type must be listed, anonymous
         * included: open62541 cannot activate a session without one. */
        bool usable = find_token(e, token_type(ua->identity)) != NULL;
        if (usable && (!best || e->securityLevel > best->securityLevel))
            best = e;
    }
    if (best)
        return best;
    if (any_match) {
        snprintf(err, errlen,
                 R_IDENTITY_UNSUPPORTED " the server's %s/%s endpoint accepts no "
                 "%s user identity",
                 ua->policy->name, mode_name(ua->mode), identity_kind(ua->identity));
        return NULL;
    }
    /* "Policy/mode" of every endpoint, sorted and deduplicated. */
    char **offers = calloc(n ? n : 1, sizeof *offers);
    size_t k = 0;
    for (size_t i = 0; offers && i < n; i++) {
        const ua_policy_t *p = policy_from_uri(&eps[i].securityPolicyUri);
        char buf[300];
        if (p)
            snprintf(buf, sizeof buf, "%s/%s", p->name, mode_name(eps[i].securityMode));
        else
            snprintf(buf, sizeof buf, "%.*s/%s", (int)eps[i].securityPolicyUri.length,
                     (const char *)eps[i].securityPolicyUri.data,
                     mode_name(eps[i].securityMode));
        offers[k++] = strdup(buf);
    }
    if (offers)
        qsort(offers, k, sizeof *offers, cmp_str);
    char list[600] = "";
    size_t used = 0;
    for (size_t i = 0; i < k; i++) {
        if (i && !strcmp(offers[i], offers[i - 1]))
            continue;
        int w = snprintf(list + used, sizeof list - used, "%s%s", used ? ", " : "",
                         offers[i]);
        if (w > 0 && (size_t)w < sizeof list - used)
            used += (size_t)w;
    }
    for (size_t i = 0; i < k; i++)
        free(offers[i]);
    free(offers);
    snprintf(err, errlen,
             R_NO_ENDPOINT " the server offers no %s/%s endpoint (it offers %s)",
             ua->policy->name, mode_name(ua->mode), used ? list : "none");
    return NULL;
}

/* Whether a password sent to `e` would be unprotected (security.rs
 * password_in_plaintext): any password on a channel without message security,
 * where no server certificate is authenticated, even with token encryption;
 * and a token policy that names None on a signed-only channel. */
static bool password_in_plaintext(const UA_EndpointDescription *e) {
    const UA_UserTokenPolicy *t = find_token(e, UA_USERTOKENTYPE_USERNAME);
    if (!t)
        return false;
    if (e->securityMode == UA_MESSAGESECURITYMODE_NONE)
        return true;
    return e->securityMode == UA_MESSAGESECURITYMODE_SIGN &&
           t->securityPolicyUri.length > 0 &&
           policy_from_uri(&t->securityPolicyUri) == &POLICIES[0];
}

static void set_application(UA_ClientConfig *cc, const ua_state_t *st) {
    UA_LocalizedText_clear(&cc->clientDescription.applicationName);
    cc->clientDescription.applicationName.locale = UA_STRING_ALLOC("en");
    cc->clientDescription.applicationName.text = UA_STRING_ALLOC(st->application_name);
    UA_String_clear(&cc->clientDescription.applicationUri);
    cc->clientDescription.applicationUri = UA_STRING_ALLOC(st->application_uri);
    /* keep the client quiet unless debugging (TDOT_OPCUA_DEBUG=1 keeps
     * open62541's own handshake log on stdout) */
    if (!getenv("TDOT_OPCUA_DEBUG"))
        cc->logging->log = NULL;
    /* The handshake gets its own bound, as in the Rust module, which wraps
     * wait_for_connection() in connect_timeout_s: establishing a session is
     * several round trips and a slow-but-working server should not be cut off
     * by the per-request timeout. Restored to request_timeout_s once the
     * session is up, so ordinary reads keep the tighter bound. */
    cc->timeout = (UA_UInt32)st->connect_timeout_s * 1000;
}

static UA_Client *new_client(const ua_state_t *st) {
    UA_Client *client = UA_Client_new();
    UA_ClientConfig *cc = UA_Client_getConfig(client);
    UA_ClientConfig_setDefault(cc);
    set_application(cc, st);
    return client;
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev,
                          char *err, size_t errlen) {
    ua_state_t *st = self->state;
    ua_device_t *ua = dev->proto;
    disconnect_device(self, dev);
    warn_expiry(st);
    ua->server_thumbprint[0] = '\0';
    ua->server_certificate = NULL;
    bool secure = ua->policy != &POLICIES[0];

    if (secure) {
        if (!st->own_loaded) {
            snprintf(err, errlen, R_APPCERT " %s",
                     *st->own_err ? st->own_err : "none loaded");
            return -1;
        }
        ua_cert_info_t own;
        if (ua_pki_cert_info(st->own_cert.data, st->own_cert.length, &own) == 0 &&
            time(NULL) > own.not_after_t) {
            snprintf(err, errlen, R_APPCERT " %s expired on %s", st->cert_path,
                     own.not_after);
            return -1;
        }
    }

    UA_EndpointDescription chosen;
    UA_EndpointDescription_init(&chosen);
    bool verified = false;

    if (secure || ua->identity != UA_ID_ANONYMOUS) {
        /* Discover, then dial the configured address (spec "Endpoint
         * selection"): servers behind NAT advertise URLs we cannot reach. */
        UA_Client *probe = new_client(st);
        /* GetEndpoints runs over a channel without security, which servers allow for
         * discovery even when they offer no None endpoint. Configuring that channel
         * as the exact endpoint keeps open62541 from first selecting an endpoint it
         * could open a session on -- which fails on such a server. */
        UA_ClientConfig *pc = UA_Client_getConfig(probe);
        UA_EndpointDescription_clear(&pc->endpoint);
        pc->endpoint.endpointUrl = UA_STRING_ALLOC(ua->endpoint);
        pc->endpoint.securityMode = UA_MESSAGESECURITYMODE_NONE;
        pc->endpoint.securityPolicyUri = UA_STRING_ALLOC(UA_POLICY_URI_PREFIX "None");
        pc->endpoint.transportProfileUri = UA_STRING_ALLOC(
            "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary");
        size_t n = 0;
        UA_EndpointDescription *eps = NULL;
        UA_StatusCode rc = UA_Client_getEndpoints(probe, ua->endpoint, &n, &eps);
        UA_Client_delete(probe);
        if (rc != UA_STATUSCODE_GOOD) {
            snprintf(err, errlen, "connect %s: %s", ua->endpoint,
                     UA_StatusCode_name(rc));
            return -1;
        }
        const UA_EndpointDescription *sel = select_endpoint(eps, n, ua, err, errlen);
        if (sel)
            UA_EndpointDescription_copy(sel, &chosen);
        UA_Array_delete(eps, n, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
        if (!sel)
            return -1;
        UA_String_clear(&chosen.endpointUrl);
        chosen.endpointUrl = UA_STRING_ALLOC(ua->endpoint);

        if (ua->identity == UA_ID_USERNAME && password_in_plaintext(&chosen)) {
            if (!ua->allow_plaintext) {
                snprintf(err, errlen,
                         R_PLAINTEXT " the password would be readable by the server's "
                         "endpoint without an authenticated server certificate, or in "
                         "clear (a channel without message security, or a signed-only "
                         "channel whose token policy is None); use sign_and_encrypt or "
                         "set allow_plaintext_password = true");
                UA_EndpointDescription_clear(&chosen);
                return -1;
            }
            fprintf(stderr,
                    "warn  device %s: sending the password unencrypted "
                    "(allow_plaintext_password)\n",
                    dev->name);
        }

        if (secure) {
            const UA_ByteString *sc = &chosen.serverCertificate;
            size_t leaf = first_der_len(sc);
            ua_cert_info_t info;
            if (sc->length == 0 || ua_pki_cert_info(sc->data, leaf, &info) != 0) {
                snprintf(err, errlen,
                         R_INVALID " the server advertises no usable certificate");
                UA_EndpointDescription_clear(&chosen);
                return -1;
            }
            snprintf(ua->server_thumbprint, sizeof ua->server_thumbprint, "%s",
                     info.thumbprint);
            /* The key length belongs to the security policy (OPC UA Part 7),
             * not to trust, so it is checked before the trust store AND before
             * trust_any: that option says "do not judge who this server is",
             * not "use a key the policy forbids". open62541 refuses such a key
             * in the policy itself, where no trust setting can reach it, but it
             * reports BadCertificateUseNotAllowed with nothing to act on --
             * saying which key and which policy is the point of doing it here.
             * The Rust build checks this at the same place. */
            if (info.key_bits < ua->policy->min_bits ||
                info.key_bits > ua->policy->max_bits) {
                snprintf(err, errlen,
                         R_INVALID " the server certificate's key is %zu bits, which %s "
                         "does not allow (it requires %zu-%zu) (thumbprint %s, subject %s)",
                         info.key_bits, ua->policy->name, ua->policy->min_bits,
                         ua->policy->max_bits, info.thumbprint, info.subject);
                UA_EndpointDescription_clear(&chosen);
                return -1;
            }
            if (ua->trust_any) {
                fprintf(stderr,
                        "warn  device %s: server certificate %s is accepted "
                        "without verification (trust_any_server_certificate)\n",
                        dev->name, info.thumbprint);
                ua->server_certificate = "not_verified";
            } else {
                char host[256], app_uri[512];
                url_host(ua->endpoint, host, sizeof host);
                snprintf(app_uri, sizeof app_uri, "%.*s",
                         (int)chosen.server.applicationUri.length,
                         (const char *)chosen.server.applicationUri.data);
                UA_StatusCode vr = ua_pki_validate(st->pki_root, sc->data, leaf,
                                                   ua->policy->min_bits,
                                                   ua->policy->max_bits, host,
                                                   app_uri);
                if (vr != UA_STATUSCODE_GOOD) {
                    const char *cat = reason_category(vr);
                    if (!cat)
                        cat = R_INVALID;
                    int w = snprintf(err, errlen, "%s %s (%s; thumbprint %s, subject %s)",
                                     cat, reason_text(vr), UA_StatusCode_name(vr),
                                     info.thumbprint, info.subject);
                    if (!strcmp(cat, R_UNTRUSTED) && w > 0 && (size_t)w < errlen)
                        snprintf(err + w, errlen - (size_t)w,
                                 "; trust it with `tedge-dot pki trust %.8s`",
                                 info.thumbprint);
                    UA_EndpointDescription_clear(&chosen);
                    return -1;
                }
                ua->server_certificate = "trusted";
                verified = true;
            }
        }
    }

    ua->client = UA_Client_new();
    UA_ClientConfig *cc = UA_Client_getConfig(ua->client);
    UA_ClientConfig_setDefault(cc);
    cc->privateKeyPasswordCallback = no_key_password;
    if (secure) {
        UA_StatusCode er = UA_ClientConfig_setDefaultEncryption(
            cc, st->own_cert, st->own_key, NULL, 0, NULL, 0);
        if (er != UA_STATUSCODE_GOOD) {
            snprintf(err, errlen, R_APPCERT " cannot be used: %s",
                     UA_StatusCode_name(er));
            UA_EndpointDescription_clear(&chosen);
            UA_Client_delete(ua->client);
            ua->client = NULL;
            return -1;
        }
        install_verification(cc, st->pki_root, ua->policy, ua->trust_any);
    }
    set_application(cc, st);
    cc->securityMode = ua->mode;
    UA_String_clear(&cc->securityPolicyUri);
    char policy_uri[200];
    snprintf(policy_uri, sizeof policy_uri, UA_POLICY_URI_PREFIX "%s", ua->policy->name);
    cc->securityPolicyUri = UA_STRING_ALLOC(policy_uri);
    if (chosen.endpointUrl.length > 0) {
        UA_EndpointDescription_clear(&cc->endpoint);
        cc->endpoint = chosen; /* moved */
        UA_EndpointDescription_init(&chosen);
    }
    cc->allowNonePolicyPassword = ua->allow_plaintext;
    UA_StatusCode rc = UA_STATUSCODE_GOOD;
    if (ua->identity == UA_ID_USERNAME) {
        ua_secret_t *sec = &st->secrets[ua->secret];
        rc = UA_ClientConfig_setAuthenticationUsername(cc, sec->user, sec->password);
    } else if (ua->identity == UA_ID_X509) {
        ua_secret_t *sec = &st->secrets[ua->secret];
        rc = UA_ClientConfig_setAuthenticationCert(cc, sec->x509_cert, sec->x509_key);
    }
    if (rc == UA_STATUSCODE_GOOD)
        rc = UA_Client_connect(ua->client, ua->endpoint);
    if (rc != UA_STATUSCODE_GOOD) {
        const char *cat = reason_category(rc);
        /* The server certificate passed our checks, so a trust refusal is
         * the server's: it does not accept this connector's certificate. */
        if (cat && !strcmp(cat, R_UNTRUSTED) && secure &&
            (verified || ua->trust_any)) {
            char tp[41];
            ua_pki_thumbprint(st->own_cert.data, st->own_cert.length, tp);
            snprintf(err, errlen,
                     R_APPCERT_REJECTED " the server rejected the connection (%s); it "
                     "does not trust this connector's application certificate "
                     "(thumbprint %s, export it with `tedge-dot pki export` and "
                     "have the server administrator trust it)",
                     UA_StatusCode_name(rc), tp);
        } else if (cat && !strcmp(cat, R_IDENTITY_REJECTED)) {
            snprintf(err, errlen,
                     R_IDENTITY_REJECTED " the server rejected the %s identity (%s)",
                     identity_kind(ua->identity), UA_StatusCode_name(rc));
        } else if (cat) {
            snprintf(err, errlen, "%s %s (%s)", cat, reason_text(rc),
                     UA_StatusCode_name(rc));
        } else {
            snprintf(err, errlen, "connect %s: %s", ua->endpoint,
                     UA_StatusCode_name(rc));
        }
        UA_Client_delete(ua->client);
        ua->client = NULL;
        return -1;
    }
    /* Session established: from here every request is bounded by
     * connector.operation_timeout (see configure()). open62541 reads
     * config.timeout per request, so changing it now applies to all of them. */
    cc->timeout = (UA_UInt32)st->request_timeout_s * 1000;
    return 0;
}

/* Link status `info` (spec §8): the endpoint, the effective security and what
 * is known of the server certificate. Never a credential. */
static char *device_info(tdot_connector_t *self, const tdot_device_t *dev) {
    (void)self;
    const ua_device_t *ua = dev->proto;
    if (!ua || !ua->policy)
        return NULL;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "endpoint", ua->endpoint);
    cJSON_AddStringToObject(obj, "security_policy", ua->policy->name);
    cJSON_AddStringToObject(obj, "security_mode", mode_name(ua->mode));
    if (ua->server_certificate)
        cJSON_AddStringToObject(obj, "server_certificate", ua->server_certificate);
    if (ua->server_thumbprint[0])
        cJSON_AddStringToObject(obj, "server_thumbprint", ua->server_thumbprint);
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

/* Serialize a UA variant scalar to canonical big-endian bytes + value,
 * honouring the point's configured datatype for the envelope. */
static int variant_to_sample(const UA_Variant *v, tdot_point_t *pt,
                             tdot_sample_t *out) {
    uint64_t bits = 0;
    size_t len = 0;
    tdot_value_t raw_val = {0};

    if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_BOOLEAN])) {
        bool b = *(UA_Boolean *)v->data;
        raw_val.kind = TDOT_VAL_BOOL;
        raw_val.b = b;
        bits = b ? 1 : 0;
        len = 1;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_SBYTE])) {
        int8_t x = *(UA_SByte *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = (uint8_t)x;
        len = 1;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_BYTE])) {
        uint8_t x = *(UA_Byte *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = x;
        len = 1;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_INT16])) {
        int16_t x = *(UA_Int16 *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = (uint16_t)x;
        len = 2;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_UINT16])) {
        uint16_t x = *(UA_UInt16 *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = x;
        len = 2;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_INT32])) {
        int32_t x = *(UA_Int32 *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = (uint32_t)x;
        len = 4;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_UINT32])) {
        uint32_t x = *(UA_UInt32 *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = x;
        bits = x;
        len = 4;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_INT64])) {
        int64_t x = *(UA_Int64 *)v->data;
        if (x > TDOT_JS_SAFE_MAX || x < -TDOT_JS_SAFE_MAX) {
            raw_val.kind = TDOT_VAL_STR;
            snprintf(raw_val.str, sizeof raw_val.str, "%lld", (long long)x);
        } else {
            raw_val.kind = TDOT_VAL_NUM;
            raw_val.num = (double)x;
        }
        bits = (uint64_t)x;
        len = 8;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_UINT64])) {
        uint64_t x = *(UA_UInt64 *)v->data;
        if (x > (uint64_t)TDOT_JS_SAFE_MAX) {
            raw_val.kind = TDOT_VAL_STR;
            snprintf(raw_val.str, sizeof raw_val.str, "%llu",
                     (unsigned long long)x);
        } else {
            raw_val.kind = TDOT_VAL_NUM;
            raw_val.num = (double)x;
        }
        bits = x;
        len = 8;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_FLOAT])) {
        float f = *(UA_Float *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = (double)f;
        uint32_t b32;
        memcpy(&b32, &f, 4);
        bits = b32;
        len = 4;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_DOUBLE])) {
        double d = *(UA_Double *)v->data;
        raw_val.kind = TDOT_VAL_NUM;
        raw_val.num = d;
        memcpy(&bits, &d, 8);
        len = 8;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_STRING])) {
        UA_String *s = (UA_String *)v->data;
        raw_val.kind = TDOT_VAL_STR;
        size_t n = s->length < sizeof raw_val.str - 1 ? s->length
                                                      : sizeof raw_val.str - 1;
        memcpy(raw_val.str, s->data, n);
        raw_val.str[n] = '\0';
        size_t rn = s->length < TDOT_RAW_MAX ? s->length : TDOT_RAW_MAX;
        memcpy(out->raw, s->data, rn);
        out->raw_len = rn;
        out->raw_group = 1;
        out->value = raw_val;
        return 0;
    } else if (UA_Variant_hasScalarType(v, &UA_TYPES[UA_TYPES_BYTESTRING])) {
        /* `bytes` only, as in the Rust module (structure.rs `accepted`): the
         * value is the lowercase hex of the bytes, `raw` the bytes themselves.
         * A null ByteString is an empty run. */
        const UA_ByteString *bs = (const UA_ByteString *)v->data;
        size_t n = bs->data ? bs->length : 0;
        if (pt->mode == TDOT_MODE_TYPED && pt->datatype != TDOT_DT_BYTES) {
            tdot_sample_bad(out,
                            "value is ByteString, point declares %s "
                            "(accepted: bytes)",
                            tdot_datatype_str(pt->datatype));
            return 0;
        }
        if (pt->mode == TDOT_MODE_TYPED && 2 * n >= sizeof raw_val.str) {
            /* Refused rather than cut: a truncated hex run reads as a
             * different, valid value. */
            tdot_sample_bad(out,
                            "ByteString of %zu bytes is longer than a bytes "
                            "value holds in the C build (%zu bytes)",
                            n, (sizeof raw_val.str - 1) / 2);
            return 0;
        }
        static const char HEX[] = "0123456789abcdef";
        raw_val.kind = TDOT_VAL_STR;
        if (pt->mode == TDOT_MODE_TYPED) {
            for (size_t i = 0; i < n; i++) {
                raw_val.str[2 * i] = HEX[bs->data[i] >> 4];
                raw_val.str[2 * i + 1] = HEX[bs->data[i] & 0x0f];
            }
        }
        raw_val.str[pt->mode == TDOT_MODE_TYPED ? 2 * n : 0] = '\0';
        size_t rn = n < TDOT_RAW_MAX ? n : TDOT_RAW_MAX;
        if (rn)
            memcpy(out->raw, bs->data, rn);
        out->raw_len = rn;
        out->raw_group = 1;
        out->value = raw_val;
        return 0;
    } else {
        tdot_sample_bad(out, "unsupported OPC-UA value type");
        return 0;
    }

    /* big-endian raw echo */
    for (size_t i = 0; i < len; i++)
        out->raw[i] = (uint8_t)(bits >> (8 * (len - 1 - i)));
    out->raw_len = len;
    out->raw_group = 1;
    out->value = raw_val;

    if (out->value.kind == TDOT_VAL_NUM && pt->has_transform)
        out->value.num = tdot_transform_apply(&pt->transform, out->value.num);
    return 0;
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev,
                      tdot_point_t *pt, tdot_sample_t *out) {
    (void)self;
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;

    if (!ua->client) {
        tdot_sample_bad(out, "device not connected");
        return -1;
    }

    UA_NodeId node;
    if (UA_NodeId_parse(&node, UA_STRING(up->node_id)) !=
        UA_STATUSCODE_GOOD) {
        tdot_sample_bad(out, "invalid node id: %s", up->node_id);
        return 0;
    }

    UA_Variant value;
    UA_Variant_init(&value);
    UA_StatusCode rc =
        UA_Client_readValueAttribute(ua->client, node, &value);
    UA_NodeId_clear(&node);

    if (rc != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(out, "bad status: %s", UA_StatusCode_name(rc));
        /* server answered -> transport healthy; connection loss -> down */
        bool transport_down =
            rc == UA_STATUSCODE_BADCONNECTIONCLOSED ||
            rc == UA_STATUSCODE_BADSERVERNOTCONNECTED ||
            rc == UA_STATUSCODE_BADDISCONNECT ||
            rc == UA_STATUSCODE_BADTIMEOUT ||
            rc == UA_STATUSCODE_BADSESSIONIDINVALID ||
            rc == UA_STATUSCODE_BADSECURECHANNELCLOSED ||
            rc == UA_STATUSCODE_BADINTERNALERROR;
        return transport_down ? -1 : 0;
    }

    int r = variant_to_sample(&value, pt, out);
    UA_Variant_clear(&value);
    return r;
}

static int write_point(tdot_connector_t *self, tdot_device_t *dev,
                       tdot_point_t *pt, const tdot_value_t *value, char *err,
                       size_t errlen) {
    (void)self;
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;

    if (!ua->client) {
        snprintf(err, errlen, "device not connected");
        return -1;
    }

    UA_Variant v;
    UA_Variant_init(&v);
    UA_Boolean vb;
    UA_SByte vi8;
    UA_Byte vu8;
    UA_Int16 vi16;
    UA_UInt16 vu16;
    UA_Int32 vi32;
    UA_UInt32 vu32;
    UA_Int64 vi64;
    UA_UInt64 vu64;
    UA_Float vf;
    UA_Double vd;
    UA_String vs;

    double num = value->kind == TDOT_VAL_NUM ? value->num
                 : value->kind == TDOT_VAL_STR ? strtod(value->str, NULL)
                                               : 0;
    switch (pt->datatype) {
    case TDOT_DT_BOOL:
        vb = value->kind == TDOT_VAL_BOOL ? value->b : (num != 0);
        UA_Variant_setScalar(&v, &vb, &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case TDOT_DT_INT8:
        vi8 = (UA_SByte)num;
        UA_Variant_setScalar(&v, &vi8, &UA_TYPES[UA_TYPES_SBYTE]);
        break;
    case TDOT_DT_UINT8:
        vu8 = (UA_Byte)num;
        UA_Variant_setScalar(&v, &vu8, &UA_TYPES[UA_TYPES_BYTE]);
        break;
    case TDOT_DT_INT16:
        vi16 = (UA_Int16)num;
        UA_Variant_setScalar(&v, &vi16, &UA_TYPES[UA_TYPES_INT16]);
        break;
    case TDOT_DT_UINT16:
        vu16 = (UA_UInt16)num;
        UA_Variant_setScalar(&v, &vu16, &UA_TYPES[UA_TYPES_UINT16]);
        break;
    case TDOT_DT_INT32:
        vi32 = (UA_Int32)num;
        UA_Variant_setScalar(&v, &vi32, &UA_TYPES[UA_TYPES_INT32]);
        break;
    case TDOT_DT_UINT32:
        vu32 = (UA_UInt32)num;
        UA_Variant_setScalar(&v, &vu32, &UA_TYPES[UA_TYPES_UINT32]);
        break;
    case TDOT_DT_INT64:
        vi64 = value->kind == TDOT_VAL_STR ? strtoll(value->str, NULL, 10)
                                           : (UA_Int64)num;
        UA_Variant_setScalar(&v, &vi64, &UA_TYPES[UA_TYPES_INT64]);
        break;
    case TDOT_DT_UINT64:
        vu64 = value->kind == TDOT_VAL_STR ? strtoull(value->str, NULL, 10)
                                           : (UA_UInt64)num;
        UA_Variant_setScalar(&v, &vu64, &UA_TYPES[UA_TYPES_UINT64]);
        break;
    case TDOT_DT_FLOAT32:
        vf = (UA_Float)num;
        UA_Variant_setScalar(&v, &vf, &UA_TYPES[UA_TYPES_FLOAT]);
        break;
    case TDOT_DT_FLOAT64:
        vd = (UA_Double)num;
        UA_Variant_setScalar(&v, &vd, &UA_TYPES[UA_TYPES_DOUBLE]);
        break;
    case TDOT_DT_STRING:
        if (value->kind != TDOT_VAL_STR) {
            snprintf(err, errlen, "expected string value");
            return -1;
        }
        vs = UA_STRING((char *)value->str);
        UA_Variant_setScalar(&v, &vs, &UA_TYPES[UA_TYPES_STRING]);
        break;
    case TDOT_DT_BYTES: /* read-only in both builds */
        snprintf(err, errlen, "datatype bytes is not writable over OPC-UA");
        return -1;
    default:
        snprintf(err, errlen, "write requires a datatype on the point");
        return -1;
    }

    UA_NodeId node;
    if (UA_NodeId_parse(&node, UA_STRING(up->node_id)) !=
        UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "invalid node id: %s", up->node_id);
        return -1;
    }
    UA_StatusCode rc =
        UA_Client_writeValueAttribute(ua->client, node, &v);
    UA_NodeId_clear(&node);
    if (rc != UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "write failed: %s", UA_StatusCode_name(rc));
        return -1;
    }
    return 0;
}

/* ---- push delivery (monitored items) -------------------------------------
 *
 * One subscription per device, one monitored item per subscribe-enabled point,
 * mirroring impl/rust/crates/connector-opcua. The data-change callback runs
 * inside UA_Client_run_iterate(), which the runtime calls from its own loop
 * through drain_subscriptions() -- so the callback only parks the sample in the
 * device's ring and the runtime publishes it, exactly as it does for a polled
 * one. Nothing here touches MQTT.
 */

/* A point's EFFECTIVE sampling interval (sampling_interval of point ?? device
 * ?? connector, else its resolved poll interval; config.c
 * resolve_sampling_intervals) is its monitored-item sampling interval, which is
 * what the Rust runtime hands its module in PointRef::interval. Both
 * implementations must derive it identically: the same config otherwise
 * monitors at different rates in the two builds, and a subscription sampling
 * more slowly silently coalesces away value changes the other one reports. */
double tdot_ua_sampling_interval_ms(const tdot_point_t *pt) {
    return pt->sampling_interval_s * 1000.0;
}

/* Points that opted out (subscribe = false) and write-only points stay on the
 * polling schedule; the rest set the pace. A `have` flag, not a zero
 * sentinel: zero is a valid interval and must stay the fastest. */
double tdot_ua_publishing_interval_ms(const tdot_device_t *dev, size_t *wanted) {
    *wanted = 0;
    double fastest = 0;
    for (size_t j = 0; j < dev->npoints; j++) {
        const tdot_point_t *pt = &dev->points[j];
        if (!pt->subscribe || !(pt->access & TDOT_ACCESS_READ))
            continue;
        double ms = tdot_ua_sampling_interval_ms(pt);
        if (*wanted == 0 || ms < fastest)
            fastest = ms;
        (*wanted)++;
    }
    return fastest;
}

bool tdot_ua_revised(double requested_ms, double revised_ms) {
    double delta = revised_ms - requested_ms;
    return revised_ms == revised_ms /* not NaN */ && (delta >= 1.0 || delta <= -1.0);
}

static void on_data_change(UA_Client *client, UA_UInt32 sub_id,
                           void *sub_ctx, UA_UInt32 mon_id, void *mon_ctx,
                           UA_DataValue *value) {
    (void)client;
    (void)sub_id;
    (void)mon_id;
    tdot_device_t *dev = sub_ctx;
    tdot_point_t *pt = mon_ctx;
    if (!dev || !pt)
        return;
    ua_device_t *ua = dev->proto;

    size_t next = (ua->head + 1) % UA_PUSH_QUEUE_LEN;
    if (next == ua->tail) {
        /* Ring full: the runtime has not drained for a while. Drop the NEWEST
         * change rather than overwriting the oldest, so the samples that are
         * already queued keep their order and none is silently replaced. */
        ua->dropped++;
        return;
    }

    ua_pending_t *slot = &ua->queue[ua->head];
    slot->pt = pt;
    tdot_sample_init(&slot->sample);
    if (!value->hasValue || !UA_Variant_isScalar(&value->value)) {
        tdot_sample_bad(&slot->sample, "no scalar value in notification");
    } else if (value->hasStatus && value->status != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(&slot->sample, "%s",
                        UA_StatusCode_name(value->status));
    } else {
        variant_to_sample(&value->value, pt, &slot->sample);
    }
    ua->head = next;
}

/* The subscription is gone. All three hooks below mean the same thing to us --
 * push delivery has stopped -- and are handled by dropping the link so the
 * runtime's existing reconnect/re-subscribe path runs. */
static void mark_subscription_lost(tdot_device_t *dev) {
    if (!dev)
        return;
    ua_device_t *ua = dev->proto;
    if (!ua)
        return;
    ua->subscribed = false;
    ua->sub_lost = true;
}

/* Server deleted the subscription, or it died with the session. */
static void on_subscription_deleted(UA_Client *client, UA_UInt32 sub_id,
                                    void *sub_ctx) {
    (void)client;
    (void)sub_id;
    mark_subscription_lost(sub_ctx);
}

/* Server reported a status change on the subscription (e.g. BadTimeout). */
static void on_subscription_status_change(UA_Client *client, UA_UInt32 sub_id,
                                          void *sub_ctx,
                                          UA_StatusChangeNotification *n) {
    (void)client;
    (void)sub_id;
    (void)n;
    mark_subscription_lost(sub_ctx);
}

/* No PublishResponse within the keep-alive window: the path is up but the
 * subscription is not delivering. */
static void on_subscription_inactivity(UA_Client *client, UA_UInt32 sub_id,
                                       void *sub_ctx) {
    (void)client;
    (void)sub_id;
    mark_subscription_lost(sub_ctx);
}

static int subscribe_device(tdot_connector_t *self, tdot_device_t *dev,
                            char *err, size_t errlen) {
    (void)self;
    ua_device_t *ua = dev->proto;
    if (!ua || !ua->client) {
        snprintf(err, errlen, "device not connected");
        return -1;
    }

    ua->sub_id = 0;
    ua->subscribed = false;
    ua->sub_lost = false;
    ua->head = ua->tail = 0;
    ua->dropped = 0;

    size_t wanted = 0;
    double fastest = tdot_ua_publishing_interval_ms(dev, &wanted);
    if (wanted == 0)
        return 0; /* nothing to push: armed, with no points */

    /* One subscription per device, publishing at the fastest requested
     * sampling rate -- the same parameters the Rust module uses. */
    UA_CreateSubscriptionRequest req = UA_CreateSubscriptionRequest_default();
    req.requestedPublishingInterval = fastest;
    req.requestedLifetimeCount = 60;
    req.requestedMaxKeepAliveCount = 20;
    UA_Client_getConfig(ua->client)->subscriptionInactivityCallback =
        on_subscription_inactivity;
    UA_CreateSubscriptionResponse resp = UA_Client_Subscriptions_create(
        ua->client, req, dev /*subContext*/, on_subscription_status_change,
        on_subscription_deleted);
    if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "create_subscription failed: %s",
                 UA_StatusCode_name(resp.responseHeader.serviceResult));
        UA_CreateSubscriptionResponse_clear(&resp);
        return -1;
    }
    ua->sub_id = resp.subscriptionId;
    ua->subscribed = true;
    /* The server may grant other rates than requested: say so, so a "200ms"
     * that behaves like "1s" is explained. */
    if (tdot_ua_revised(fastest, resp.revisedPublishingInterval))
        fprintf(stderr,
                "info  device %s: the server revised the subscription's publishing "
                "interval: requested %.0fms, revised %.0fms\n",
                dev->name, fastest, resp.revisedPublishingInterval);
    /* The response header can carry a heap-allocated diagnostics/string table;
     * a flapping link re-subscribes often enough for that to add up. */
    UA_CreateSubscriptionResponse_clear(&resp);

    size_t armed = 0;
    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        if (!pt->subscribe || !(pt->access & TDOT_ACCESS_READ))
            continue;
        ua_point_t *up = pt->proto;
        UA_NodeId node;
        if (UA_NodeId_parse(&node, UA_STRING(up->node_id)) !=
            UA_STATUSCODE_GOOD) {
            UA_NodeId_clear(&node);
            continue; /* the polling path reports this as a bad sample */
        }

        UA_MonitoredItemCreateRequest mreq =
            UA_MonitoredItemCreateRequest_default(node);
        mreq.requestedParameters.samplingInterval = tdot_ua_sampling_interval_ms(pt);
        mreq.requestedParameters.queueSize = 1;
        mreq.requestedParameters.discardOldest = true;
        UA_MonitoredItemCreateResult mres =
            UA_Client_MonitoredItems_createDataChange(
                ua->client, ua->sub_id, UA_TIMESTAMPSTORETURN_BOTH, mreq,
                pt /*monContext*/, on_data_change, NULL);
        UA_NodeId_clear(&node);
        if (mres.statusCode == UA_STATUSCODE_GOOD) {
            /* Telling the runtime this point is pushed is what takes it off the
             * polling schedule. A node the server refuses to monitor simply
             * stays polled. */
            pt->subscribed = true;
            armed++;
            if (tdot_ua_revised(mreq.requestedParameters.samplingInterval,
                                mres.revisedSamplingInterval))
                fprintf(stderr,
                        "info  device %s: point %s: the server revised the point's "
                        "sampling interval: requested %.0fms, revised %.0fms\n",
                        dev->name, pt->id, mreq.requestedParameters.samplingInterval,
                        mres.revisedSamplingInterval);
        }
        UA_MonitoredItemCreateResult_clear(&mres); /* filterResult may be heap */
    }
    if (armed == 0) {
        /* The subscription exists but carries nothing: tear it down rather than
         * leaving an idle one on the server. */
        UA_Client_Subscriptions_deleteSingle(ua->client, ua->sub_id);
        ua->sub_id = 0;
        ua->subscribed = false;
        /* deleteSingle fires on_subscription_deleted, which sets sub_lost. Clearing it here
         * keeps "we tore this down deliberately" from looking like "it died on us": today the
         * runtime never drains a device with no subscribed points, but a future change that
         * did would see a permanent -1 and reconnect in a 1s loop against a server that simply
         * refuses monitored items. */
        ua->sub_lost = false;
        snprintf(err, errlen,
                 "server accepted no monitored item for %zu requested point(s)",
                 wanted);
        return -1;
    }
    return 0;
}

static int drain_subscriptions(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_sample_sink_t sink, void *sink_ctx) {
    (void)self;
    ua_device_t *ua = dev->proto;
    if (!ua || !ua->client)
        return 0;
    if (ua->sub_lost)
        return -1; /* reported below; drop the link so we re-subscribe */
    if (!ua->subscribed)
        return 0;

    /* Non-blocking: services whatever publish responses have arrived, firing
     * on_data_change() for each notification, and keeps publish requests in
     * flight. It can also fire the subscription callbacks above, so re-check
     * sub_lost afterwards. */
    UA_StatusCode rc = UA_Client_run_iterate(ua->client, 0);
    if (rc != UA_STATUSCODE_GOOD)
        return -1; /* session gone: the runtime reconnects and re-subscribes */
    if (ua->sub_lost) {
        fprintf(stderr,
                "warn  device %s: OPC UA subscription lost; reconnecting\n",
                dev->name);
        return -1;
    }

    /* The session can also go down without run_iterate saying so. Points
     * delivered by push are off the polling schedule, so nothing else would
     * notice. */
    UA_SecureChannelState channel_state;
    UA_SessionState session_state;
    UA_StatusCode connect_status;
    UA_Client_getState(ua->client, &channel_state, &session_state,
                       &connect_status);
    if (session_state != UA_SESSIONSTATE_ACTIVATED ||
        connect_status != UA_STATUSCODE_GOOD)
        return -1;

    while (ua->tail != ua->head) {
        ua_pending_t *slot = &ua->queue[ua->tail];
        sink(sink_ctx, dev, slot->pt, &slot->sample);
        ua->tail = (ua->tail + 1) % UA_PUSH_QUEUE_LEN;
    }
    if (ua->dropped) {
        fprintf(stderr,
                "warn  device %s: dropped %lu pushed sample(s); the queue of "
                "%d filled between two runtime ticks\n",
                dev->name, ua->dropped, UA_PUSH_QUEUE_LEN);
        ua->dropped = 0;
    }
    return 0;
}

static void destroy(tdot_connector_t *self) {
    if (self->state)
        wipe_secrets(self->state);
    free(self->state);
    free(self);
}

tdot_connector_t *tdot_connector_opcua_new(void) {
    tdot_connector_t *c = calloc(1, sizeof *c);
    c->protocol = "opcua";
    c->capabilities_json = CAPABILITIES;
    c->local_only_settings = LOCAL_ONLY_SETTINGS;
    c->state = calloc(1, sizeof(ua_state_t));
    c->configure = configure;
    c->connect_device = connect_device;
    c->read_point = read_point;
    c->write_point = write_point;
    c->subscribe_device = subscribe_device;
    c->drain_subscriptions = drain_subscriptions;
    c->disconnect_device = disconnect_device;
    c->device_info = device_info;
    c->destroy = destroy;
    return c;
}
