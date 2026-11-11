/* tedge-dot — OPC UA connector on open62541 (MPL-2.0).
 * Mirrors impl/rust/crates/connector-opcua: client sessions per device, node-id
 * addressed points ("ns=2;s=Temperature"), typed reads/writes, quality "bad"
 * on Bad status codes, and monitored-item push delivery.
 */
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_highlevel_async.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/certificategroup_default.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
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
#include "push_ring.h"
#include "ua_pki.h"
#include "ua_blocks.h"
#include "ua_sampling.h"
#include "structure.h"

/* Per-point parsed address (pt->proto, flat). */
typedef struct {
    char node_id[160]; /* textual "ns=2;s=Temperature" */
    /* The effective monitored-item queue size: the point's, the device's,
     * [connection]'s, then UA_QUEUE_SIZE_DEFAULT. */
    UA_UInt32 queue_size;
    /* The namespace table the id was written against (openspec change
     * opcua-namespace-table): the [library] table's `namespaces` for an
     * address a library supplied, else the device's protocol_address one.
     * Borrowed from the config. NULL for a point read as configured: no table,
     * or a namespace-0 id. */
    toml_array_t *table;
    int ns; /* the configured namespace index */
    /* This session's translation (design D3): `resolved` is the node id to
     * use when `translated`, else `reason` says why there is none. Cleared on
     * disconnect. */
    bool translated;
    char resolved[160];
    char reason[300];
    /* A subscribed point waiting for its namespace: no monitored item yet,
     * reported through the push queue and retried (design D5). */
    bool pending;
    /* `access = "call_method"` (openspec change opcua-method-calls): node_id
     * is then the method id (so it is translated, and echoed in `addr`, like
     * a variable's) and object_id the object it is called on. The object id
     * has its own namespace index and translation; `table` is set when either
     * id needs one. Never read or subscribed. */
    bool is_method;
    char object_id[160];
    int object_ns;
    char object_resolved[160];
    /* Structured values (openspec change opcua-structures-c): `address.field`
     * as written ("" for none; parsed again per session, since the runtime
     * frees this struct with a plain free()) and `address.index` (-1: none). */
    char field[256];
    int64_t index;
    /* This session's resolution, valid when gen == ua_device_t.types_gen:
     * a field point's plan and its structure's expected encoding (null: not
     * named), or why there is none; a raw point's DataType (for the
     * addr.data_type echo; null when unknown). */
    unsigned gen;
    bool has_plan;
    uas_plan_t plan;
    UA_NodeId encoding;
    UA_NodeId data_type;
    char unresolved[TDOT_ERR_MAX];
    /* A raw structure sample's addr, with encoding_id and data_type (C7). */
    char addr_dyn[768];
    /* `address.inputs`: the input argument datatypes, in order. */
    size_t ninputs;
    tdot_datatype_t inputs[];
} ua_point_t;

/* A monitored item's queue size when no level sets one (spec §3.8). */
#define UA_QUEUE_SIZE_DEFAULT 16

/* The most monitored items one CreateMonitoredItems request carries: what the
 * Rust module sends too (async-opcua refuses replies of more than 1000, and
 * servers often cap MaxMonitoredItemsPerCall at 1000). */
#define UA_ITEMS_PER_REQUEST 500

/* The most nodes one polling Read carries, unless the server's MaxNodesPerRead
 * is lower: the Rust module's NODES_PER_READ. */
#define UA_NODES_PER_READ 500

/* The URI of namespace 0, which is never translated. */
#define UA_NS0_URI "http://opcfoundation.org/UA/"
/* How often untranslated subscribed points are tried again (seconds). A
 * polled point is tried again at its next poll. Mirrors RETRY_INTERVAL. */
#define UA_RETRY_INTERVAL_S 30.0

/* Samples that arrived by subscription since the last drain.
 *
 * open62541 delivers data changes through a callback fired from inside
 * UA_Client_run_iterate(), which drain_subscriptions() calls on the runtime
 * thread -- so producer and consumer are the same thread and the queue needs no
 * locking. It is a ring rather than a single slot per point so that a burst of
 * changes between two ticks is delivered as the separate value changes it was,
 * not collapsed into the latest one. Sized per device when it subscribes and
 * grown on demand (push_ring.h). */

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

/* One resolved (or failed) structure type of a session, parallel to
 * ua_device_t.types.structs. */
typedef struct {
    UA_NodeId id;       /* the DataType, in the client's namespace indexes */
    UA_NodeId encoding; /* its default binary encoding; null when not named */
    char *err;          /* why it could not be resolved; NULL when it was */
} ua_tslot_t;

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
    ua_ring_t queue; /* pushed values not yet handed to the runtime */

    /* Namespace tables (openspec change opcua-namespace-table). */
    double last_retry; /* monotonic s of the last retry for polled points */
    double retry_at;   /* monotonic s of the next retry for pending points */
    char **logged;     /* URIs logged as moved in this session */
    size_t nlogged;
    /* The server lists a namespace this session's client cannot address yet:
     * open62541 maps namespace indexes between client and server with a table
     * it builds only when a session is created, so a namespace the server adds
     * later needs a new session. Set by translate_device(); read_point() and
     * drain_subscriptions() then report the link down, so the runtime
     * reconnects. */
    bool renew_session;
    /* The most nodes one polling Read of this session carries:
     * UA_NODES_PER_READ, or the server's MaxNodesPerRead when lower. */
    size_t nodes_per_read;

    /* Structured values (openspec change opcua-structures-c, design C2, C3),
     * all of one session: bumped on every connect, so a point resolved in an
     * earlier session is resolved again. */
    unsigned types_gen;
    uas_types_t types; /* the structures resolved so far */
    ua_tslot_t *tslots; /* per structure: its DataType, encoding, failure */
    /* The server's NamespaceArray (structure bodies) and the client's
     * namespace table (NodeIds open62541 decoded), for the nsu= forms. */
    bool ns_read;
    char **srv_ns;
    size_t nsrv;
    char **cli_ns;
    size_t ncli;
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

    UA_UInt32 queue_size; /* [connection] queue_size; 0: unset */
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
    "\"command_verbs\":[\"write\",\"write-batch\",\"call-method\"],"
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

/* `queue_size` as written: an integer from 1 to 65535, with the message the
 * Rust module gives. 0 when the key is absent. */
static int get_queue_size(toml_table_t *tab, UA_UInt32 *out, const char *where,
                          char *err, size_t errlen) {
    *out = 0;
    if (!tab || !toml_key_exists(tab, "queue_size"))
        return 0;
    toml_datum_t d = toml_int_in(tab, "queue_size");
    if (!d.ok || d.u.i < 1 || d.u.i > 65535) {
        snprintf(err, errlen,
                 "%squeue_size must be an integer from 1 to 65535", where);
        return -1;
    }
    *out = (UA_UInt32)d.u.i;
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

/* ---- namespace tables (openspec change opcua-namespace-table) ----------
 * A configured id like ns=3;i=1234 names its namespace by position in the
 * server's NamespaceArray, which a server may number differently after a
 * restart (OPC 10000-5 §6.3.1). A table records the array the ids were written
 * against; each session translates index -> URI -> the server's index now.
 * Mirrors impl/rust/crates/connector-opcua/src/namespaces.rs. */

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Validate the `namespaces` key of `holder` (absent: *out = NULL). The
 * messages are the Rust module's (namespaces::parse_table). */
static int parse_table(toml_table_t *holder, toml_array_t **out, char *why,
                       size_t whylen) {
    *out = NULL;
    if (!holder || (!toml_raw_in(holder, "namespaces") &&
                    !toml_array_in(holder, "namespaces") &&
                    !toml_table_in(holder, "namespaces")))
        return 0;
    toml_array_t *arr = toml_array_in(holder, "namespaces");
    int n = arr ? toml_array_nelem(arr) : 0;
    for (int i = 0; arr && i < n; i++) {
        toml_datum_t d = toml_string_at(arr, i);
        if (!d.ok) {
            arr = NULL;
            break;
        }
        free(d.u.s);
    }
    if (!arr) {
        snprintf(why, whylen, "namespaces must be an array of namespace URI strings");
        return -1;
    }
    if (n == 0) {
        snprintf(why, whylen, "namespaces entry 0 must be '" UA_NS0_URI "', found none");
        return -1;
    }
    toml_datum_t first = toml_string_at(arr, 0);
    int bad = strcmp(first.u.s, UA_NS0_URI) != 0;
    if (bad)
        snprintf(why, whylen, "namespaces entry 0 must be '" UA_NS0_URI "', found '%s'",
                 first.u.s);
    free(first.u.s);
    if (bad)
        return -1;
    *out = arr;
    return 0;
}

/* The node id this session reads for a point, or NULL with *reason. */
static char *point_node(ua_point_t *up, const char **reason) {
    if (!up->table)
        return up->node_id;
    if (up->translated)
        return up->resolved;
    *reason = up->reason[0] ? up->reason : "namespace not translated";
    return NULL;
}

/* Forget the session's translations (on disconnect). */
static void reset_translation(tdot_device_t *dev, ua_device_t *ua) {
    for (size_t j = 0; j < dev->npoints; j++) {
        ua_point_t *up = dev->points[j].proto;
        if (!up)
            continue;
        up->translated = false;
        up->resolved[0] = up->reason[0] = '\0';
        up->pending = false;
    }
    for (size_t i = 0; i < ua->nlogged; i++)
        free(ua->logged[i]);
    free(ua->logged);
    ua->logged = NULL;
    ua->nlogged = 0;
    ua->last_retry = ua->retry_at = 0;
    ua->renew_session = false;
}

/* Log a namespace the server lists at another index, once per session. */
static void log_moved(const tdot_device_t *dev, ua_device_t *ua, const char *uri,
                      int local, size_t server) {
    for (size_t i = 0; i < ua->nlogged; i++)
        if (!strcmp(ua->logged[i], uri))
            return;
    char **grown = realloc(ua->logged, (ua->nlogged + 1) * sizeof *grown);
    if (!grown)
        return;
    ua->logged = grown;
    ua->logged[ua->nlogged++] = strdup(uri);
    fprintf(stderr, "info  %s: namespace %s local %d -> server %zu\n", dev->name, uri,
            local, server);
}

/* Translate one configured node id of a point through its table (design
 * D3): `ns` is the id's configured namespace index. Namespace 0 is used as
 * configured. Fills `resolved` and returns true, or fills `reason`. */
static bool translate_node(const tdot_device_t *dev, ua_device_t *ua, toml_array_t *table,
                           int ns, const char *node_id, const UA_String *uris, size_t nuris,
                           char *resolved, size_t rlen, char *reason, size_t reasonlen) {
    if (ns == 0) {
        snprintf(resolved, rlen, "%s", node_id);
        return true;
    }
    bool ok = false;
    /* configure checked that the table lists the index */
    toml_datum_t uri = toml_string_at(table, ns);
    size_t found = nuris;
    for (size_t k = 0; uri.ok && k < nuris && found == nuris; k++)
        if (uris[k].length == strlen(uri.u.s) &&
            !memcmp(uris[k].data, uri.u.s, uris[k].length))
            found = k;
    /* The index to put in the node id is the CLIENT's for the URI:
     * open62541 rewrites every encoded node id from its own index to
     * the server's (client.h, "Namespace Mapping"). For namespaces the
     * server listed when the session was created the two agree. */
    UA_UInt16 local = 0;
    UA_NodeId node;
    if (!uri.ok) {
        snprintf(reason, reasonlen, "namespace not translated");
    } else if (found == nuris) {
        snprintf(reason, reasonlen, "namespace not found: %s", uri.u.s);
    } else if (UA_Client_getNamespaceIndex(ua->client, UA_STRING(uri.u.s), &local) !=
               UA_STATUSCODE_GOOD) {
        snprintf(reason, reasonlen,
                 "namespace %s was added after connecting; renewing the session",
                 uri.u.s);
        if (!ua->renew_session)
            fprintf(stderr,
                    "info  device %s: the server added namespace %s; renewing the "
                    "session to address it\n",
                    dev->name, uri.u.s);
        ua->renew_session = true;
    } else if (UA_NodeId_parse(&node, UA_STRING((char *)node_id)) == UA_STATUSCODE_GOOD) {
        node.namespaceIndex = local;
        UA_String text = UA_STRING_NULL;
        UA_NodeId_print(&node, &text);
        snprintf(resolved, rlen, "%.*s", (int)text.length, (const char *)text.data);
        UA_String_clear(&text);
        UA_NodeId_clear(&node);
        ok = true;
        if ((int)found != ns)
            log_moved(dev, ua, uri.u.s, ns, found);
    } else {
        snprintf(reason, reasonlen, "invalid node id: %s", node_id);
    }
    if (uri.ok)
        free(uri.u.s);
    return ok;
}

/* Translate the device's points that have a table: all of them, or with
 * `retry` only those not translated yet (design D3, D5). */
static void translate_device(tdot_device_t *dev, bool retry) {
    ua_device_t *ua = dev->proto;
    bool any = false;
    for (size_t j = 0; j < dev->npoints && !any; j++) {
        const ua_point_t *up = dev->points[j].proto;
        any = up && up->table && (!retry || !up->translated);
    }
    if (!any || !ua->client)
        return;

    UA_Variant array;
    UA_Variant_init(&array);
    UA_StatusCode rc = UA_Client_readValueAttribute(
        ua->client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY), &array);
    char unavailable[200] = "";
    if (rc != UA_STATUSCODE_GOOD)
        snprintf(unavailable, sizeof unavailable, "namespace array unavailable: %s",
                 UA_StatusCode_name(rc));
    else if (UA_Variant_isScalar(&array) || array.type != &UA_TYPES[UA_TYPES_STRING])
        snprintf(unavailable, sizeof unavailable,
                 "namespace array unavailable: not an array of strings");
    const UA_String *uris = unavailable[0] ? NULL : array.data;
    size_t nuris = unavailable[0] ? 0 : array.arrayLength;

    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        ua_point_t *up = pt->proto;
        if (!up || !up->table || (retry && up->translated))
            continue;
        up->translated = false;
        if (unavailable[0]) {
            snprintf(up->reason, sizeof up->reason, "%s", unavailable);
        } else if (translate_node(dev, ua, up->table, up->ns, up->node_id, uris, nuris,
                                  up->resolved, sizeof up->resolved, up->reason,
                                  sizeof up->reason) &&
                   (!up->is_method ||
                    translate_node(dev, ua, up->table, up->object_ns, up->object_id, uris,
                                   nuris, up->object_resolved, sizeof up->object_resolved,
                                   up->reason, sizeof up->reason))) {
            up->translated = true;
            up->reason[0] = '\0';
        }
        if (!retry && !up->translated)
            fprintf(stderr, "warn  device '%s' point '%s': %s\n", dev->name, pt->id,
                    up->reason);
    }
    UA_Variant_clear(&array);
}

/* ---- method points (openspec change opcua-method-calls) ----------------- */

/* Address keys of a method point, refused on a variable. */
static const char *const METHOD_KEYS[] = {"object_id", "method_id", "inputs"};
/* Address keys of a variable, refused on a method point. */
static const char *const VARIABLE_KEYS[] = {"node_id", "namespace", "identifier",
                                            "field",   "index",     "queue_size"};

static bool address_has(toml_table_t *address, const char *key) {
    return address && (toml_raw_in(address, key) || toml_array_in(address, key) ||
                       toml_table_in(address, key));
}

/* `key` of a method address: a required textual node id, copied to `out`,
 * with its namespace index in *ns. */
static int method_node(const tdot_point_t *pt, const char *key, char *out, size_t outlen,
                       int *ns, char *err, size_t errlen) {
    toml_datum_t d = toml_string_in(pt->address, key);
    if (!d.ok) {
        snprintf(err, errlen, "point '%s' address: %s is required for a call_method point",
                 pt->id, key);
        return -1;
    }
    snprintf(out, outlen, "%s", d.u.s);
    UA_NodeId node;
    if (strlen(d.u.s) >= outlen || UA_NodeId_parse(&node, UA_STRING(d.u.s)) != UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "point '%s' address: invalid %s '%s'", pt->id, key, d.u.s);
        free(d.u.s);
        return -1;
    }
    *ns = node.namespaceIndex;
    UA_NodeId_clear(&node);
    free(d.u.s);
    return 0;
}

/* Parse a call_method point's address: object_id, method_id and inputs. */
static int configure_method(tdot_point_t *pt, toml_array_t *table, char *err, size_t errlen) {
    for (size_t k = 0; k < sizeof VARIABLE_KEYS / sizeof VARIABLE_KEYS[0]; k++)
        if (address_has(pt->address, VARIABLE_KEYS[k])) {
            snprintf(err, errlen, "point '%s' address: %s does not apply to a call_method point",
                     pt->id, VARIABLE_KEYS[k]);
            return -1;
        }
    toml_array_t *inputs = toml_array_in(pt->address, "inputs");
    if (address_has(pt->address, "inputs") &&
        (!inputs || (toml_array_nelem(inputs) > 0 && toml_array_kind(inputs) != 'v'))) {
        snprintf(err, errlen, "point '%s' address: inputs must be an array of datatypes",
                 pt->id);
        return -1;
    }
    size_t n = inputs ? (size_t)toml_array_nelem(inputs) : 0;
    ua_point_t *up = calloc(1, sizeof *up + n * sizeof up->inputs[0]);
    if (up)
        up->index = -1;
    pt->proto = up;
    up->is_method = true;
    for (size_t k = 0; k < n; k++) {
        toml_datum_t d = toml_string_at(inputs, (int)k);
        if (!d.ok) {
            snprintf(err, errlen, "point '%s' address: inputs must be an array of datatypes",
                     pt->id);
            return -1;
        }
        tdot_datatype_t dt = tdot_datatype_parse(d.u.s);
        if (dt == TDOT_DT_NONE)
            snprintf(err, errlen, "point '%s' address: inputs[%zu]: unknown datatype '%s'",
                     pt->id, k, d.u.s);
        else if (dt == TDOT_DT_BYTES)
            snprintf(err, errlen,
                     "point '%s' address: inputs[%zu]: bytes is not supported as a method "
                     "argument",
                     pt->id, k);
        free(d.u.s);
        if (dt == TDOT_DT_NONE || dt == TDOT_DT_BYTES)
            return -1;
        up->inputs[up->ninputs++] = dt;
    }
    if (method_node(pt, "object_id", up->object_id, sizeof up->object_id, &up->object_ns,
                    err, errlen) != 0 ||
        method_node(pt, "method_id", up->node_id, sizeof up->node_id, &up->ns, err,
                    errlen) != 0)
        return -1;
    if (table && (up->ns != 0 || up->object_ns != 0)) {
        /* object_id first, then method_id, as the ids are written. */
        int entries = toml_array_nelem(table);
        int bad = up->object_ns >= entries ? up->object_ns : up->ns >= entries ? up->ns : -1;
        if (bad >= 0) {
            snprintf(err, errlen,
                     "point '%s': namespace index %d is not in the namespace table (%d "
                     "entries)",
                     pt->id, bad, entries);
            return -1;
        }
        up->table = table;
    }
    cJSON *addr = cJSON_CreateObject();
    cJSON_AddStringToObject(addr, "object_id", up->object_id);
    cJSON_AddStringToObject(addr, "method_id", up->node_id);
    pt->addr_json = cJSON_PrintUnformatted(addr);
    cJSON_Delete(addr);
    return 0;
}

/* `address.field` and `address.index` of a variable point (openspec changes
 * opcua-custom-datatypes and opcua-structures-c), checked as the Rust module
 * checks them. */
static int configure_structure(tdot_point_t *pt, ua_point_t *up, char *err, size_t errlen) {
    if (address_has(pt->address, "field")) {
        toml_datum_t f = toml_string_in(pt->address, "field");
        if (!f.ok) {
            snprintf(err, errlen, "point '%s' address: field must be a string", pt->id);
            return -1;
        }
        char why[300];
        uas_path_t path;
        int rc = uas_parse_path(f.u.s, &path, why, sizeof why);
        if (rc == 0) {
            /* echoed as parsed, as the Rust module echoes it */
            uas_path_text(&path, up->field, sizeof up->field);
            uas_path_free(&path);
            if (strlen(f.u.s) >= sizeof up->field) {
                snprintf(why, sizeof why, "field path is longer than %zu bytes",
                         sizeof up->field - 1);
                rc = -1;
            }
        }
        free(f.u.s);
        if (rc != 0) {
            snprintf(err, errlen, "point '%s' address: %s", pt->id, why);
            return -1;
        }
    }
    if (address_has(pt->address, "index")) {
        toml_datum_t i = toml_int_in(pt->address, "index");
        if (!i.ok || i.u.i < 0 || i.u.i > 0xFFFFFFFFLL) {
            snprintf(err, errlen,
                     "point '%s' address: index must be a non-negative integer", pt->id);
            return -1;
        }
        up->index = (int64_t)i.u.i;
    }
    if (up->field[0] && pt->mode == TDOT_MODE_RAW) {
        snprintf(err, errlen,
                 "point '%s': address.field selects a typed value; a raw point reads the "
                 "whole structure body",
                 pt->id);
        return -1;
    }
    if ((up->field[0] || up->index >= 0) && (pt->access & TDOT_ACCESS_WRITE)) {
        snprintf(err, errlen,
                 "point '%s': points with address.field or address.index are read-only",
                 pt->id);
        return -1;
    }
    return 0;
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
    if (get_queue_size(c, &st->queue_size, "[connection]: ", err, errlen) < 0)
        return -1;
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

        char where[256];
        snprintf(where, sizeof where, "device '%s' protocol_address: ", dev->name);
        UA_UInt32 device_queue_size;
        if (get_queue_size(dev->protocol_address, &device_queue_size, where,
                           err, errlen) < 0)
            return -1;

        if (configure_security(st, &def, dev, ua, base, err, errlen) != 0)
            return -1;
        char why[400];
        toml_array_t *device_table = NULL;
        if (parse_table(dev->protocol_address, &device_table, why, sizeof why) != 0) {
            snprintf(err, errlen, "device '%s' protocol_address: %s", dev->name, why);
            return -1;
        }
        if (ua->policy != &POLICIES[0])
            any_secure = true;

        for (size_t j = 0; j < dev->npoints; j++) {
            tdot_point_t *pt = &dev->points[j];
            /* A library's table describes the library's addresses; the
             * device's describes the rest. */
            toml_array_t *table = device_table;
            if (pt->library && (toml_raw_in(pt->library, "namespaces") ||
                                toml_array_in(pt->library, "namespaces") ||
                                toml_table_in(pt->library, "namespaces"))) {
                if (parse_table(pt->library, &table, why, sizeof why) != 0) {
                    snprintf(err, errlen, "point '%s': [library] %s", pt->id, why);
                    return -1;
                }
            }
            if (pt->access & TDOT_ACCESS_CALL) {
                if (configure_method(pt, table, err, errlen) != 0)
                    return -1;
                continue;
            }
            for (size_t k = 0; k < sizeof METHOD_KEYS / sizeof METHOD_KEYS[0]; k++)
                if (address_has(pt->address, METHOD_KEYS[k])) {
                    snprintf(err, errlen,
                             "point '%s' address: %s applies only to a call_method point",
                             pt->id, METHOD_KEYS[k]);
                    return -1;
                }
            /* Two address forms, like the Rust module: `node_id = "ns=2;s=X"`
             * or structured `namespace = 2, identifier = "X" | 1001`. */
            ua_point_t *up = calloc(1, sizeof *up);
            pt->proto = up;
            up->index = -1;
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

            snprintf(where, sizeof where, "point '%s' address: ", pt->id);
            if (get_queue_size(pt->address, &up->queue_size, where, err,
                               errlen) < 0)
                return -1;
            if (!up->queue_size)
                up->queue_size = device_queue_size  ? device_queue_size
                                 : st->queue_size   ? st->queue_size
                                                    : UA_QUEUE_SIZE_DEFAULT;
            UA_NodeId parsed;
            if (table && UA_NodeId_parse(&parsed, UA_STRING(up->node_id)) == UA_STATUSCODE_GOOD) {
                up->ns = parsed.namespaceIndex;
                UA_NodeId_clear(&parsed);
                if (up->ns != 0) {
                    int entries = toml_array_nelem(table);
                    if (up->ns >= entries) {
                        snprintf(err, errlen,
                                 "point '%s': namespace index %d is not in the namespace "
                                 "table (%d entries)",
                                 pt->id, up->ns, entries);
                        return -1;
                    }
                    up->table = table;
                }
            }

            if (configure_structure(pt, up, err, errlen) != 0)
                return -1;
            cJSON *addr = cJSON_CreateObject();
            cJSON_AddStringToObject(addr, "node_id", up->node_id);
            if (up->field[0])
                cJSON_AddStringToObject(addr, "field", up->field);
            if (up->index >= 0)
                cJSON_AddNumberToObject(addr, "index", (double)up->index);
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

static void reset_structures(tdot_device_t *dev, ua_device_t *ua);
static void read_client_namespaces(ua_device_t *ua);

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
         * already cleared pt->subscribed for every point. The ring is freed:
         * the runtime releases ua_device_t with a plain free(), always after
         * a disconnect, and the next subscribe sizes a new one. */
        ua->sub_id = 0;
        ua->subscribed = false;
        ua->sub_lost = false;
        ua_ring_free(&ua->queue);
        reset_translation(dev, ua);
        reset_structures(dev, ua);
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

/* The block size of a session's polling reads: UA_NODES_PER_READ, or the
 * server's OperationLimits.MaxNodesPerRead when that is lower. A limit of 0,
 * or none published, means the server sets none. */
static size_t nodes_per_read(UA_Client *client) {
    UA_Variant v;
    UA_Variant_init(&v);
    size_t block = UA_NODES_PER_READ;
    if (UA_Client_readValueAttribute(
            client,
            UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_SERVERCAPABILITIES_OPERATIONLIMITS_MAXNODESPERREAD),
            &v) == UA_STATUSCODE_GOOD &&
        UA_Variant_hasScalarType(&v, &UA_TYPES[UA_TYPES_UINT32])) {
        UA_UInt32 limit = *(UA_UInt32 *)v.data;
        if (limit > 0 && limit < block)
            block = limit;
    }
    UA_Variant_clear(&v);
    return block;
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
    ua->nodes_per_read = nodes_per_read(ua->client);
    if (ua->nodes_per_read < UA_NODES_PER_READ)
        fprintf(stderr,
                "info  device %s: the server reads at most %zu node(s) per "
                "request; polls go out in blocks of that size\n",
                dev->name, ua->nodes_per_read);
    translate_device(dev, false);
    /* A new session: structures are resolved again (design C2). */
    if (++ua->types_gen == 0)
        ua->types_gen = 1;
    read_client_namespaces(ua);
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

/* ---- structured values (openspec change opcua-structures-c) ------------------
 * The port of the Rust module's resolve.rs and decode_value: what a session
 * learns from the server before a structure can be decoded (design C2), and
 * how a value is decoded as its point selects it (C3, C4). The walker itself
 * is structure.c. */

/* Prefix of every reason a definition could not be resolved. */
#define UA_UNAVAILABLE "data type definition unavailable"
/* Supertype hops followed to find the built-in type behind a simple subtype. */
#define UA_MAX_SUPERTYPES 8
/* ns0 ids this section needs. */
#define UA_NS0_ENUMERATION 29
#define UA_NS0_HASENCODING 38
#define UA_NS0_HASSUBTYPE 45

/* Whether a point needs this section: a field point, or a raw point (whose
 * structure value carries its DataType). */
static bool needs_types(const tdot_point_t *pt) {
    const ua_point_t *up = pt->proto;
    return up && !up->is_method && (up->field[0] || pt->mode == TDOT_MODE_RAW);
}

static void node_str(const UA_NodeId *id, char *out, size_t len) {
    UA_String s = UA_STRING_NULL;
    if (UA_NodeId_print(id, &s) == UA_STATUSCODE_GOOD)
        snprintf(out, len, "%.*s", (int)s.length, (const char *)s.data);
    else
        snprintf(out, len, "?");
    UA_String_clear(&s);
}

/* A NodeId open62541 decoded (client namespace indexes) in the samples'
 * nsu= form (C3). */
static void node_nsu(const ua_device_t *ua, const UA_NodeId *id, char *out, size_t len) {
    UA_NodeId bare = *id;
    bare.namespaceIndex = 0;
    char ident[512];
    node_str(&bare, ident, sizeof ident);
    uas_node_text(id->namespaceIndex, ident, (const char *const *)ua->cli_ns, ua->ncli, out,
                  len);
}

/* The built-in type a namespace-0 DataType is encoded as, for the built-in
 * types themselves and their common simple subtypes (resolve.rs builtin_of). */
static int builtin_of(const UA_NodeId *dt) {
    if (dt->namespaceIndex != 0 || dt->identifierType != UA_NODEIDTYPE_NUMERIC)
        return 0;
    UA_UInt32 id = dt->identifier.numeric;
    if ((id >= 1 && id <= 21) || id == 23 || id == 25)
        return (int)id;
    switch (id) {
    case 22: return UAS_EXTENSIONOBJECT; /* Structure: abstract */
    case 24: case 26: case 27: case 28: return UAS_VARIANT; /* BaseDataType, Number, ... */
    case 288: case 289: case 17588: case 20998: return UAS_UINT32;
    case 290: return UAS_DOUBLE;
    case 293: case 294: return UAS_DATETIME;
    case 291: case 292: case 295: case 12877: case 12878: case 12879: case 12880:
    case 12881: case 23751: case 24263: return UAS_STRING;
    case 30: case 311: case 521: case 2000: case 2001: case 2002: case 2003:
    case 16307: return UAS_BYTESTRING;
    case 388: return UAS_NODEID;
    case 11737: return UAS_UINT64;
    default: return 0;
    }
}

static void free_strings(char **v, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

/* Forget the session's structures (on disconnect). */
static void reset_structures(tdot_device_t *dev, ua_device_t *ua) {
    for (size_t j = 0; j < dev->npoints; j++) {
        ua_point_t *up = dev->points[j].proto;
        if (!up)
            continue;
        uas_plan_free(&up->plan);
        up->has_plan = false;
        UA_NodeId_clear(&up->encoding);
        UA_NodeId_clear(&up->data_type);
        up->unresolved[0] = '\0';
        up->gen = 0;
    }
    for (size_t i = 0; i < ua->types.n; i++) {
        UA_NodeId_clear(&ua->tslots[i].id);
        UA_NodeId_clear(&ua->tslots[i].encoding);
        free(ua->tslots[i].err);
    }
    free(ua->tslots);
    ua->tslots = NULL;
    uas_types_free(&ua->types);
    free_strings(ua->srv_ns, ua->nsrv);
    free_strings(ua->cli_ns, ua->ncli);
    ua->srv_ns = ua->cli_ns = NULL;
    ua->nsrv = ua->ncli = 0;
    ua->ns_read = false;
}

/* The client's namespace table (C3): local to open62541, no request. Filled
 * on connect, when the client has added the server's namespaces to it. */
static void read_client_namespaces(ua_device_t *ua) {
    free_strings(ua->cli_ns, ua->ncli);
    ua->cli_ns = NULL;
    ua->ncli = 0;
    for (UA_UInt16 i = 0; i < 1024; i++) {
        UA_String uri = UA_STRING_NULL;
        if (UA_Client_getNamespaceUri(ua->client, i, &uri) != UA_STATUSCODE_GOOD)
            break;
        char **grown = realloc(ua->cli_ns, (ua->ncli + 1) * sizeof *grown);
        char *text = malloc(uri.length + 1);
        if (!grown || !text) {
            free(text);
            if (grown)
                ua->cli_ns = grown;
            UA_String_clear(&uri);
            break;
        }
        memcpy(text, uri.data, uri.length);
        text[uri.length] = '\0';
        ua->cli_ns = grown;
        ua->cli_ns[ua->ncli++] = text;
        UA_String_clear(&uri);
    }
}

/* The server's NamespaceArray, for NodeIds inside structure bodies; once per
 * session. A failure leaves it empty: those NodeIds render as ns=<n>;. */
static void read_namespaces(ua_device_t *ua) {
    if (ua->ns_read)
        return;
    ua->ns_read = true;
    UA_Variant array;
    UA_Variant_init(&array);
    if (UA_Client_readValueAttribute(ua->client,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY),
                                     &array) == UA_STATUSCODE_GOOD &&
        !UA_Variant_isScalar(&array) && array.type == &UA_TYPES[UA_TYPES_STRING]) {
        const UA_String *uris = array.data;
        ua->srv_ns = calloc(array.arrayLength ? array.arrayLength : 1, sizeof *ua->srv_ns);
        for (size_t i = 0; ua->srv_ns && i < array.arrayLength; i++) {
            ua->srv_ns[i] = malloc(uris[i].length + 1);
            if (ua->srv_ns[i]) {
                memcpy(ua->srv_ns[i], uris[i].data, uris[i].length);
                ua->srv_ns[i][uris[i].length] = '\0';
            }
        }
        if (ua->srv_ns)
            ua->nsrv = array.arrayLength;
    }
    UA_Variant_clear(&array);
}

/* One Read; a failed service or a short answer is a "read failed" reason. */
static int read_nodes(ua_device_t *ua, UA_ReadValueId *ids, size_t n, UA_ReadResponse *resp,
                      char *err, size_t errlen) {
    UA_ReadRequest req;
    UA_ReadRequest_init(&req);
    req.nodesToRead = ids;
    req.nodesToReadSize = n;
    req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
    *resp = UA_Client_Service_read(ua->client, req);
    UA_StatusCode sc = resp->responseHeader.serviceResult;
    if (sc == UA_STATUSCODE_GOOD && resp->resultsSize != n)
        sc = UA_STATUSCODE_BADUNEXPECTEDERROR;
    if (sc != UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "read failed: %s", UA_StatusCode_name(sc));
        UA_ReadResponse_clear(resp);
        return -1;
    }
    return 0;
}

/* The first node an inverse reference of `reftype` leads to: 0 with *out,
 * 1 when there is none, -1 with *sc when the browse failed. */
static int browse_inverse(ua_device_t *ua, const UA_NodeId *node, UA_UInt32 reftype,
                          UA_NodeId *out, UA_StatusCode *sc) {
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = *node;
    bd.browseDirection = UA_BROWSEDIRECTION_INVERSE;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, reftype);
    bd.includeSubtypes = false;
    bd.resultMask = UA_BROWSERESULTMASK_ALL;
    UA_BrowseRequest req;
    UA_BrowseRequest_init(&req);
    req.requestedMaxReferencesPerNode = 1;
    req.nodesToBrowse = &bd;
    req.nodesToBrowseSize = 1;
    UA_BrowseResponse resp = UA_Client_Service_browse(ua->client, req);
    *sc = resp.responseHeader.serviceResult;
    if (*sc == UA_STATUSCODE_GOOD && resp.resultsSize == 1)
        *sc = resp.results[0].statusCode;
    int rc = -1;
    if (*sc == UA_STATUSCODE_GOOD) {
        rc = 1;
        if (resp.resultsSize == 1 && resp.results[0].referencesSize > 0) {
            UA_NodeId_copy(&resp.results[0].references[0].nodeId.nodeId, out);
            rc = 0;
        }
    }
    UA_BrowseResponse_clear(&resp);
    return rc;
}

typedef enum { UA_DEF_NONE, UA_DEF_STRUCT, UA_DEF_ENUM } ua_def_kind_t;

/* A DataType node's browse name and definition, in one Read. With
 * UA_DEF_STRUCT, *def is a copy the caller clears. */
static int read_definition(ua_device_t *ua, const UA_NodeId *dt, char *name, size_t namelen,
                           ua_def_kind_t *kind, UA_StructureDefinition *def, char *err,
                           size_t errlen) {
    UA_ReadValueId ids[2];
    UA_ReadValueId_init(&ids[0]);
    UA_ReadValueId_init(&ids[1]);
    ids[0].nodeId = ids[1].nodeId = *dt; /* borrowed: not cleared */
    ids[0].attributeId = UA_ATTRIBUTEID_BROWSENAME;
    ids[1].attributeId = UA_ATTRIBUTEID_DATATYPEDEFINITION;
    UA_ReadResponse resp;
    if (read_nodes(ua, ids, 2, &resp, err, errlen) != 0)
        return -1;
    const UA_Variant *bn = &resp.results[0].value;
    if (resp.results[0].hasValue && UA_Variant_hasScalarType(bn, &UA_TYPES[UA_TYPES_QUALIFIEDNAME])) {
        const UA_QualifiedName *q = bn->data;
        snprintf(name, namelen, "%.*s", (int)q->name.length, (const char *)q->name.data);
    } else {
        node_str(dt, name, namelen);
    }
    *kind = UA_DEF_NONE;
    const UA_Variant *v = &resp.results[1].value;
    const UA_DataType *type = v->type;
    const void *data = v->data;
    if (type == &UA_TYPES[UA_TYPES_EXTENSIONOBJECT] && UA_Variant_isScalar(v)) {
        const UA_ExtensionObject *eo = data;
        if (eo->encoding >= UA_EXTENSIONOBJECT_DECODED) {
            type = eo->content.decoded.type;
            data = eo->content.decoded.data;
        }
    }
    if (resp.results[1].hasValue && UA_Variant_isScalar(v) && data) {
        if (type == &UA_TYPES[UA_TYPES_STRUCTUREDEFINITION]) {
            UA_StructureDefinition_copy(data, def);
            *kind = UA_DEF_STRUCT;
        } else if (type == &UA_TYPES[UA_TYPES_ENUMDEFINITION]) {
            *kind = UA_DEF_ENUM;
        }
    }
    UA_ReadResponse_clear(&resp);
    return 0;
}

static int struct_of(tdot_device_t *dev, const UA_NodeId *dt, size_t *idx, char *err,
                     size_t errlen);

/* The type of a structure field: a built-in type, an Enumeration or a
 * structure. Fills f->kind/builtin/type_name/ref. */
static int field_type(tdot_device_t *dev, const UA_NodeId *dt, uas_field_t *f, char *err,
                      size_t errlen) {
    ua_device_t *ua = dev->proto;
    if ((f->builtin = builtin_of(dt))) {
        f->kind = UAS_T_BUILTIN;
        return 0;
    }
    char name[256];
    ua_def_kind_t kind;
    UA_StructureDefinition def;
    UA_StructureDefinition_init(&def);
    if (read_definition(ua, dt, name, sizeof name, &kind, &def, err, errlen) != 0)
        return -1;
    UA_StructureDefinition_clear(&def);
    if (kind == UA_DEF_STRUCT) {
        f->kind = UAS_T_STRUCT;
        return struct_of(dev, dt, &f->ref, err, errlen);
    }
    if (kind == UA_DEF_ENUM) {
        f->kind = UAS_T_ENUM;
        f->type_name = strdup(name);
        return 0;
    }
    /* A simple subtype (an alias of a built-in type): follow its supertypes. */
    UA_NodeId current;
    UA_NodeId_copy(dt, &current);
    char text[200];
    for (int hop = 0; hop < UA_MAX_SUPERTYPES; hop++) {
        UA_NodeId super;
        UA_StatusCode sc;
        int rc = browse_inverse(ua, &current, UA_NS0_HASSUBTYPE, &super, &sc);
        node_str(&current, text, sizeof text);
        UA_NodeId_clear(&current);
        if (rc < 0) {
            snprintf(err, errlen, "browsing the supertype of %s: %s", text, UA_StatusCode_name(sc));
            return -1;
        }
        if (rc > 0) {
            snprintf(err, errlen, "%s has no supertype", text);
            return -1;
        }
        current = super;
        if (current.namespaceIndex == 0 && current.identifierType == UA_NODEIDTYPE_NUMERIC &&
            current.identifier.numeric == UA_NS0_ENUMERATION) {
            f->kind = UAS_T_ENUM;
            f->type_name = strdup(name);
            return 0;
        }
        if ((f->builtin = builtin_of(&current))) {
            UA_NodeId_clear(&current);
            f->kind = UAS_T_BUILTIN;
            return 0;
        }
    }
    UA_NodeId_clear(&current);
    node_str(dt, text, sizeof text);
    snprintf(err, errlen, "%s (%s) has no definition and no built-in supertype", name, text);
    return -1;
}

/* Fill the structure in slot i from its DataType's definition. */
static int load_struct(tdot_device_t *dev, size_t i, char *err, size_t errlen) {
    ua_device_t *ua = dev->proto;
    UA_NodeId dt;
    UA_NodeId_copy(&ua->tslots[i].id, &dt);
    char name[256], text[200];
    ua_def_kind_t kind;
    UA_StructureDefinition def;
    UA_StructureDefinition_init(&def);
    int rc = -1;
    uas_field_t *fields = NULL;
    size_t nfields = 0;
    node_str(&dt, text, sizeof text);
    if (read_definition(ua, &dt, name, sizeof name, &kind, &def, err, errlen) != 0)
        goto out;
    if (kind == UA_DEF_ENUM) {
        snprintf(err, errlen, "%s is an Enumeration, not a structure", name);
        goto out;
    }
    if (kind == UA_DEF_NONE) {
        int b = builtin_of(&dt);
        if (b)
            snprintf(err, errlen, "the value's DataType is %s, not a structure",
                     uas_builtin_name(b));
        else
            snprintf(err, errlen, "%s (%s) has no DataTypeDefinition", name, text);
        goto out;
    }
    uas_skind_t skind;
    switch (def.structureType) {
    case UA_STRUCTURETYPE_STRUCTURE: skind = UAS_STRUCTURE; break;
    case UA_STRUCTURETYPE_STRUCTUREWITHOPTIONALFIELDS: skind = UAS_OPTIONAL_FIELDS; break;
    case UA_STRUCTURETYPE_UNION: skind = UAS_UNION; break;
    case UA_STRUCTURETYPE_STRUCTUREWITHSUBTYPEDVALUES:
        snprintf(err, errlen, "%s is a StructureWithSubtypedValues, which is not supported", name);
        goto out;
    case UA_STRUCTURETYPE_UNIONWITHSUBTYPEDVALUES:
        snprintf(err, errlen, "%s is a UnionWithSubtypedValues, which is not supported", name);
        goto out;
    default:
        snprintf(err, errlen, "%s is a structure type %d, which is not supported", name,
                 (int)def.structureType);
        goto out;
    }
    if (!UA_NodeId_isNull(&def.defaultEncodingId))
        UA_NodeId_copy(&def.defaultEncodingId, &ua->tslots[i].encoding);
    free(ua->types.structs[i].name);
    ua->types.structs[i].name = strdup(name);
    ua->types.structs[i].kind = skind;
    /* Built apart and assigned at the end: field_type may add structures,
     * which moves ua->types.structs. */
    fields = calloc(def.fieldsSize ? def.fieldsSize : 1, sizeof *fields);
    if (!fields) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    for (size_t j = 0; j < def.fieldsSize; j++) {
        const UA_StructureField *sf = &def.fields[j];
        uas_field_t *f = &fields[nfields];
        f->name = malloc(sf->name.length + 1);
        if (!f->name) {
            snprintf(err, errlen, "out of memory");
            goto out;
        }
        memcpy(f->name, sf->name.data, sf->name.length);
        f->name[sf->name.length] = '\0';
        nfields++;
        if (sf->valueRank == 1) {
            f->array = true;
        } else if (sf->valueRank != -1) {
            snprintf(err, errlen,
                     "field \"%s\" of %s has ValueRank %d, which is not supported", f->name,
                     name, (int)sf->valueRank);
            goto out;
        }
        f->optional = sf->isOptional;
        char why[TDOT_ERR_MAX * 2];
        if (field_type(dev, &sf->dataType, f, why, sizeof why) != 0) {
            snprintf(err, errlen, "field \"%s\" of %s: %s", f->name, name, why);
            goto out;
        }
    }
    ua->types.structs[i].fields = fields;
    ua->types.structs[i].nfields = nfields;
    fields = NULL;
    rc = 0;
out:
    for (size_t j = 0; fields && j < nfields; j++) {
        free(fields[j].name);
        free(fields[j].type_name);
    }
    free(fields);
    UA_StructureDefinition_clear(&def);
    UA_NodeId_clear(&dt);
    return rc;
}

/* Resolve a structure type and every type it contains: its index in
 * ua->types. A type being resolved further up (a recursive definition) is
 * returned as is; a failure is remembered for the session. */
static int struct_of(tdot_device_t *dev, const UA_NodeId *dt, size_t *idx, char *err,
                     size_t errlen) {
    ua_device_t *ua = dev->proto;
    for (size_t i = 0; i < ua->types.n; i++) {
        if (!UA_NodeId_equal(&ua->tslots[i].id, dt))
            continue;
        if (ua->tslots[i].err) {
            snprintf(err, errlen, "%s", ua->tslots[i].err);
            return -1;
        }
        *idx = i;
        return 0;
    }
    size_t i = ua->types.n;
    uas_struct_t *structs = realloc(ua->types.structs, (i + 1) * sizeof *structs);
    if (structs)
        ua->types.structs = structs;
    ua_tslot_t *slots = realloc(ua->tslots, (i + 1) * sizeof *slots);
    if (slots)
        ua->tslots = slots;
    if (!structs || !slots) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    memset(&ua->types.structs[i], 0, sizeof ua->types.structs[i]);
    memset(&ua->tslots[i], 0, sizeof ua->tslots[i]);
    UA_NodeId_copy(dt, &ua->tslots[i].id);
    char text[200];
    node_str(dt, text, sizeof text);
    ua->types.structs[i].name = strdup(text);
    ua->types.n++;
    if (load_struct(dev, i, err, errlen) != 0) {
        ua->tslots[i].err = strdup(err);
        return -1;
    }
    *idx = i;
    return 0;
}

/* The structure inside a value: its encoding id (*enc, borrowed or the
 * type's) and body (*body; *owned when re-encoded here, then cleared by the
 * caller). 1 when v holds a structure, 0 when it does not, -1 with err. */
static int structure_body(const UA_Variant *v, const UA_NodeId **enc, UA_ByteString *body,
                          bool *owned, char *err, size_t errlen) {
    *owned = false;
    UA_ByteString_init(body);
    if (!v->type || !UA_Variant_isScalar(v))
        return 0;
    const UA_DataType *type = v->type;
    const void *data = v->data;
    if (type == &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]) {
        const UA_ExtensionObject *eo = data;
        switch (eo->encoding) {
        case UA_EXTENSIONOBJECT_ENCODED_NOBODY:
            *enc = &eo->content.encoded.typeId;
            return 1;
        case UA_EXTENSIONOBJECT_ENCODED_BYTESTRING:
            *enc = &eo->content.encoded.typeId;
            *body = eo->content.encoded.body; /* borrowed */
            return 1;
        case UA_EXTENSIONOBJECT_ENCODED_XML:
            snprintf(err, errlen, "the value is an XML-encoded structure, which is not supported");
            return -1;
        default:
            type = eo->content.decoded.type;
            data = eo->content.decoded.data;
            break;
        }
    } else if (type->typeKind != UA_DATATYPEKIND_STRUCTURE &&
               type->typeKind != UA_DATATYPEKIND_OPTSTRUCT &&
               type->typeKind != UA_DATATYPEKIND_UNION) {
        return 0;
    }
    /* A type open62541 knows (namespace 0) arrives decoded: encode it again
     * for its body, as the Rust module does. */
    *enc = &type->binaryEncodingId;
    if (UA_encodeBinary(data, type, body, NULL) != UA_STATUSCODE_GOOD) {
        snprintf(err, errlen, "re-encoding a %s value failed", type->typeName ? type->typeName : "structure");
        return -1;
    }
    *owned = true;
    return 1;
}

/* The DataType of the structure a variable currently holds: its value's
 * encoding id, and the DataType that encoding belongs to (inverse
 * HasEncoding). */
static int concrete_type(tdot_device_t *dev, const UA_NodeId *node, int64_t index, UA_NodeId *out,
                         char *err, size_t errlen) {
    ua_device_t *ua = dev->proto;
    UA_ReadValueId id;
    UA_ReadValueId_init(&id);
    id.nodeId = *node; /* borrowed */
    id.attributeId = UA_ATTRIBUTEID_VALUE;
    id.indexRange = tdot_ua_index_range(index);
    UA_ReadResponse resp;
    int r = read_nodes(ua, &id, 1, &resp, err, errlen);
    UA_String_clear(&id.indexRange);
    if (r != 0)
        return -1;
    char text[200], enc_text[300];
    node_str(node, text, sizeof text);
    UA_Variant el = resp.results[0].value;
    if (el.type && !UA_Variant_isScalar(&el) && el.arrayLength > 0)
        UA_Variant_setScalar(&el, el.data, el.type); /* the first element, borrowed */
    const UA_NodeId *enc = NULL;
    UA_ByteString body;
    bool owned;
    char why[200];
    int rc = -1;
    if (structure_body(&el, &enc, &body, &owned, why, sizeof why) != 1) {
        snprintf(err, errlen,
                 "%s is declared with an abstract DataType and does not hold a structure", text);
    } else {
        UA_NodeId encoding;
        UA_NodeId_copy(enc, &encoding);
        if (owned)
            UA_ByteString_clear(&body);
        node_str(&encoding, enc_text, sizeof enc_text);
        UA_StatusCode sc;
        int b = browse_inverse(ua, &encoding, UA_NS0_HASENCODING, out, &sc);
        if (b < 0)
            snprintf(err, errlen, "browsing the DataType of encoding %s: %s", enc_text,
                     UA_StatusCode_name(sc));
        else if (b > 0)
            snprintf(err, errlen,
                     "%s is declared with an abstract DataType, and the server names no "
                     "DataType for its encoding %s",
                     text, enc_text);
        else
            rc = 0;
        UA_NodeId_clear(&encoding);
    }
    UA_ReadResponse_clear(&resp);
    return rc;
}

/* Resolve one field point against its DataType `dt` (read already). */
static void resolve_field_point(tdot_device_t *dev, tdot_point_t *pt, const UA_NodeId *node,
                                const UA_NodeId *dt) {
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;
    char why[TDOT_ERR_MAX * 2];
    UA_NodeId concrete = UA_NODEID_NULL;
    int b = builtin_of(dt);
    if (b == UAS_EXTENSIONOBJECT || b == UAS_VARIANT) {
        /* Declared with the abstract Structure (or BaseDataType): the concrete
         * type is the one its value is encoded as. */
        if (concrete_type(dev, node, up->index, &concrete, why, sizeof why) != 0) {
            snprintf(up->unresolved, sizeof up->unresolved, UA_UNAVAILABLE ": %s", why);
            return;
        }
        dt = &concrete;
    }
    size_t root;
    if (struct_of(dev, dt, &root, why, sizeof why) != 0) {
        snprintf(up->unresolved, sizeof up->unresolved, UA_UNAVAILABLE ": %s", why);
    } else {
        uas_path_t path;
        if (uas_parse_path(up->field, &path, why, sizeof why) == 0) {
            tdot_datatype_t datatype =
                pt->datatype != TDOT_DT_NONE ? pt->datatype : TDOT_DT_FLOAT64;
            if (uas_compile(&ua->types, root, &path, datatype, &up->plan, why, sizeof why) == 0) {
                up->has_plan = true;
                UA_NodeId_copy(&ua->tslots[root].encoding, &up->encoding);
            }
            uas_path_free(&path);
        }
        if (!up->has_plan)
            snprintf(up->unresolved, sizeof up->unresolved, "%s", why);
    }
    UA_NodeId_clear(&concrete);
}

/* Resolve, for this session, what the device's field and raw points need
 * whose node is known (resolve.rs `resolve`): the namespace tables, each
 * node's DataType in one Read per block, and the definitions the field paths
 * cross or skip. Points resolved in this session are left alone. Never fails
 * the link: a failure is the point's bad sample. */
static void resolve_device(tdot_device_t *dev) {
    ua_device_t *ua = dev->proto;
    if (!ua->client || !ua->types_gen)
        return;
    size_t want = 0;
    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        ua_point_t *up = pt->proto;
        const char *reason;
        if (needs_types(pt) && up->gen != ua->types_gen && point_node(up, &reason))
            want++;
    }
    if (!want)
        return;
    read_namespaces(ua);
    size_t block = ua->nodes_per_read ? ua->nodes_per_read : UA_NODES_PER_READ;
    UA_ReadValueId *ids = calloc(block, sizeof *ids);
    size_t *at = calloc(block, sizeof *at);
    if (!ids || !at) {
        free(ids);
        free(at);
        return;
    }
    size_t j = 0;
    while (j < dev->npoints) {
        size_t n = 0;
        for (; j < dev->npoints && n < block; j++) {
            tdot_point_t *pt = &dev->points[j];
            ua_point_t *up = pt->proto;
            const char *reason;
            char *text;
            if (!needs_types(pt) || up->gen == ua->types_gen || !(text = point_node(up, &reason)))
                continue;
            UA_ReadValueId_init(&ids[n]);
            if (UA_NodeId_parse(&ids[n].nodeId, UA_STRING(text)) != UA_STATUSCODE_GOOD)
                continue; /* the read path reports it */
            ids[n].attributeId = UA_ATTRIBUTEID_DATATYPE;
            at[n++] = j;
        }
        if (n == 0)
            break;
        char err[200];
        UA_ReadResponse resp;
        bool ok = read_nodes(ua, ids, n, &resp, err, sizeof err) == 0;
        for (size_t k = 0; k < n; k++) {
            tdot_point_t *pt = &dev->points[at[k]];
            ua_point_t *up = pt->proto;
            uas_plan_free(&up->plan);
            up->has_plan = false;
            UA_NodeId_clear(&up->encoding);
            UA_NodeId_clear(&up->data_type);
            up->unresolved[0] = '\0';
            up->gen = ua->types_gen;
            char text[200], why[TDOT_ERR_MAX];
            node_str(&ids[k].nodeId, text, sizeof text);
            const UA_DataValue *dv = ok ? &resp.results[k] : NULL;
            const UA_NodeId *dt = NULL;
            if (!dv)
                snprintf(why, sizeof why, "%s", err);
            else if (dv->hasStatus && !UA_StatusCode_isGood(dv->status))
                snprintf(why, sizeof why, "reading the DataType of %s: %s", text,
                         UA_StatusCode_name(dv->status));
            else if (!UA_Variant_hasScalarType(&dv->value, &UA_TYPES[UA_TYPES_NODEID]))
                snprintf(why, sizeof why, "%s has no DataType", text);
            else
                dt = dv->value.data;
            if (!up->field[0]) {
                if (dt)
                    UA_NodeId_copy(dt, &up->data_type);
            } else if (!dt) {
                snprintf(up->unresolved, sizeof up->unresolved, UA_UNAVAILABLE ": %s", why);
            } else {
                resolve_field_point(dev, pt, &ids[k].nodeId, dt);
            }
        }
        if (ok)
            UA_ReadResponse_clear(&resp);
        for (size_t k = 0; k < n; k++)
            UA_ReadValueId_clear(&ids[k]);
    }
    free(ids);
    free(at);
}

static const char *variant_type_name(const UA_Variant *v) {
    if (!v->type)
        return "empty";
    if (!UA_Variant_isScalar(v))
        return "an array";
    if (v->type->typeKind <= UA_DATATYPEKIND_DIAGNOSTICINFO)
        return uas_builtin_name((int)v->type->typeKind + 1);
    return "ExtensionObject";
}

/* A decoded walker value into the point's sample, with its transform. */
static void sample_from(const uas_out_t *o, const tdot_point_t *pt, tdot_sample_t *out) {
    out->value = o->val;
    memcpy(out->raw, o->raw, o->raw_len);
    out->raw_len = o->raw_len;
    out->raw_group = 1;
    if (out->value.kind == TDOT_VAL_NUM && pt->has_transform)
        out->value.num = tdot_transform_apply(&pt->transform, out->value.num);
}

/* Decode a value as the point selects it (lib.rs decode_value and
 * build_sample): an array element by index, a structure field, a raw
 * structure body, a primitive, or a built-in type rendered as an SDK
 * datatype. Never does I/O: it also runs inside open62541's data-change
 * callback. Returns 0 (a bad value is a bad sample, not a link fault). */
static int decode_value(tdot_device_t *dev, tdot_point_t *pt, const UA_Variant *v,
                        tdot_sample_t *out) {
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;
    char err[TDOT_ERR_MAX * 2];
    UA_Variant el = *v; /* shallow: never cleared */
    /* A read with an IndexRange returns an array holding just the selected
     * element. A server that ignores the range returns the whole array: the
     * element is selected here. Only a one-element array cannot be told
     * apart, and is taken as the answer. */
    if (up->index >= 0 && v->type && !UA_Variant_isScalar(v)) {
        size_t len = v->arrayLength;
        size_t i = len == 1 ? 0 : (size_t)up->index;
        if (len == 0) {
            tdot_sample_bad(out, "index %" PRId64 " out of range (no element returned)", up->index);
            return 0;
        }
        if (i >= len) {
            tdot_sample_bad(out, "index %" PRId64 " out of range (length %zu)", up->index, len);
            return 0;
        }
        UA_Variant_setScalar(&el, (char *)v->data + i * v->type->memSize, v->type);
    }
    bool resolved = ua->types_gen && up->gen == ua->types_gen;
    const UA_NodeId *enc = NULL;
    UA_ByteString body;
    bool owned = false;
    if (up->field[0]) {
        if (!resolved) {
            tdot_sample_bad(out, "%s: not resolved", UA_UNAVAILABLE);
            return 0;
        }
        if (!up->has_plan) {
            tdot_sample_bad(out, "%s", up->unresolved);
            return 0;
        }
        int s = structure_body(&el, &enc, &body, &owned, err, sizeof err);
        if (s <= 0) {
            if (s == 0)
                tdot_sample_bad(out, "value is %s, not a structure", variant_type_name(&el));
            else
                tdot_sample_bad(out, "%s", err);
            return 0;
        }
        if (!UA_NodeId_isNull(&up->encoding) && !UA_NodeId_equal(enc, &up->encoding)) {
            char got[300], want[300];
            node_nsu(ua, enc, got, sizeof got);
            node_nsu(ua, &up->encoding, want, sizeof want);
            tdot_sample_bad(out, "value is encoded as %s, expected the structure's encoding %s",
                            got, want);
        } else {
            uas_out_t o;
            if (uas_decode(&up->plan, &ua->types, body.data, body.length,
                           (const char *const *)ua->srv_ns, ua->nsrv, &o, err, sizeof err) == 0)
                sample_from(&o, pt, out);
            else
                tdot_sample_bad(out, "%s", err);
        }
        if (owned)
            UA_ByteString_clear(&body);
        return 0;
    }
    int s = structure_body(&el, &enc, &body, &owned, err, sizeof err);
    if (s < 0) {
        tdot_sample_bad(out, "%s", err);
        return 0;
    }
    if (s > 0) {
        if (pt->mode == TDOT_MODE_TYPED) {
            tdot_sample_bad(out, "value is a structure; select one of its fields with address.field");
        } else if (body.length > TDOT_RAW_MAX) {
            /* Refused rather than cut (design C5). */
            tdot_sample_bad(out,
                            "structure body of %zu bytes is longer than a raw sample holds in "
                            "the C build (%d bytes)",
                            body.length, TDOT_RAW_MAX);
        } else {
            if (body.length)
                memcpy(out->raw, body.data, body.length);
            out->raw_len = body.length;
            out->raw_group = 1;
            /* The addr echo with the encoding and the DataType (C7). */
            cJSON *addr = cJSON_Parse(pt->addr_json ? pt->addr_json : "{}");
            if (addr) {
                char text[300];
                node_nsu(ua, enc, text, sizeof text);
                cJSON_AddStringToObject(addr, "encoding_id", text);
                if (resolved && !UA_NodeId_isNull(&up->data_type)) {
                    node_nsu(ua, &up->data_type, text, sizeof text);
                    cJSON_AddStringToObject(addr, "data_type", text);
                }
                if (cJSON_PrintPreallocated(addr, up->addr_dyn, (int)sizeof up->addr_dyn, 0))
                    out->addr_json = up->addr_dyn;
                cJSON_Delete(addr);
            }
        }
        if (owned)
            UA_ByteString_clear(&body);
        return 0;
    }
    if (el.type && !UA_Variant_isScalar(&el)) {
        tdot_sample_bad(out, "value is an array; select one element with address.index");
        return 0;
    }
    int b = el.type && el.type->typeKind <= UA_DATATYPEKIND_DIAGNOSTICINFO
                ? (int)el.type->typeKind + 1
                : 0;
    bool primitive = (b >= UAS_BOOLEAN && b <= UAS_STRING) || b == UAS_BYTESTRING;
    if (!b || primitive)
        return variant_to_sample(&el, pt, out);
    /* A built-in type with no SDK primitive of its own: its binary form,
     * decoded like a field (C4). */
    uas_leaf_t leaf = {.builtin = b};
    tdot_datatype_t accepted[2];
    tdot_datatype_t dt = pt->datatype;
    if (pt->mode != TDOT_MODE_TYPED || dt == TDOT_DT_NONE) {
        if (uas_accepted(&leaf, accepted) == 0) {
            tdot_sample_bad(out, "value is %s, which a point cannot read", uas_builtin_name(b));
            return 0;
        }
        dt = accepted[0];
    }
    UA_ByteString bytes = UA_BYTESTRING_NULL;
    if (UA_encodeBinary(el.data, el.type, &bytes, NULL) != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(out, "re-encoding a %s value failed", uas_builtin_name(b));
        return 0;
    }
    uas_out_t o;
    if (uas_decode_builtin(b, bytes.data, bytes.length, dt, (const char *const *)ua->cli_ns,
                           ua->ncli, &o, err, sizeof err) == 0)
        sample_from(&o, pt, out);
    else
        tdot_sample_bad(out, "%s", err);
    UA_ByteString_clear(&bytes);
    return 0;
}

/* Everything read_point does before its Read: true with the node `pt` reads,
 * or false with *out a bad sample and *rc the outcome read_point returns (0,
 * -1). */
static bool point_read_node(tdot_device_t *dev, tdot_point_t *pt, UA_NodeId *node,
                            tdot_sample_t *out, int *rc) {
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;

    if (up && up->is_method) {
        /* Never offered by the runtime; an explicitly named CLI read. */
        tdot_sample_bad(out, "point %s is a method", pt->id);
        *rc = 0;
        return false;
    }
    if (!ua->client) {
        tdot_sample_bad(out, "device not connected");
        *rc = -1;
        return false;
    }

    /* A point whose namespace this session has not translated is tried again
     * first -- once per tick for the device -- so a namespace the server
     * added since is picked up at the next poll (design D5). */
    const char *reason = NULL;
    char *node_text = point_node(up, &reason);
    if (!node_text && mono_s() - ua->last_retry >= 0.5) {
        ua->last_retry = mono_s();
        translate_device(dev, true);
        node_text = point_node(up, &reason);
    }
    if (!node_text) {
        tdot_sample_bad(out, "%s", reason);
        *rc = ua->renew_session ? -1 : 0;
        return false;
    }

    if (UA_NodeId_parse(node, UA_STRING(node_text)) != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(out, "invalid node id: %s", up->node_id);
        *rc = 0;
        return false;
    }
    /* A field or raw point's first read in a session resolves what the
     * device's points need, in one go (design C2). */
    if (needs_types(pt) && up->gen != ua->types_gen)
        resolve_device(dev);
    return true;
}

/* A read that failed with `rc`: whether the link is gone (the server answered
 * otherwise, so the transport is healthy). */
static bool read_link_lost(UA_StatusCode rc) {
    return rc == UA_STATUSCODE_BADCONNECTIONCLOSED ||
           rc == UA_STATUSCODE_BADSERVERNOTCONNECTED ||
           rc == UA_STATUSCODE_BADDISCONNECT ||
           rc == UA_STATUSCODE_BADTIMEOUT ||
           rc == UA_STATUSCODE_BADSESSIONIDINVALID ||
           rc == UA_STATUSCODE_BADSECURECHANNELCLOSED ||
           rc == UA_STATUSCODE_BADINTERNALERROR;
}

/* The sample of a point whose Read returned `rc` and, when GOOD, `value`;
 * read_point's outcome. */
static int read_result(tdot_device_t *dev, UA_StatusCode rc, const UA_Variant *value,
                       tdot_point_t *pt, tdot_sample_t *out) {
    const ua_point_t *up = pt->proto;
    if (rc == UA_STATUSCODE_BADINDEXRANGENODATA && up->index >= 0) {
        tdot_sample_bad(out, "index %" PRId64 " out of range (the server reports %s)", up->index,
                        UA_StatusCode_name(rc));
        return 0;
    }
    if (rc != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(out, "bad status: %s", UA_StatusCode_name(rc));
        return read_link_lost(rc) ? -1 : 0;
    }
    return decode_value(dev, pt, value, out);
}

static UA_StatusCode result_status(const UA_DataValue *dv);

static int read_point(tdot_connector_t *self, tdot_device_t *dev,
                      tdot_point_t *pt, tdot_sample_t *out) {
    (void)self;
    ua_device_t *ua = dev->proto;
    UA_NodeId node;
    int r;
    if (!point_read_node(dev, pt, &node, out, &r))
        return r;

    /* A Read of one node, with the point's IndexRange: what
     * UA_Client_readValueAttribute does, which takes none. */
    UA_ReadValueId id;
    UA_ReadValueId_init(&id);
    id.nodeId = node; /* owns it now */
    id.attributeId = UA_ATTRIBUTEID_VALUE;
    id.indexRange = tdot_ua_index_range(((ua_point_t *)pt->proto)->index);
    UA_ReadRequest req;
    UA_ReadRequest_init(&req);
    req.nodesToRead = &id;
    req.nodesToReadSize = 1;
    req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
    UA_ReadResponse resp = UA_Client_Service_read(ua->client, req);
    UA_StatusCode rc = resp.responseHeader.serviceResult;
    if (rc == UA_STATUSCODE_GOOD && resp.resultsSize != 1)
        rc = UA_STATUSCODE_BADUNEXPECTEDERROR;
    r = read_result(dev, rc == UA_STATUSCODE_GOOD ? result_status(&resp.results[0]) : rc,
                    rc == UA_STATUSCODE_GOOD ? &resp.results[0].value : NULL, pt, out);
    UA_ReadResponse_clear(&resp);
    UA_ReadValueId_clear(&id);
    return r;
}

/* What UA_Client_readValueAttribute would return for `dv`, one result of a
 * Read whose service call succeeded: its status, or BadUnexpectedError when
 * a good result carries no value. */
static UA_StatusCode result_status(const UA_DataValue *dv) {
    if (!UA_StatusCode_isEqualTop(dv->status, UA_STATUSCODE_GOOD))
        return dv->status;
    if (!dv->hasValue)
        return UA_STATUSCODE_BADUNEXPECTEDERROR;
    return dv->status;
}

/* The distinct (node, IndexRange) pairs of one block (design C6): an
 * open-addressing table of slot + 1 over nodes[], so that a block of 500 nodes
 * and thousands of points finds a shared node without a linear search. With no
 * table (out of memory) the search is linear. */
typedef struct {
    size_t *tab;
    size_t mask;
} ua_nodeset_t;

static void nodeset_init(ua_nodeset_t *set, size_t block) {
    size_t cap = 16;
    while (cap < 2 * block)
        cap <<= 1;
    set->tab = calloc(cap, sizeof *set->tab);
    set->mask = set->tab ? cap - 1 : 0;
}

static void nodeset_reset(ua_nodeset_t *set) {
    if (set->tab)
        memset(set->tab, 0, (set->mask + 1) * sizeof *set->tab);
}

static bool same_read(const UA_ReadValueId *r, const UA_NodeId *node, const UA_String *range) {
    return UA_NodeId_equal(&r->nodeId, node) && UA_String_equal(&r->indexRange, range);
}

/* The slot of node/range among nodes[0..*nnodes). A new one is added when the
 * block has room, taking over node and range; returns `block` when it is new
 * and the block is full. A shared one leaves node and range to the caller. */
static size_t nodeset_slot(ua_nodeset_t *set, UA_ReadValueId *nodes, size_t *nnodes,
                           size_t block, UA_NodeId *node, UA_String *range, bool *added) {
    *added = false;
    size_t h = 0;
    if (set->tab) {
        h = (UA_NodeId_hash(node) * 31u + (uint32_t)range->length) & set->mask;
        for (; set->tab[h]; h = (h + 1) & set->mask)
            if (same_read(&nodes[set->tab[h] - 1], node, range))
                return set->tab[h] - 1;
    } else {
        for (size_t j = 0; j < *nnodes; j++)
            if (same_read(&nodes[j], node, range))
                return j;
    }
    if (*nnodes >= block)
        return block;
    size_t slot = (*nnodes)++;
    UA_ReadValueId_init(&nodes[slot]);
    nodes[slot].nodeId = *node;      /* owns it now */
    nodes[slot].indexRange = *range; /* likewise */
    nodes[slot].attributeId = UA_ATTRIBUTEID_VALUE;
    if (set->tab)
        set->tab[h] = slot + 1;
    *added = true;
    return slot;
}

/* One Read of the `nnodes` nodes in nodes[], for the npts points at[m] (each
 * reading node at[n + m]): fills rc[] for them. Returns the first of them
 * whose rc is -1, or n when there is none. Clears nodes[]. */
static size_t read_block(const tdot_ua_blocks_t *io, UA_ReadValueId *nodes,
                         size_t nnodes, const size_t *at, size_t npts, size_t n, int *rc) {
    UA_ReadRequest req;
    UA_ReadRequest_init(&req);
    req.nodesToRead = nodes;
    req.nodesToReadSize = nnodes;
    req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
    UA_ReadResponse resp = io->read(io->ctx, &req);
    UA_StatusCode service = resp.responseHeader.serviceResult;
    if (service == UA_STATUSCODE_GOOD && resp.resultsSize != nnodes)
        service = UA_STATUSCODE_BADUNEXPECTEDERROR;

    size_t lost = n;
    for (size_t m = 0; m < npts; m++) {
        size_t k = at[m], j = at[n + m];
        if (service != UA_STATUSCODE_GOOD)
            rc[k] = io->result(io->ctx, k, service, NULL);
        else
            rc[k] = io->result(io->ctx, k, result_status(&resp.results[j]),
                               &resp.results[j].value);
        if (rc[k] == -1 && k < lost)
            lost = k;
    }
    for (size_t j = 0; j < nnodes; j++)
        UA_ReadValueId_clear(&nodes[j]);
    UA_ReadResponse_clear(&resp);
    return lost;
}

size_t tdot_ua_read_blocks(const tdot_ua_blocks_t *io, size_t n, size_t block,
                           UA_ReadValueId *nodes, size_t *at, int *rc) {
    ua_nodeset_t set;
    nodeset_init(&set, block);
    size_t nnodes = 0, npts = 0, stop = n;
    for (size_t k = 0; k < n; k++) {
        UA_NodeId node;
        int64_t index = -1;
        if (!io->prepare(io->ctx, k, &node, &index, &rc[k])) {
            if (rc[k] == -1) {
                stop = k;
                break;
            }
            continue;
        }
        UA_String range = tdot_ua_index_range(index);
        bool added;
        size_t slot = nodeset_slot(&set, nodes, &nnodes, block, &node, &range, &added);
        if (slot == block) {
            /* A new node and a full block: read it, then start the next. */
            size_t lost = read_block(io, nodes, nnodes, at, npts, n, rc);
            nnodes = npts = 0;
            nodeset_reset(&set);
            if (lost < n) {
                UA_NodeId_clear(&node);
                UA_String_clear(&range);
                free(set.tab);
                return lost + 1;
            }
            slot = nodeset_slot(&set, nodes, &nnodes, block, &node, &range, &added);
        }
        if (!added) {
            UA_NodeId_clear(&node);
            UA_String_clear(&range);
        }
        at[npts] = k;
        at[n + npts] = slot;
        npts++;
    }
    if (npts > 0) {
        size_t lost = read_block(io, nodes, nnodes, at, npts, n, rc);
        if (lost < stop)
            stop = lost;
    }
    free(set.tab);
    return stop < n ? stop + 1 : n;
}

/* read_points' points, for the tdot_ua_blocks_t callbacks below. */
typedef struct {
    tdot_device_t *dev;
    tdot_point_t *const *pts;
    tdot_sample_t *out;
} poll_ctx_t;

static bool poll_prepare(void *ctx, size_t k, UA_NodeId *node, int64_t *index, int *rc) {
    poll_ctx_t *c = ctx;
    if (!point_read_node(c->dev, c->pts[k], node, &c->out[k], rc))
        return false;
    *index = ((ua_point_t *)c->pts[k]->proto)->index;
    return true;
}

static UA_ReadResponse poll_read(void *ctx, UA_ReadRequest *req) {
    poll_ctx_t *c = ctx;
    ua_device_t *ua = c->dev->proto;
    return UA_Client_Service_read(ua->client, *req);
}

static int poll_result(void *ctx, size_t k, UA_StatusCode status, const UA_Variant *value) {
    poll_ctx_t *c = ctx;
    return read_result(c->dev, status, value, c->pts[k], &c->out[k]);
}

/* Poll pts[0..n) in blocks of at most ua->nodes_per_read nodes, one Read per
 * block instead of one per point: a pass over a large device then takes a
 * few round trips, not one per point. Each point's sample and outcome are
 * read_point's. Stops after the first point that takes the link down. */
static size_t read_points(tdot_connector_t *self, tdot_device_t *dev,
                          tdot_point_t *const *pts, size_t n, tdot_sample_t *out,
                          int *rc) {
    ua_device_t *ua = dev->proto;
    /* 0 until a session has read the server's limit; a block of 0 would never
     * read anything. */
    size_t block = ua->nodes_per_read ? ua->nodes_per_read : UA_NODES_PER_READ;
    if (block > n)
        block = n;
    UA_ReadValueId *nodes = calloc(block, sizeof *nodes);
    size_t *at = calloc(2 * n, sizeof *at);
    if (!nodes || !at) {
        free(nodes);
        free(at);
        /* Too short of memory for a block: one point at a time. */
        for (size_t k = 0; k < n; k++) {
            rc[k] = read_point(self, dev, pts[k], &out[k]);
            if (rc[k] == -1)
                return k + 1;
        }
        return n;
    }
    poll_ctx_t c = {.dev = dev, .pts = pts, .out = out};
    tdot_ua_blocks_t io = {
        .ctx = &c, .prepare = poll_prepare, .read = poll_read, .result = poll_result};
    size_t got = tdot_ua_read_blocks(&io, n, block, nodes, at, rc);
    free(nodes);
    free(at);
    return got;
}


/* ---- pipelined polling (contract §5.6) ----------------------------------------
 * read_points on a worker thread: the same blocks as read_points, with up to
 * tdot_inflight_current() Reads in flight at once on the device's client,
 * sent with UA_Client_sendAsyncReadRequest and driven by
 * UA_Client_run_iterate. The runtime reads one device on one thread at a
 * time, so nothing else drives this client meanwhile. */

/* How often one block is sent again after being refused as busy. */
#define UA_BUSY_RETRIES 8

/* BadServerTooBusy (OPC UA 1.05), which this open62541 does not name. */
#ifndef UA_STATUSCODE_BADSERVERTOOBUSY
#define UA_STATUSCODE_BADSERVERTOOBUSY ((UA_StatusCode)0x80EE0000)
#endif

typedef struct {
    UA_ReadValueId *nodes; /* cleared once the block is handled */
    size_t first;          /* its first point, an index into pts */
    size_t nnodes;
    unsigned tries;
    bool queued;   /* to be sent (again) */
    bool sent;     /* sent and not yet handled: counts against the allowance */
    bool answered; /* set by on_block_read, or by a send that failed */
    UA_ReadResponse resp;
} ua_pblock_t;

static void on_block_read(UA_Client *client, void *userdata, UA_UInt32 requestId,
                          UA_ReadResponse *rr) {
    (void)client;
    (void)requestId;
    ua_pblock_t *b = userdata;
    UA_ReadResponse_init(&b->resp);
    if (rr)
        UA_ReadResponse_copy(rr, &b->resp);
    else
        b->resp.responseHeader.serviceResult = UA_STATUSCODE_BADUNEXPECTEDERROR;
    b->answered = true;
}

/* A status that means the server is busy, not that the request or the link is
 * wrong: the endpoint backs off and the block is sent again. Not
 * BadTooManyOperations: a Read larger than the server accepts stays so. */
static bool ua_busy(UA_StatusCode rc) {
    return rc == UA_STATUSCODE_BADSERVERTOOBUSY || rc == UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
}

/* The endpoint key of a device's OPC UA endpoint URL (contract §5.6): its host
 * and port, lowercase, so devices on one server share its budget whatever
 * path or case their URLs use. As endpoint_key in the Rust module. */
static int endpoint(tdot_connector_t *self, const tdot_device_t *dev, char *buf, size_t len) {
    (void)self;
    const ua_device_t *ua = dev->proto;
    if (!ua || !ua->endpoint[0])
        return -1;
    const char *rest = strstr(ua->endpoint, "://");
    rest = rest ? rest + 3 : ua->endpoint;
    size_t n = strcspn(rest, "/");
    if (n + 6 >= len)
        n = len - 7;
    size_t i = 0;
    for (; i < n; i++)
        buf[i] = (char)tolower((unsigned char)rest[i]);
    buf[i] = '\0';
    bool has_port = memchr(buf, ':', i) != NULL && buf[i ? i - 1 : 0] != ']';
    if (!has_port)
        snprintf(buf + i, len - i, ":4840");
    return 0;
}

static size_t read_wanted(tdot_connector_t *self, tdot_device_t *dev, size_t n) {
    (void)self;
    ua_device_t *ua = dev->proto;
    size_t block = ua && ua->nodes_per_read ? ua->nodes_per_read : UA_NODES_PER_READ;
    return n ? (n + block - 1) / block : 1;
}

/* Publish a handled block's answer into its points' samples: the points
 * whose blk[] is `bi`, each from its node's result slot[]. Returns the first
 * of them whose outcome is -1, or `none`. */
static size_t block_results(tdot_device_t *dev, ua_pblock_t *b, size_t bi, UA_StatusCode service,
                            tdot_point_t *const *pts, const size_t *blk, const size_t *slot,
                            size_t n, tdot_sample_t *out, int *rc, size_t none) {
    size_t lost = none;
    for (size_t k = b->first; k < n; k++) {
        if (blk[k] != bi)
            continue;
        size_t j = slot[k];
        if (service != UA_STATUSCODE_GOOD)
            rc[k] = read_result(dev, service, NULL, pts[k], &out[k]);
        else
            rc[k] = read_result(dev, result_status(&b->resp.results[j]),
                                &b->resp.results[j].value, pts[k], &out[k]);
        if (rc[k] == -1 && k < lost)
            lost = k;
    }
    return lost;
}

static size_t read_points_inflight(tdot_connector_t *self, tdot_device_t *dev,
                                   tdot_point_t *const *pts, size_t n, tdot_sample_t *out,
                                   int *rc, tdot_inflight_t *inflight) {
    ua_device_t *ua = dev->proto;
    ua_state_t *st = self->state;
    size_t block = ua->nodes_per_read ? ua->nodes_per_read : UA_NODES_PER_READ;
    size_t maxblocks = n ? (n + block - 1) / block : 1;
    ua_pblock_t *blocks = calloc(maxblocks, sizeof *blocks);
    if (!blocks || !ua->client) {
        free(blocks);
        return read_points(self, dev, pts, n, out, rc);
    }
    /* Prepare every point; one that cannot be read gets its sample now, and
     * one that takes the link down ends the call there. Points on the same
     * node and index share its ReadValueId (design C6): blk[k] is point k's
     * block (SIZE_MAX: not read), slot[k] its node there. */
    size_t *blk = malloc(2 * (n ? n : 1) * sizeof *blk);
    size_t *slot = blk ? blk + n : NULL;
    ua_nodeset_t set;
    nodeset_init(&set, block);
    if (!blk) {
        free(blocks);
        free(set.tab);
        return read_points(self, dev, pts, n, out, rc);
    }
    size_t nb = 0, lost = n;
    for (size_t k = 0; k < n; k++)
        blk[k] = SIZE_MAX;
    for (size_t k = 0; k < n && lost == n; k++) {
        UA_NodeId node;
        if (!point_read_node(dev, pts[k], &node, &out[k], &rc[k])) {
            if (rc[k] == -1)
                lost = k;
            continue;
        }
        UA_String range = tdot_ua_index_range(((ua_point_t *)pts[k]->proto)->index);
        ua_pblock_t *b = nb > 0 ? &blocks[nb - 1] : NULL;
        bool added = false;
        size_t j = b ? nodeset_slot(&set, b->nodes, &b->nnodes, block, &node, &range, &added)
                     : block;
        if (j == block) {
            b = &blocks[nb];
            b->nodes = calloc(block, sizeof *b->nodes);
            if (!b->nodes) {
                UA_NodeId_clear(&node);
                UA_String_clear(&range);
                tdot_sample_bad(&out[k], "out of memory");
                rc[k] = -1;
                lost = k;
                break;
            }
            b->queued = true;
            b->first = k;
            nb++;
            nodeset_reset(&set);
            j = nodeset_slot(&set, b->nodes, &b->nnodes, block, &node, &range, &added);
        }
        if (!added) {
            UA_NodeId_clear(&node);
            UA_String_clear(&range);
        }
        blk[k] = (size_t)(b - blocks);
        slot[k] = j;
    }
    free(set.tab);

    size_t sent = 0; /* blocks sent and not yet handled */
    double deadline = mono_s() + 2.0 * (st ? st->request_timeout_s : 5) + 1.0;
    for (;;) {
        /* Send queued blocks in order, within what the call may have in flight;
         * nothing past a point that took the link down. */
        for (size_t i = 0; i < nb && sent < tdot_inflight_current(inflight); i++) {
            ua_pblock_t *b = &blocks[i];
            if (!b->queued || b->first > lost)
                continue;
            UA_ReadRequest req;
            UA_ReadRequest_init(&req);
            req.nodesToRead = b->nodes;
            req.nodesToReadSize = b->nnodes;
            req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
            b->queued = false;
            b->sent = true;
            sent++;
            UA_StatusCode rc_send =
                UA_Client_sendAsyncReadRequest(ua->client, &req, on_block_read, b, NULL);
            if (rc_send != UA_STATUSCODE_GOOD) {
                UA_ReadResponse_init(&b->resp);
                b->resp.responseHeader.serviceResult = rc_send;
                b->answered = true;
            }
        }
        /* Handle the answers. */
        bool more = false;
        for (size_t i = 0; i < nb; i++) {
            ua_pblock_t *b = &blocks[i];
            if (b->answered) {
                b->answered = false;
                b->sent = false;
                sent--;
                UA_StatusCode service = b->resp.responseHeader.serviceResult;
                if (service == UA_STATUSCODE_GOOD && b->resp.resultsSize != b->nnodes)
                    service = UA_STATUSCODE_BADUNEXPECTEDERROR;
                bool busy = ua_busy(service);
                bool behind = service == UA_STATUSCODE_BADTIMEOUT && sent > 0;
                if (b->tries < UA_BUSY_RETRIES && (busy || behind)) {
                    if (busy)
                        tdot_inflight_busy(inflight);
                    else
                        tdot_inflight_timed_out(inflight, true);
                    b->tries++;
                    b->queued = true;
                    UA_ReadResponse_clear(&b->resp);
                } else {
                    if (service == UA_STATUSCODE_GOOD)
                        tdot_inflight_ok(inflight);
                    size_t l = block_results(dev, b, i, service, pts, blk, slot, n, out, rc, n);
                    if (l < lost)
                        lost = l;
                    UA_ReadResponse_clear(&b->resp);
                    for (size_t j = 0; j < b->nnodes; j++)
                        UA_ReadValueId_clear(&b->nodes[j]);
                    b->nnodes = 0;
                }
            }
            if (b->queued && b->first <= lost)
                more = true;
        }
        if (sent == 0 && !more)
            break;
        if (mono_s() > deadline) {
            /* A last resort: open62541 times requests out itself. A block still
             * in flight keeps its memory (leaked below), since its answer may
             * arrive on a later iterate. */
            fprintf(stderr, "warn  device %s: %zu OPC UA Read(s) unanswered past the deadline\n",
                    dev->name, sent);
            break;
        }
        /* open62541 runs its event loop for the whole timeout, so a short one
         * sends the next block as soon as an answer frees its place. */
        UA_StatusCode it = UA_Client_run_iterate(ua->client, 1);
        if (it != UA_STATUSCODE_GOOD && read_link_lost(it)) {
            /* The channel is gone: nothing queued is sent. What is in flight is
             * answered by open62541 as it cancels it, or by the deadline. */
            for (size_t i = 0; i < nb; i++)
                if (blocks[i].queued) {
                    blocks[i].queued = false;
                    UA_ReadResponse_init(&blocks[i].resp);
                    blocks[i].resp.responseHeader.serviceResult = it;
                    blocks[i].answered = true;
                    blocks[i].sent = true;
                    sent++;
                }
        }
    }
    bool keep = false;
    for (size_t i = 0; i < nb; i++) {
        ua_pblock_t *b = &blocks[i];
        if (b->sent) {
            keep = true; /* its answer may still arrive: leave it be */
            continue;
        }
        for (size_t j = 0; j < b->nnodes; j++)
            UA_ReadValueId_clear(&b->nodes[j]);
        free(b->nodes);
    }
    if (!keep)
        free(blocks);
    free(blk);
    return lost < n ? lost + 1 : n;
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

    const char *reason = NULL;
    char *node_text = point_node(up, &reason);
    if (!node_text) {
        snprintf(err, errlen, "%s", reason);
        return -1;
    }
    UA_NodeId node;
    if (UA_NodeId_parse(&node, UA_STRING(node_text)) !=
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

/* ---- method calls (openspec change opcua-method-calls) ------------------ */

/* Largest integer a JSON number carries exactly (2^53 - 1). */
#define UA_MAX_SAFE_INT 9007199254740991.0

/* Convert method argument `k` (1-based) to its declared input type, into
 * `out`. Strings are borrowed from `arg`. Integers must be whole and in range;
 * int64/uint64 also take a decimal string, for values past 2^53. */
static int argument_variant(tdot_datatype_t dt, const cJSON *arg, size_t k,
                            UA_Variant *out, void *slot, char *err, size_t errlen) {
    UA_Variant_init(out);
    bool ok = false;
    double num = cJSON_IsNumber(arg) ? arg->valuedouble : 0;
    bool whole = cJSON_IsNumber(arg) && isfinite(num) && num == floor(num);
#define UA_ARG_INT(ctype, uatype, lo, hi)                                              \
    if (whole && num >= (lo) && num <= (hi)) {                                         \
        *(ctype *)slot = (ctype)num;                                                   \
        UA_Variant_setScalar(out, slot, &UA_TYPES[uatype]);                            \
        ok = true;                                                                     \
    }
    switch (dt) {
    case TDOT_DT_BOOL:
        if (cJSON_IsBool(arg)) {
            *(UA_Boolean *)slot = cJSON_IsTrue(arg);
            UA_Variant_setScalar(out, slot, &UA_TYPES[UA_TYPES_BOOLEAN]);
            ok = true;
        }
        break;
    case TDOT_DT_INT8:
        UA_ARG_INT(UA_SByte, UA_TYPES_SBYTE, -128, 127)
        break;
    case TDOT_DT_UINT8:
        UA_ARG_INT(UA_Byte, UA_TYPES_BYTE, 0, 255)
        break;
    case TDOT_DT_INT16:
        UA_ARG_INT(UA_Int16, UA_TYPES_INT16, -32768, 32767)
        break;
    case TDOT_DT_UINT16:
        UA_ARG_INT(UA_UInt16, UA_TYPES_UINT16, 0, 65535)
        break;
    case TDOT_DT_INT32:
        UA_ARG_INT(UA_Int32, UA_TYPES_INT32, -2147483648.0, 2147483647.0)
        break;
    case TDOT_DT_UINT32:
        UA_ARG_INT(UA_UInt32, UA_TYPES_UINT32, 0, 4294967295.0)
        break;
    case TDOT_DT_INT64:
    case TDOT_DT_UINT64: {
        bool is_signed = dt == TDOT_DT_INT64;
        const UA_DataType *type = &UA_TYPES[is_signed ? UA_TYPES_INT64 : UA_TYPES_UINT64];
        if (cJSON_IsString(arg) && arg->valuestring[0]) {
            const char *text = arg->valuestring;
            char *end = NULL;
            errno = 0;
            if (is_signed) {
                long long v = strtoll(text, &end, 10);
                ok = !errno && !*end && !isspace((unsigned char)text[0]);
                *(UA_Int64 *)slot = v;
            } else {
                unsigned long long v = strtoull(text, &end, 10);
                ok = !errno && !*end && isdigit((unsigned char)text[0]);
                *(UA_UInt64 *)slot = v;
            }
        } else if (whole && fabs(num) <= UA_MAX_SAFE_INT && (is_signed || num >= 0)) {
            if (is_signed)
                *(UA_Int64 *)slot = (UA_Int64)num;
            else
                *(UA_UInt64 *)slot = (UA_UInt64)num;
            ok = true;
        }
        if (ok)
            UA_Variant_setScalar(out, slot, type);
        break;
    }
    case TDOT_DT_FLOAT32:
        if (cJSON_IsNumber(arg)) {
            *(UA_Float *)slot = (UA_Float)num;
            UA_Variant_setScalar(out, slot, &UA_TYPES[UA_TYPES_FLOAT]);
            ok = true;
        }
        break;
    case TDOT_DT_FLOAT64:
        if (cJSON_IsNumber(arg)) {
            *(UA_Double *)slot = num;
            UA_Variant_setScalar(out, slot, &UA_TYPES[UA_TYPES_DOUBLE]);
            ok = true;
        }
        break;
    case TDOT_DT_STRING:
        if (cJSON_IsString(arg)) {
            *(UA_String *)slot = UA_STRING(arg->valuestring);
            UA_Variant_setScalar(out, slot, &UA_TYPES[UA_TYPES_STRING]);
            ok = true;
        }
        break;
    default:
        break;
    }
#undef UA_ARG_INT
    if (!ok) {
        char *text = cJSON_PrintUnformatted(arg);
        snprintf(err, errlen, "argument %zu: %s is not a valid %s", k,
                 text ? text : "?", tdot_datatype_str(dt));
        free(text);
    }
    return ok ? 0 : -1;
}

/* One element of an output argument as JSON, the way a sample renders the
 * same value; NULL for a type samples do not decode. */
static cJSON *output_scalar(const UA_DataType *type, const void *p) {
    if (type == &UA_TYPES[UA_TYPES_BOOLEAN])
        return cJSON_CreateBool(*(const UA_Boolean *)p);
    if (type == &UA_TYPES[UA_TYPES_SBYTE])
        return cJSON_CreateNumber(*(const UA_SByte *)p);
    if (type == &UA_TYPES[UA_TYPES_BYTE])
        return cJSON_CreateNumber(*(const UA_Byte *)p);
    if (type == &UA_TYPES[UA_TYPES_INT16])
        return cJSON_CreateNumber(*(const UA_Int16 *)p);
    if (type == &UA_TYPES[UA_TYPES_UINT16])
        return cJSON_CreateNumber(*(const UA_UInt16 *)p);
    if (type == &UA_TYPES[UA_TYPES_INT32])
        return cJSON_CreateNumber(*(const UA_Int32 *)p);
    if (type == &UA_TYPES[UA_TYPES_UINT32])
        return cJSON_CreateNumber(*(const UA_UInt32 *)p);
    char text[32];
    if (type == &UA_TYPES[UA_TYPES_INT64]) {
        UA_Int64 x = *(const UA_Int64 *)p;
        if (x >= -(UA_Int64)UA_MAX_SAFE_INT && x <= (UA_Int64)UA_MAX_SAFE_INT)
            return cJSON_CreateNumber((double)x);
        snprintf(text, sizeof text, "%lld", (long long)x);
        return cJSON_CreateString(text);
    }
    if (type == &UA_TYPES[UA_TYPES_UINT64]) {
        UA_UInt64 x = *(const UA_UInt64 *)p;
        if (x <= (UA_UInt64)UA_MAX_SAFE_INT)
            return cJSON_CreateNumber((double)x);
        snprintf(text, sizeof text, "%llu", (unsigned long long)x);
        return cJSON_CreateString(text);
    }
    double d = type == &UA_TYPES[UA_TYPES_FLOAT]    ? (double)*(const UA_Float *)p
               : type == &UA_TYPES[UA_TYPES_DOUBLE] ? *(const UA_Double *)p
                                                    : NAN;
    if (type == &UA_TYPES[UA_TYPES_FLOAT] || type == &UA_TYPES[UA_TYPES_DOUBLE])
        return isfinite(d) ? cJSON_CreateNumber(d) : cJSON_CreateNull();
    if (type == &UA_TYPES[UA_TYPES_STRING]) {
        const UA_String *str = p;
        char *copy = malloc(str->length + 1);
        if (!copy)
            return NULL;
        memcpy(copy, str->data, str->length);
        copy[str->length] = '\0';
        cJSON *out = cJSON_CreateString(copy);
        free(copy);
        return out;
    }
    return NULL;
}

/* An output argument as JSON (design D8): a scalar as in a sample, a
 * one-dimensional array of them as a JSON array, anything else null -- the
 * method has run, so an output that cannot be rendered never fails the call. */
static cJSON *output_json(const UA_Variant *v) {
    if (!v->type)
        return cJSON_CreateNull();
    if (UA_Variant_isScalar(v)) {
        cJSON *out = output_scalar(v->type, v->data);
        return out ? out : cJSON_CreateNull();
    }
    if (v->arrayDimensionsSize > 1)
        return cJSON_CreateNull();
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < v->arrayLength; i++) {
        cJSON *item = output_scalar(v->type, (const char *)v->data + i * v->type->memSize);
        if (!item) {
            cJSON_Delete(array);
            return cJSON_CreateNull();
        }
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

static int call_method(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                       const cJSON *arguments, cJSON **outputs, char *err, size_t errlen) {
    (void)self;
    ua_device_t *ua = dev->proto;
    ua_point_t *up = pt->proto;
    *outputs = NULL;
    if (!up || !up->is_method) {
        snprintf(err, errlen, "point %s is not a method", pt->id);
        return -1;
    }
    if (!ua->client) {
        snprintf(err, errlen, "device %s is not connected", dev->name);
        return -1;
    }

    /* Both ids through the namespace table, retrying a translation that
     * failed, as a read does. */
    const char *reason = NULL;
    char *method_text = point_node(up, &reason);
    if (!method_text) {
        translate_device(dev, true);
        method_text = point_node(up, &reason);
    }
    if (!method_text) {
        snprintf(err, errlen, "%s", reason);
        return -1;
    }
    const char *object_text = up->table ? up->object_resolved : up->object_id;

    size_t given = (size_t)cJSON_GetArraySize(arguments);
    if (given != up->ninputs) {
        snprintf(err, errlen, "expected %zu arguments, got %zu", up->ninputs, given);
        return -1;
    }
    /* Each argument's value lives in its own slot; strings stay borrowed. */
    union {
        UA_Boolean b;
        UA_Int64 i;
        UA_UInt64 u;
        UA_Double d;
        UA_Float f;
        UA_String s;
    } *slots = calloc(up->ninputs ? up->ninputs : 1, sizeof *slots);
    UA_Variant *input = calloc(up->ninputs ? up->ninputs : 1, sizeof *input);
    if (!slots || !input) {
        free(slots);
        free(input);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int rc = 0;
    for (size_t k = 0; k < up->ninputs && rc == 0; k++)
        rc = argument_variant(up->inputs[k], cJSON_GetArrayItem(arguments, (int)k), k + 1,
                              &input[k], &slots[k], err, errlen);

    UA_NodeId object = UA_NODEID_NULL, method = UA_NODEID_NULL;
    if (rc == 0 && (UA_NodeId_parse(&object, UA_STRING((char *)object_text)) !=
                        UA_STATUSCODE_GOOD ||
                    UA_NodeId_parse(&method, UA_STRING(method_text)) != UA_STATUSCODE_GOOD)) {
        snprintf(err, errlen, "invalid node id: %s", up->node_id);
        rc = -1;
    }
    if (rc == 0) {
        UA_CallMethodRequest item;
        UA_CallMethodRequest_init(&item);
        item.objectId = object;
        item.methodId = method;
        item.inputArgumentsSize = up->ninputs;
        item.inputArguments = up->ninputs ? input : NULL;
        UA_CallRequest request;
        UA_CallRequest_init(&request);
        request.methodsToCall = &item;
        request.methodsToCallSize = 1;
        /* Bounded by the client's timeout, which is request_timeout_s; a
         * timeout arrives as the service result (BadTimeout). */
        UA_CallResponse response = UA_Client_Service_call(ua->client, request);
        UA_StatusCode service = response.responseHeader.serviceResult;
        if (service == UA_STATUSCODE_GOOD && response.resultsSize != 1)
            service = UA_STATUSCODE_BADUNEXPECTEDERROR;
        if (service != UA_STATUSCODE_GOOD) {
            snprintf(err, errlen, "call failed: %s", UA_StatusCode_name(service));
            rc = -1;
        } else {
            const UA_CallMethodResult *result = &response.results[0];
            size_t bad_arg = 0;
            for (size_t k = 0; k < result->inputArgumentResultsSize && !bad_arg; k++)
                if (result->inputArgumentResults[k] != UA_STATUSCODE_GOOD)
                    bad_arg = k + 1;
            if (bad_arg) {
                snprintf(err, errlen, "call rejected: argument %zu: %s", bad_arg,
                         UA_StatusCode_name(result->inputArgumentResults[bad_arg - 1]));
                rc = -1;
            } else if (UA_StatusCode_isBad(result->statusCode)) {
                snprintf(err, errlen, "call rejected: %s",
                         UA_StatusCode_name(result->statusCode));
                rc = -1;
            } else {
                *outputs = cJSON_CreateArray();
                for (size_t k = 0; k < result->outputArgumentsSize; k++)
                    cJSON_AddItemToArray(*outputs, output_json(&result->outputArguments[k]));
            }
        }
        UA_CallResponse_clear(&response);
    }
    UA_NodeId_clear(&object);
    UA_NodeId_clear(&method);
    free(slots);
    free(input);
    return rc;
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

double tdot_ua_source_ts_ms(bool has_source, long long source, bool has_server,
                            long long server) {
    /* Ticks between the OPC UA epoch (1601-01-01) and the Unix epoch. */
    const long long unix_epoch = 116444736000000000LL;
    long long ticks = has_source && source != 0 ? source : has_server && server != 0 ? server : 0;
    return ticks ? (double)(ticks - unix_epoch) / 10000.0 : 0;
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

    /* A full ring grows. Only past its bound is the NEWEST change dropped
     * (and counted), so the queued samples keep their order and none is
     * silently replaced. */
    ua_pending_t *slot = ua_ring_slot(&ua->queue);
    if (!slot)
        return;
    slot->pt = pt;
    tdot_sample_init(&slot->sample);
    const ua_point_t *up = pt->proto;
    if (value->hasStatus && value->status == UA_STATUSCODE_BADINDEXRANGENODATA &&
        up->index >= 0) {
        tdot_sample_bad(&slot->sample, "index %" PRId64 " out of range (the server reports %s)",
                        up->index, UA_StatusCode_name(value->status));
    } else if (!value->hasValue) {
        tdot_sample_bad(&slot->sample, "no scalar value in notification");
    } else if (value->hasStatus && value->status != UA_STATUSCODE_GOOD) {
        tdot_sample_bad(&slot->sample, "%s",
                        UA_StatusCode_name(value->status));
    } else {
        /* Structures were resolved before the items were created
         * (subscribe_device, retry_pending): no request is made in here. */
        decode_value(dev, pt, &value->value, &slot->sample);
        /* A pushed value carries when the source saw it (contract §5), as in
         * the Rust module; a polled one carries its read time. */
        slot->sample.source_ts_ms = tdot_ua_source_ts_ms(
            value->hasSourceTimestamp, value->sourceTimestamp, value->hasServerTimestamp,
            value->serverTimestamp);
    }
    ua_ring_commit(&ua->queue);
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

/* Queue a bad sample for a pushed point, as a notification would. */
static void push_bad(tdot_device_t *dev, tdot_point_t *pt, const char *reason) {
    ua_device_t *ua = dev->proto;
    ua_pending_t *slot = ua_ring_slot(&ua->queue);
    if (!slot)
        return;
    slot->pt = pt;
    tdot_sample_init(&slot->sample);
    tdot_sample_bad(&slot->sample, "%s", reason);
    ua_ring_commit(&ua->queue);
}

/* Create the monitored item of one point on `node_text`. */
static UA_StatusCode monitor_point(tdot_device_t *dev, tdot_point_t *pt,
                                   char *node_text) {
    ua_device_t *ua = dev->proto;
    UA_NodeId node;
    if (UA_NodeId_parse(&node, UA_STRING(node_text)) != UA_STATUSCODE_GOOD) {
        UA_NodeId_clear(&node);
        return UA_STATUSCODE_BADNODEIDINVALID;
    }
    UA_MonitoredItemCreateRequest mreq = UA_MonitoredItemCreateRequest_default(node);
    mreq.itemToMonitor.indexRange = tdot_ua_index_range(((ua_point_t *)pt->proto)->index);
    mreq.requestedParameters.samplingInterval = tdot_ua_sampling_interval_ms(pt);
    mreq.requestedParameters.queueSize = ((ua_point_t *)pt->proto)->queue_size;
    mreq.requestedParameters.discardOldest = true;
    UA_MonitoredItemCreateResult mres = UA_Client_MonitoredItems_createDataChange(
        ua->client, ua->sub_id, UA_TIMESTAMPSTORETURN_BOTH, mreq, pt /*monContext*/,
        on_data_change, NULL);
    UA_NodeId_clear(&node);
    UA_String_clear(&mreq.itemToMonitor.indexRange);
    UA_StatusCode rc = mres.statusCode;
    if (rc == UA_STATUSCODE_GOOD &&
        tdot_ua_revised(mreq.requestedParameters.samplingInterval,
                        mres.revisedSamplingInterval))
        fprintf(stderr,
                "info  device %s: point %s: the server revised the point's "
                "sampling interval: requested %.0fms, revised %.0fms\n",
                dev->name, pt->id, mreq.requestedParameters.samplingInterval,
                mres.revisedSamplingInterval);
    UA_MonitoredItemCreateResult_clear(&mres); /* filterResult may be heap */
    return rc;
}

/* Try the subscribed points waiting for their namespace again (design D5):
 * those that now have a node get their monitored item, the others report why.
 * Every UA_RETRY_INTERVAL_S, from drain_subscriptions(). */
static void retry_pending(tdot_device_t *dev) {
    ua_device_t *ua = dev->proto;
    /* The interval first: this runs on every runtime tick, and looking for a
     * pending point visits every point of the device. */
    if (mono_s() < ua->retry_at)
        return;
    bool any = false;
    for (size_t j = 0; j < dev->npoints && !any; j++) {
        const ua_point_t *up = dev->points[j].proto;
        any = up && up->pending;
    }
    ua->retry_at = mono_s() + UA_RETRY_INTERVAL_S;
    if (!any)
        return;
    translate_device(dev, true);
    resolve_device(dev); /* points that have their node now (design C2) */
    char added[512] = "";
    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        ua_point_t *up = pt->proto;
        if (!up || !up->pending)
            continue;
        const char *reason = NULL;
        char *node_text = point_node(up, &reason);
        if (!node_text) {
            push_bad(dev, pt, reason);
            continue;
        }
        UA_StatusCode rc = monitor_point(dev, pt, node_text);
        if (rc != UA_STATUSCODE_GOOD) {
            char why[200];
            snprintf(why, sizeof why, "monitored item rejected: %s", UA_StatusCode_name(rc));
            push_bad(dev, pt, why);
            continue;
        }
        up->pending = false;
        size_t len = strlen(added);
        snprintf(added + len, sizeof added - len, "%s%s", len ? ", " : "", pt->id);
    }
    if (added[0])
        fprintf(stderr, "info  device %s: subscribed after translating their namespace: %s\n",
                dev->name, added);
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

    size_t wanted = 0;
    double fastest = tdot_ua_publishing_interval_ms(dev, &wanted);
    if (wanted == 0)
        return 0; /* nothing to push: armed, with no points */
    /* Before any item exists: creating them runs the client, which can
     * already deliver the first start values. */
    ua_ring_reserve(&ua->queue, wanted);
    /* Structures too: a data-change callback cannot make requests. */
    resolve_device(dev);

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

    /* The items go out in batches of at most UA_ITEMS_PER_REQUEST: one round
     * trip per batch rather than per point, which is what makes subscribing
     * tens of thousands of points practical. A refused item, or a whole batch
     * the server faults, leaves just those points on the polling schedule. */
    size_t cap = wanted < UA_ITEMS_PER_REQUEST ? wanted : UA_ITEMS_PER_REQUEST;
    UA_MonitoredItemCreateRequest *items = calloc(cap, sizeof *items);
    void **contexts = calloc(cap, sizeof *contexts);
    UA_Client_DataChangeNotificationCallback *callbacks =
        calloc(cap, sizeof *callbacks);
    if (!items || !contexts || !callbacks) {
        free(items);
        free(contexts);
        free(callbacks);
        snprintf(err, errlen, "out of memory arming %zu monitored item(s)", wanted);
        UA_Client_Subscriptions_deleteSingle(ua->client, ua->sub_id);
        ua->sub_id = 0;
        ua->subscribed = false;
        ua->sub_lost = false;
        return -1;
    }

    size_t armed = 0;
    size_t j = 0;
    while (j < dev->npoints) {
        size_t n = 0;
        for (; j < dev->npoints && n < cap; j++) {
            tdot_point_t *pt = &dev->points[j];
            if (!pt->subscribe || !(pt->access & TDOT_ACCESS_READ))
                continue;
            ua_point_t *up = pt->proto;
            const char *reason = NULL;
            char *node_text = point_node(up, &reason);
            if (!node_text) {
                /* Waiting for its namespace (design D4, D5): pushed, so it
                 * stays off the polling schedule -- its bad samples come
                 * through the queue and say nothing about the link -- and
                 * retry_pending() gives it its monitored item once the
                 * namespace is there. */
                up->pending = true;
                pt->subscribed = true;
                push_bad(dev, pt, reason);
                armed++;
                continue;
            }
            UA_NodeId node;
            if (UA_NodeId_parse(&node, UA_STRING(node_text)) !=
                UA_STATUSCODE_GOOD) {
                UA_NodeId_clear(&node);
                continue; /* the polling path reports this as a bad sample */
            }
            items[n] = UA_MonitoredItemCreateRequest_default(node); /* owns node */
            items[n].itemToMonitor.indexRange = tdot_ua_index_range(up->index);
            items[n].requestedParameters.samplingInterval =
                tdot_ua_sampling_interval_ms(pt);
            items[n].requestedParameters.queueSize = up->queue_size;
            items[n].requestedParameters.discardOldest = true;
            contexts[n] = pt;
            callbacks[n] = on_data_change;
            n++;
        }
        if (n == 0)
            break;

        UA_CreateMonitoredItemsRequest req;
        UA_CreateMonitoredItemsRequest_init(&req);
        req.subscriptionId = ua->sub_id;
        req.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
        req.itemsToCreate = items;
        req.itemsToCreateSize = n;
        UA_CreateMonitoredItemsResponse resp =
            UA_Client_MonitoredItems_createDataChanges(ua->client, req, contexts,
                                                       callbacks, NULL);
        if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD ||
            resp.resultsSize != n) {
            fprintf(stderr,
                    "warn  device %s: creating %zu monitored item(s) failed: %s; "
                    "they stay polled\n",
                    dev->name, n,
                    UA_StatusCode_name(resp.responseHeader.serviceResult));
        } else {
            for (size_t k = 0; k < n; k++) {
                tdot_point_t *pt = contexts[k];
                const UA_MonitoredItemCreateResult *mres = &resp.results[k];
                if (mres->statusCode != UA_STATUSCODE_GOOD)
                    continue;
                /* Telling the runtime this point is pushed is what takes it
                 * off the polling schedule. A node the server refuses to
                 * monitor simply stays polled. */
                pt->subscribed = true;
                armed++;
                const UA_MonitoringParameters *asked = &items[k].requestedParameters;
                if (tdot_ua_revised(asked->samplingInterval,
                                    mres->revisedSamplingInterval))
                    fprintf(stderr,
                            "info  device %s: point %s: the server revised the point's "
                            "sampling interval: requested %.0fms, revised %.0fms\n",
                            dev->name, pt->id, asked->samplingInterval,
                            mres->revisedSamplingInterval);
                if (mres->revisedQueueSize != asked->queueSize)
                    fprintf(stderr,
                            "info  device %s: point %s: the server revised the point's "
                            "queue size: requested %u, revised %u\n",
                            dev->name, pt->id, (unsigned)asked->queueSize,
                            (unsigned)mres->revisedQueueSize);
            }
        }
        /* The response's results can carry heap-allocated filter results. */
        UA_CreateMonitoredItemsResponse_clear(&resp);
        for (size_t k = 0; k < n; k++)
            UA_MonitoredItemCreateRequest_clear(&items[k]);
    }
    free(items);
    free(contexts);
    free(callbacks);
    ua->retry_at = mono_s() + UA_RETRY_INTERVAL_S;
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

    retry_pending(dev);
    if (ua->renew_session)
        return -1; /* reconnect: see ua_device_t.renew_session */

    ua_pending_t *slot;
    while ((slot = ua_ring_peek(&ua->queue))) {
        sink(sink_ctx, dev, slot->pt, &slot->sample);
        ua_ring_pop(&ua->queue);
    }
    if (ua->queue.nomem) {
        fprintf(stderr,
                "warn  device %s: dropped %lu pushed sample(s); out of memory "
                "growing the queue beyond %zu entries\n",
                dev->name, ua->queue.nomem, ua->queue.cap);
    }
    if (ua->queue.dropped > ua->queue.nomem) {
        fprintf(stderr,
                "warn  device %s: dropped %lu pushed sample(s); the queue "
                "reached its bound of %zu between two runtime ticks\n",
                dev->name, ua->queue.dropped - ua->queue.nomem, ua->queue.bound);
    }
    ua->queue.dropped = ua->queue.nomem = 0;
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
    c->read_points = read_points;
    /* Each device has its own client (dev->proto); the read path touches
     * nothing the devices share (contract §5.6). */
    c->concurrent_reads = true;
    c->endpoint = endpoint;
    c->read_wanted = read_wanted;
    c->read_points_inflight = read_points_inflight;
    c->write_point = write_point;
    c->call_method = call_method;
    c->subscribe_device = subscribe_device;
    c->drain_subscriptions = drain_subscriptions;
    c->disconnect_device = disconnect_device;
    c->device_info = device_info;
    c->destroy = destroy;
    return c;
}
