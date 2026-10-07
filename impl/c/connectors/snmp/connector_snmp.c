/* tedge-dot — SNMP connector (doc/connectors/snmp-connector-spec.md), on
 * net-snmp's libnetsnmp. Mirrors impl/rust/crates/connector-snmp:
 *
 *  - object points are polled: a device's due points are fetched in batches of
 *    max_varbinds, one GETBULK (non-repeaters = N) or GET each (§5.1), and
 *    written with SET (§5.2);
 *  - trap and varbind points are pushed from notifications -- v1/v2c/v3 traps
 *    and informs -- received on one UDP socket for the whole connector, routed
 *    by source address or through a trusted forwarder, checked against the
 *    device's community or USM user, acknowledged when an inform (§4).
 *
 * All net-snmp calls go through snmp_netsnmp.c, which serialises them.
 *
 * The runtime asks read_point() one point at a time, so the first call of a
 * tick fetches every due object point of the device in batches and caches the
 * samples; the calls for the rest of the tick are served from that result.
 * Notifications are received on the runtime thread (§10): each
 * drain_subscriptions() reads every pending datagram, queues each on the
 * device it routes to, and hands the drained device its queue.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "snmp_netsnmp.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/decode.h"
#include "tedge_dot/runtime.h"

#define SNMPC_QUEUE_LEN 256       /* notifications per device (§10) */
#define SNMPC_RX_MAX 65536        /* largest UDP payload, 65507, fits */
#define SNMPC_RX_BURST 4096       /* datagrams read per drain */
#define SNMPC_LOG_INTERVAL_S 60.0 /* rate limit of the per-source/device logs */
#define SNMPC_SOURCE_LOG_LEN 64
#define SNMPC_BATCH_FRESH_S 1.0   /* how long a fetched batch serves read_point */

/* SNMPv3 password files are read at configure: a management command must not
 * point them elsewhere. Mirrors SnmpConnector::local_only_settings (Rust). */
static const char *const LOCAL_ONLY_SETTINGS[] = {"auth_password_file", "priv_password_file",
                                                  NULL};

static const char CAPABILITIES[] =
    "{\"protocol\":\"snmp\",\"version\":\"" TDOT_VERSION "\","
    "\"modes\":[\"raw\",\"typed\"],"
    "\"datatypes\":[\"bool\",\"int8\",\"uint8\",\"int16\",\"uint16\","
    "\"int32\",\"uint32\",\"int64\",\"uint64\",\"float32\",\"float64\","
    "\"string\"],"
    "\"point_kinds\":[\"object\",\"trap\",\"varbind\"],"
    "\"command_verbs\":[\"write\"],"
    "\"features\":[\"polling\",\"subscribe\",\"bulk_read\",\"snmpv3\"],"
    "\"subscribe\":true}";

typedef struct {
    int family; /* AF_INET or AF_INET6 */
    uint8_t bytes[16];
} snmpc_ip_t;

typedef enum { PT_OBJECT = 0, PT_TRAP, PT_VARBIND } snmpc_kind_t;

/* Per-point parsed address (pt->proto, flat, freed by the config). */
typedef struct {
    snmpc_kind_t kind;
    tsnmp_oid_t oid;
    bool has_set_type;
    tsnmp_type_t set_type;
    /* the sample a batch fetched for this point, until read_point serves it */
    bool cached;
    int cached_rc;
    tdot_sample_t cached_sample;
    size_t ntraps;
    tsnmp_oid_t traps[];
} snmpc_point_t;

/* A notification queued on its device: decoded and authenticated already. */
typedef struct {
    netsnmp_pdu *pdu;
    char source[INET6_ADDRSTRLEN];
    char forwarder[INET6_ADDRSTRLEN]; /* "" when not forwarded */
} snmpc_queued_t;

/* Per-device state (dev->proto). Flat, so the config frees it; the session and
 * the queued notifications are released by disconnect_device, which the
 * runtime always calls before freeing the config. */
typedef struct {
    char host[256];
    int port;
    tsnmp_version_t version;
    char community[256];       /* requests */
    char write_community[256]; /* SET */
    bool has_v3;
    tsnmp_v3_creds_t v3;
    /* A v3 protocol this build cannot do (SHA-2 authentication, AES-192/256
     * privacy: they need a real OpenSSL, §10). The configuration still loads --
     * one such device must not take a whole gateway's config down -- and the
     * device stays `disconnected` with this named in the reason. */
    char unsupported[32];
    bool engine_known; /* configured, learned from a request or a trap */
    bool engine_configured;
    uint8_t engine[32];
    size_t engine_len;
    double timeout_s;
    int retries;
    int max_varbinds;
    bool bulk;
    bool has_notify;  /* trap/varbind points */
    bool has_objects; /* object points */
    size_t naccept;   /* accepted communities in data[]; 0: the connection's */

    bool connected;
    bool listening; /* holds a reference on the listener */
    snmpc_ip_t addr;
    void *sess;
    snmpc_queued_t *queue[SNMPC_QUEUE_LEN];
    size_t head, count;
    unsigned long dropped;
    double auth_warned_at;
    double batch_at;
    /* Points of the last batch still holding a transport verdict to report;
     * see read_point. */
    size_t transport_pending;
    char data[]; /* accepted communities, NUL-separated */
} snmpc_device_t;

typedef struct {
    bool used;
    snmpc_ip_t ip;
    double seen_at, undecodable_at, unknown_at;
} snmpc_source_log_t;

typedef struct {
    char listen[128];
    char listen_host[96];
    char listen_port[8];
    char **communities;
    size_t ncommunities;
    char **forwarders;
    size_t nforwarders;
    snmpc_ip_t *forwarder_ips;
    size_t nforwarder_ips;
    uint8_t engine[32];
    size_t engine_len;
    double timeout_s;
    int retries;
    int max_varbinds;
    bool debug;

    int fd;           /* -1 while no device with notification points is up */
    size_t listeners; /* connected devices with notification points */
    tdot_device_t **devices; /* connected devices, in connect order */
    size_t ndevices, capdevices;
    uint8_t *rx;
    snmpc_source_log_t sources[SNMPC_SOURCE_LOG_LEN];
} snmpc_state_t;

static void snmpc_log(const char *level, const char *fmt, ...) {
    char ts[40];
    tdot_now_rfc3339(ts, sizeof ts);
    fprintf(stderr, "%s %-5s ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- addresses ----------------------------------------------------------- */

static void ip_from_sockaddr(const struct sockaddr *sa, snmpc_ip_t *ip) {
    memset(ip, 0, sizeof *ip);
    if (sa->sa_family == AF_INET) {
        ip->family = AF_INET;
        memcpy(ip->bytes, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        return;
    }
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
    static const uint8_t MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(&in6->sin6_addr, MAPPED, 12) == 0) {
        ip->family = AF_INET; /* compared as IPv4 (§4.1) */
        memcpy(ip->bytes, (const uint8_t *)&in6->sin6_addr + 12, 4);
        return;
    }
    ip->family = AF_INET6;
    memcpy(ip->bytes, &in6->sin6_addr, 16);
}

static bool ip_equal(const snmpc_ip_t *a, const snmpc_ip_t *b) {
    return a->family == b->family &&
           memcmp(a->bytes, b->bytes, a->family == AF_INET ? 4 : 16) == 0;
}

static void ip_format(const snmpc_ip_t *ip, char *dst, size_t cap) {
    if (!inet_ntop(ip->family, ip->bytes, dst, (socklen_t)cap))
        snprintf(dst, cap, "?");
}

/* First address of a host name or literal. 0, or -1 with err. */
static int resolve(const char *host, snmpc_ip_t *ip, char *err, size_t errlen) {
    struct addrinfo hints = {.ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, errlen, "cannot resolve host '%s': %s", host,
                 rc ? gai_strerror(rc) : "no address");
        return -1;
    }
    ip_from_sockaddr(res->ai_addr, ip);
    freeaddrinfo(res);
    return 0;
}

static int parse_listen(snmpc_state_t *st, const char *text, char *err,
                        size_t errlen) {
    snprintf(st->listen, sizeof st->listen, "%s", text);
    const char *colon;
    size_t hostlen;
    const char *host = text;
    bool v6 = text[0] == '[';
    if (v6) {
        const char *close = strchr(text, ']');
        if (!close || close[1] != ':')
            goto bad;
        host = text + 1;
        hostlen = (size_t)(close - host);
        colon = close + 1;
    } else {
        colon = strrchr(text, ':');
        if (!colon)
            goto bad;
        hostlen = (size_t)(colon - text);
    }
    if (hostlen == 0 || hostlen >= sizeof st->listen_host)
        goto bad;
    memcpy(st->listen_host, host, hostlen);
    st->listen_host[hostlen] = '\0';
    const char *port = colon + 1;
    size_t plen = strlen(port);
    if (plen == 0 || plen > 5 || strspn(port, "0123456789") != plen ||
        atol(port) > 65535)
        goto bad;
    snprintf(st->listen_port, sizeof st->listen_port, "%s", port);
    unsigned char probe[16];
    if (v6) {
        char bare[96];
        snprintf(bare, sizeof bare, "%s", st->listen_host);
        char *pct = strchr(bare, '%');
        if (pct)
            *pct = '\0';
        if (inet_pton(AF_INET6, bare, probe) != 1)
            goto bad;
    } else if (inet_pton(AF_INET, st->listen_host, probe) != 1) {
        goto bad;
    }
    return 0;
bad:
    snprintf(err, errlen,
             "[connection] listen must be <IPv4>:<port> or [<IPv6>]:<port> "
             "(got '%s')",
             text);
    return -1;
}

/* ---- configuration (§3) ------------------------------------------------- */

static int check_keys(const toml_table_t *tab, const char *const *allowed,
                      const char *where, char *err, size_t errlen) {
    for (int i = 0;; i++) {
        const char *key = toml_key_in(tab, i);
        if (!key)
            return 0;
        bool known = false;
        for (const char *const *a = allowed; *a && !known; a++)
            known = strcmp(*a, key) == 0;
        if (!known) {
            snprintf(err, errlen, "%s: unknown key '%s'", where, key);
            return -1;
        }
    }
}

static void free_strings(char **list, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

/* A key holding a string or a non-empty list of strings. Returns the count
 * (0 when absent) with *out a heap array of heap strings, or -1 with err. */
static int string_or_list(const toml_table_t *tab, const char *key,
                          const char *where, bool allow_empty, char ***out,
                          char *err, size_t errlen) {
    *out = NULL;
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_string_in(tab, key);
    if (d.ok) {
        *out = malloc(sizeof **out);
        if (!*out) {
            free(d.u.s);
            snprintf(err, errlen, "%s: out of memory reading %s", where, key);
            return -1;
        }
        (*out)[0] = d.u.s;
        return 1;
    }
    toml_array_t *arr = toml_array_in(tab, key);
    int n = arr ? toml_array_nelem(arr) : -1;
    if (n == 0) {
        if (allow_empty)
            return 0;
        snprintf(err, errlen, "%s: %s must not be an empty list", where, key);
        return -1;
    }
    char **list = n > 0 ? calloc((size_t)n, sizeof *list) : NULL;
    for (int i = 0; list && i < n; i++) {
        d = toml_string_at(arr, i);
        if (!d.ok) {
            free_strings(list, (size_t)i);
            list = NULL;
            break;
        }
        list[i] = d.u.s;
    }
    if (!list) {
        snprintf(err, errlen, "%s: %s must be a string or a list of strings",
                 where, key);
        return -1;
    }
    *out = list;
    return n;
}

/* An optional string key, copied into dst. 1 present, 0 absent, -1 error. */
static int opt_string(const toml_table_t *tab, const char *key,
                      const char *where, char *dst, size_t cap, char *err,
                      size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok) {
        snprintf(err, errlen, "%s: %s must be a string", where, key);
        return -1;
    }
    size_t n = strlen(d.u.s);
    if (n >= cap) {
        snprintf(err, errlen, "%s: %s is longer than %zu characters", where,
                 key, cap - 1);
        free(d.u.s);
        return -1;
    }
    memcpy(dst, d.u.s, n + 1);
    free(d.u.s);
    return 1;
}

static int opt_duration(const toml_table_t *tab, const char *key,
                        const char *where, double *out, char *err,
                        size_t errlen) {
    char text[64];
    int rc = opt_string(tab, key, where, text, sizeof text, err, errlen);
    if (rc <= 0) {
        if (rc < 0)
            snprintf(err, errlen, "%s: %s must be a duration such as \"2s\"",
                     where, key);
        return rc;
    }
    double s = tdot_duration_parse(text);
    if (s <= 0) {
        snprintf(err, errlen,
                 "%s: %s must be a positive duration such as \"2s\" (got '%s')",
                 where, key, text);
        return -1;
    }
    *out = s;
    return 1;
}

static int opt_int(const toml_table_t *tab, const char *key, const char *where,
                   long lo, long hi, int *out, char *err, size_t errlen) {
    if (!toml_key_exists(tab, key))
        return 0;
    toml_datum_t d = toml_int_in(tab, key);
    if (!d.ok || d.u.i < lo || d.u.i > hi) {
        snprintf(err, errlen, "%s: %s must be an integer from %ld to %ld",
                 where, key, lo, hi);
        return -1;
    }
    *out = (int)d.u.i;
    return 1;
}

/* Hex engine ID, 5-32 octets. 1 present, 0 absent, -1 error. */
static int opt_engine_id(const toml_table_t *tab, const char *where,
                         uint8_t *out, size_t *len, char *err, size_t errlen) {
    char text[80];
    int rc = opt_string(tab, "engine_id", where, text, sizeof text, err, errlen);
    if (rc <= 0) {
        if (rc < 0)
            snprintf(err, errlen, "%s: engine_id must be 5-32 octets of hex",
                     where);
        return rc;
    }
    const char *p = text;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    size_t n = strlen(p);
    int got = (n % 2 == 0 && strspn(p, "0123456789abcdefABCDEF") == n)
                  ? tdot_hex_parse(p, out, 32)
                  : -1;
    if (got < 5 || got > 32) {
        snprintf(err, errlen, "%s: engine_id must be 5-32 octets of hex", where);
        return -1;
    }
    *len = (size_t)got;
    return 1;
}

/* `key` or `key_file` (§3.2): exactly one. The file's first line, trailing
 * newline stripped. The secret itself never reaches an error message. */
static int opt_secret(const toml_table_t *tab, const char *key,
                      const char *where, char *out, size_t cap, bool *present,
                      char *err, size_t errlen) {
    char file_key[64];
    snprintf(file_key, sizeof file_key, "%s_file", key);
    bool direct = toml_key_exists(tab, key);
    bool file = toml_key_exists(tab, file_key);
    *present = direct || file;
    out[0] = '\0';
    if (direct && file) {
        snprintf(err, errlen, "%s: set %s or %s, not both", where, key,
                 file_key);
        return -1;
    }
    if (direct) {
        toml_datum_t d = toml_string_in(tab, key);
        if (!d.ok || strlen(d.u.s) >= cap) {
            snprintf(err, errlen, "%s: %s must be a string of at most %zu "
                                  "characters",
                     where, key, cap - 1);
            if (d.ok)
                free(d.u.s);
            return -1;
        }
        snprintf(out, cap, "%s", d.u.s);
        memset(d.u.s, 0, strlen(d.u.s));
        free(d.u.s);
    } else if (file) {
        char path[512];
        if (opt_string(tab, file_key, where, path, sizeof path, err, errlen) < 0)
            return -1;
        FILE *fp = fopen(path, "r");
        if (!fp) {
            snprintf(err, errlen, "%s: cannot read %s '%s': %s", where,
                     file_key, path, strerror(errno));
            return -1;
        }
        if (!fgets(out, (int)cap, fp))
            out[0] = '\0';
        fclose(fp);
        out[strcspn(out, "\r\n")] = '\0';
    }
    if (*present && strlen(out) < 8) {
        snprintf(err, errlen, "%s: %s must be at least 8 characters", where,
                 key);
        return -1;
    }
    return 0;
}

static int parse_v3(const toml_table_t *v3, const char *where,
                    snmpc_device_t *sd, char *err, size_t errlen) {
    static const char *const KEYS[] = {
        "user", "level", "auth_protocol", "auth_password", "auth_password_file",
        "priv_protocol", "priv_password", "priv_password_file", "context",
        "engine_id", NULL};
    if (check_keys(v3, KEYS, where, err, errlen) != 0)
        return -1;
    tsnmp_v3_creds_t *c = &sd->v3;
    memset(c, 0, sizeof *c);
    int rc = opt_string(v3, "user", where, c->user, sizeof c->user, err, errlen);
    if (rc < 0)
        return -1;
    if (rc == 0 || c->user[0] == '\0') {
        snprintf(err, errlen, "%s: user required", where);
        return -1;
    }

    char proto[32];
    rc = opt_string(v3, "auth_protocol", where, proto, sizeof proto, err, errlen);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        if (strcmp(proto, "MD5") == 0)
            c->auth = TSNMP_AUTH_MD5;
        else if (strcmp(proto, "SHA") == 0)
            c->auth = TSNMP_AUTH_SHA;
        else if (strcmp(proto, "SHA224") == 0 || strcmp(proto, "SHA256") == 0 ||
                 strcmp(proto, "SHA384") == 0 || strcmp(proto, "SHA512") == 0) {
            snprintf(sd->unsupported, sizeof sd->unsupported, "%s", proto);
        } else {
            snprintf(err, errlen,
                     "%s: auth_protocol must be one of \"MD5\", \"SHA\" (got '%s')",
                     where, proto);
            return -1;
        }
    }
    rc = opt_string(v3, "priv_protocol", where, proto, sizeof proto, err, errlen);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        if (strcmp(proto, "DES") == 0)
            c->priv = TSNMP_PRIV_DES;
        else if (strcmp(proto, "AES") == 0)
            c->priv = TSNMP_PRIV_AES;
        else if (strcmp(proto, "AES192") == 0 || strcmp(proto, "AES256") == 0) {
            if (!sd->unsupported[0])
                snprintf(sd->unsupported, sizeof sd->unsupported, "%s", proto);
        } else {
            snprintf(err, errlen,
                     "%s: priv_protocol must be one of \"DES\", \"AES\" (got '%s')",
                     where, proto);
            return -1;
        }
    }

    char auth_pw[256], priv_pw[256];
    bool has_auth_pw, has_priv_pw;
    int result = -1;
    if (opt_secret(v3, "auth_password", where, auth_pw, sizeof auth_pw,
                   &has_auth_pw, err, errlen) != 0 ||
        opt_secret(v3, "priv_password", where, priv_pw, sizeof priv_pw,
                   &has_priv_pw, err, errlen) != 0)
        goto out;
    if (sd->unsupported[0]) {
        /* §10: a protocol this build cannot do is not a load error -- one such
         * device must not take the whole configuration down. It loads, and
         * connect_device leaves it disconnected naming the capability. */
        c->auth = TSNMP_AUTH_NONE;
        c->priv = TSNMP_PRIV_NONE;
        c->level = SNMP_SEC_LEVEL_NOAUTH;
        result = 0;
        goto out;
    }
    if (has_auth_pw != (c->auth != TSNMP_AUTH_NONE)) {
        snprintf(err, errlen,
                 has_auth_pw ? "%s: auth_protocol required with an auth password"
                             : "%s: auth_password or auth_password_file required "
                               "with auth_protocol",
                 where);
        goto out;
    }
    if (has_priv_pw != (c->priv != TSNMP_PRIV_NONE)) {
        snprintf(err, errlen,
                 has_priv_pw ? "%s: priv_protocol required with a priv password"
                             : "%s: priv_password or priv_password_file required "
                               "with priv_protocol",
                 where);
        goto out;
    }
    if (c->priv != TSNMP_PRIV_NONE && c->auth == TSNMP_AUTH_NONE) {
        snprintf(err, errlen, "%s: privacy needs authentication (auth_protocol)",
                 where);
        goto out;
    }
    int derived = c->priv ? SNMP_SEC_LEVEL_AUTHPRIV
                  : c->auth ? SNMP_SEC_LEVEL_AUTHNOPRIV
                            : SNMP_SEC_LEVEL_NOAUTH;
    c->level = derived;
    char level[32];
    rc = opt_string(v3, "level", where, level, sizeof level, err, errlen);
    if (rc < 0)
        goto out;
    if (rc > 0) {
        int want = strcmp(level, "noAuthNoPriv") == 0 ? SNMP_SEC_LEVEL_NOAUTH
                   : strcmp(level, "authNoPriv") == 0 ? SNMP_SEC_LEVEL_AUTHNOPRIV
                   : strcmp(level, "authPriv") == 0   ? SNMP_SEC_LEVEL_AUTHPRIV
                                                      : 0;
        if (!want) {
            snprintf(err, errlen,
                     "%s: level must be one of \"noAuthNoPriv\", \"authNoPriv\", "
                     "\"authPriv\" (got '%s')",
                     where, level);
            goto out;
        }
        if (want > derived) {
            snprintf(err, errlen,
                     "%s: level \"%s\" needs the %s protocol and password", where,
                     level, want == SNMP_SEC_LEVEL_AUTHPRIV && c->auth ? "priv"
                                                                       : "auth");
            goto out;
        }
        c->level = want;
    }
    if (opt_string(v3, "context", where, c->context, sizeof c->context, err,
                   errlen) < 0)
        goto out;
    rc = opt_engine_id(v3, where, sd->engine, &sd->engine_len, err, errlen);
    if (rc < 0)
        goto out;
    sd->engine_configured = sd->engine_known = rc > 0;

    tsnmp_lock();
    rc = tsnmp_v3_creds_derive(c, auth_pw, priv_pw, err, errlen);
    tsnmp_unlock();
    if (rc == 0)
        result = 0;
out:
    memset(auth_pw, 0, sizeof auth_pw);
    memset(priv_pw, 0, sizeof priv_pw);
    return result;
}

static bool hosts_same(const char *a, const char *b) {
    unsigned char x[16], y[16];
    if (inet_pton(AF_INET, a, x) == 1 && inet_pton(AF_INET, b, y) == 1)
        return memcmp(x, y, 4) == 0;
    if (inet_pton(AF_INET6, a, x) == 1 && inet_pton(AF_INET6, b, y) == 1)
        return memcmp(x, y, 16) == 0;
    return strcasecmp(a, b) == 0;
}

static const char *device_community(const snmpc_device_t *sd, size_t i) {
    const char *p = sd->data;
    while (i--)
        p += strlen(p) + 1;
    return p;
}

static bool default_set_type(tdot_datatype_t dt, tsnmp_type_t *t) {
    switch (dt) {
    case TDOT_DT_BOOL:
    case TDOT_DT_INT8:
    case TDOT_DT_INT16:
    case TDOT_DT_INT32:
    case TDOT_DT_INT64:
        *t = TSNMP_TYPE_INTEGER;
        return true;
    case TDOT_DT_UINT8:
    case TDOT_DT_UINT16:
    case TDOT_DT_UINT32:
        *t = TSNMP_TYPE_GAUGE32; /* unsigned32 */
        return true;
    case TDOT_DT_UINT64:
        *t = TSNMP_TYPE_COUNTER64;
        return true;
    case TDOT_DT_STRING:
        *t = TSNMP_TYPE_OCTET_STRING;
        return true;
    default:
        return false; /* floats and raw points state `type` */
    }
}

static int configure_point(tdot_device_t *dev, tdot_point_t *pt, char *err,
                           size_t errlen) {
    char where[256];
    snprintf(where, sizeof where, "point %s/%s: address", dev->name, pt->id);
    if (!pt->address) {
        snprintf(err, errlen, "point %s/%s: address requires oid or trap",
                 dev->name, pt->id);
        return -1;
    }
    static const char *const KEYS[] = {"oid", "trap", "type", NULL};
    if (check_keys(pt->address, KEYS, where, err, errlen) != 0)
        return -1;

    char **traps = NULL;
    int ntraps = string_or_list(pt->address, "trap", where, false, &traps, err,
                                errlen);
    if (ntraps < 0)
        return -1;
    char oid_text[TSNMP_OID_STR_MAX], type_text[32];
    int has_oid = opt_string(pt->address, "oid", where, oid_text,
                             sizeof oid_text, err, errlen);
    int has_type = has_oid < 0 ? -1
                               : opt_string(pt->address, "type", where,
                                            type_text, sizeof type_text, err,
                                            errlen);
    int rc = -1;
    snmpc_point_t *sp = NULL;
    cJSON *addr = NULL;
    if (has_oid < 0 || has_type < 0)
        goto out;
    /* As the Rust module: an OCTET STRING is already `string` or a raw run. */
    if (pt->datatype == TDOT_DT_BYTES) {
        snprintf(err, errlen,
                 "point %s/%s: datatype \"bytes\" is not supported; use mode = \"raw\"",
                 dev->name, pt->id);
        goto out;
    }
    if (ntraps == 0 && !has_oid) {
        snprintf(err, errlen, "point %s/%s: address requires oid or trap",
                 dev->name, pt->id);
        goto out;
    }
    snmpc_kind_t kind = ntraps == 0 ? PT_OBJECT : has_oid ? PT_VARBIND : PT_TRAP;
    /* The loader maps "bytes" (and a missing datatype) to none. */
    if (pt->mode == TDOT_MODE_TYPED && pt->datatype == TDOT_DT_NONE) {
        snprintf(err, errlen,
                 "point %s/%s: datatype \"bytes\" is not supported, and a typed "
                 "point needs a datatype (use mode = \"raw\" for binary values)",
                 dev->name, pt->id);
        goto out;
    }

    tsnmp_type_t set_type = TSNMP_TYPE_INTEGER;
    bool has_set_type = false;
    if (kind != PT_OBJECT) {
        if (has_type) {
            snprintf(err, errlen, "%s: type applies to object points only (an "
                                  "address without trap)",
                     where);
            goto out;
        }
        if (pt->access != TDOT_ACCESS_READ) {
            snprintf(err, errlen,
                     "point %s/%s: a %s point must have access = \"read\" (a "
                     "notification cannot be written back)",
                     dev->name, pt->id, kind == PT_TRAP ? "trap" : "varbind");
            goto out;
        }
        if (!pt->subscribe) {
            snprintf(err, errlen,
                     "point %s/%s: subscribe = false is not allowed on a %s point "
                     "(notifications cannot be polled)",
                     dev->name, pt->id, kind == PT_TRAP ? "trap" : "varbind");
            goto out;
        }
        if (kind == PT_TRAP && pt->mode == TDOT_MODE_TYPED &&
            pt->datatype != TDOT_DT_STRING && pt->datatype != TDOT_DT_BOOL) {
            snprintf(err, errlen,
                     "point %s/%s: a trap point is typed string or bool (got %s)",
                     dev->name, pt->id, tdot_datatype_str(pt->datatype));
            goto out;
        }
    } else {
        if (has_type) {
            if (tsnmp_set_type_parse(type_text, &set_type) != 0) {
                snprintf(err, errlen,
                         "%s: type must be one of \"integer\", \"unsigned32\", "
                         "\"gauge32\", \"counter32\", \"counter64\", "
                         "\"timeticks\", \"octet_string\", \"ip_address\", "
                         "\"oid\" (got '%s')",
                         where, type_text);
                goto out;
            }
            has_set_type = true;
        } else if (pt->access & TDOT_ACCESS_WRITE) {
            has_set_type = pt->mode == TDOT_MODE_TYPED &&
                           default_set_type(pt->datatype, &set_type);
            if (!has_set_type) {
                snprintf(err, errlen,
                         "point %s/%s: a writable %s point needs address.type "
                         "(the SNMP type to SET)",
                         dev->name, pt->id,
                         pt->mode == TDOT_MODE_RAW
                             ? "raw"
                             : tdot_datatype_str(pt->datatype));
                goto out;
            }
        }
    }

    sp = calloc(1, sizeof *sp + (size_t)ntraps * sizeof sp->traps[0]);
    if (!sp) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    pt->proto = sp;
    sp->kind = kind;
    sp->has_set_type = has_set_type;
    sp->set_type = set_type;
    sp->ntraps = (size_t)ntraps;
    char why[200], text[TSNMP_OID_STR_MAX];
    addr = cJSON_CreateObject();
    cJSON *trap_list = ntraps ? cJSON_AddArrayToObject(addr, "trap") : NULL;
    for (int i = 0; i < ntraps; i++) {
        if (tsnmp_oid_parse(traps[i], &sp->traps[i], why, sizeof why) != 0) {
            snprintf(err, errlen, "%s: trap: %s", where, why);
            goto out;
        }
        tsnmp_oid_format(&sp->traps[i], text, sizeof text);
        cJSON_AddItemToArray(trap_list, cJSON_CreateString(text));
    }
    if (has_oid) {
        if (tsnmp_oid_parse(oid_text, &sp->oid, why, sizeof why) != 0) {
            snprintf(err, errlen, "%s: oid: %s", where, why);
            goto out;
        }
        tsnmp_oid_format(&sp->oid, text, sizeof text);
        cJSON_AddStringToObject(addr, "oid", text);
    }
    /* object points' sample addr (§6); the static echo for trap/varbind points
     * on paths without a notification (a received sample carries its own) */
    pt->addr_json = cJSON_PrintUnformatted(addr);
    rc = 0;
out:
    cJSON_Delete(addr);
    free_strings(traps, ntraps > 0 ? (size_t)ntraps : 0);
    return rc;
}

static int configure_device(snmpc_state_t *st, tdot_config_t *cfg, size_t i,
                            char *err, size_t errlen) {
    tdot_device_t *dev = &cfg->devices[i];
    toml_table_t *pa = dev->protocol_address;
    char where[160];
    snprintf(where, sizeof where, "device %s: protocol_address", dev->name);
    if (!pa) {
        snprintf(err, errlen, "%s: host required", where);
        return -1;
    }
    static const char *const KEYS[] = {
        "host",    "port",         "version", "community",  "write_community",
        "v3",      "request_timeout", "retries", "max_varbinds", "bulk", NULL};
    if (check_keys(pa, KEYS, where, err, errlen) != 0)
        return -1;

    char **accept = NULL;
    int naccept = string_or_list(pa, "community", where, false, &accept, err,
                                 errlen);
    if (naccept < 0)
        return -1;
    size_t datalen = 1;
    for (int k = 0; k < naccept; k++)
        datalen += strlen(accept[k]) + 1;
    snmpc_device_t *sd = calloc(1, sizeof *sd + datalen);
    if (!sd) {
        free_strings(accept, (size_t)naccept);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    dev->proto = sd;
    char *p = sd->data;
    for (int k = 0; k < naccept; k++)
        p = stpcpy(p, accept[k]) + 1;
    sd->naccept = (size_t)naccept;
    snprintf(sd->community, sizeof sd->community, "%s",
             naccept > 0 ? accept[0] : "public");
    free_strings(accept, (size_t)naccept);
    sd->auth_warned_at = -1e18;
    sd->port = 161;
    sd->version = TSNMP_V2C;
    sd->timeout_s = st->timeout_s;
    sd->retries = st->retries;
    sd->max_varbinds = st->max_varbinds;
    sd->bulk = true;

    int rc = opt_string(pa, "host", where, sd->host, sizeof sd->host, err, errlen);
    if (rc < 0)
        return -1;
    if (rc == 0 || sd->host[0] == '\0') {
        snprintf(err, errlen, "%s: host required (a non-empty string)", where);
        return -1;
    }
    for (size_t k = 0; k < i; k++) {
        const snmpc_device_t *other = cfg->devices[k].proto;
        if (other && hosts_same(other->host, sd->host)) {
            snprintf(err, errlen, "%s: host '%s' is already used by device %s",
                     where, sd->host, cfg->devices[k].name);
            return -1;
        }
    }
    if (opt_int(pa, "port", where, 1, 65535, &sd->port, err, errlen) < 0)
        return -1;
    char version[16];
    rc = opt_string(pa, "version", where, version, sizeof version, err, errlen);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        if (strcmp(version, "v1") == 0)
            sd->version = TSNMP_V1;
        else if (strcmp(version, "v2c") == 0)
            sd->version = TSNMP_V2C;
        else if (strcmp(version, "v3") == 0)
            sd->version = TSNMP_V3;
        else {
            snprintf(err, errlen,
                     "%s: version must be one of \"v1\", \"v2c\", \"v3\" (got '%s')",
                     where, version);
            return -1;
        }
    }
    rc = opt_string(pa, "write_community", where, sd->write_community,
                    sizeof sd->write_community, err, errlen);
    if (rc < 0)
        return -1;
    if (rc == 0)
        snprintf(sd->write_community, sizeof sd->write_community, "%s",
                 sd->community);
    if (opt_duration(pa, "request_timeout", where, &sd->timeout_s, err, errlen) < 0 ||
        opt_int(pa, "retries", where, 0, 100, &sd->retries, err, errlen) < 0 ||
        opt_int(pa, "max_varbinds", where, 1, TSNMP_MAX_VARBINDS,
                &sd->max_varbinds, err, errlen) < 0)
        return -1;
    if (toml_key_exists(pa, "bulk")) {
        toml_datum_t d = toml_bool_in(pa, "bulk");
        if (!d.ok) {
            snprintf(err, errlen, "%s: bulk must be true or false", where);
            return -1;
        }
        sd->bulk = d.u.b;
    }
    if (toml_key_exists(pa, "v3")) {
        toml_table_t *v3 = toml_table_in(pa, "v3");
        char v3where[200];
        snprintf(v3where, sizeof v3where, "%s.v3", where);
        if (!v3) {
            snprintf(err, errlen, "%s: v3 must be a table", where);
            return -1;
        }
        if (parse_v3(v3, v3where, sd, err, errlen) != 0)
            return -1;
        sd->has_v3 = true;
    } else if (sd->version == TSNMP_V3) {
        snprintf(err, errlen, "%s: version \"v3\" needs a v3 table (user, ...)",
                 where);
        return -1;
    }

    for (size_t j = 0; j < dev->npoints; j++) {
        if (configure_point(dev, &dev->points[j], err, errlen) != 0)
            return -1;
        const snmpc_point_t *sp = dev->points[j].proto;
        if (sp->kind == PT_OBJECT)
            sd->has_objects = true;
        else
            sd->has_notify = true;
    }
    return 0;
}

/* snmpEngineBoots for this run (§4.1). Without a file to count restarts in the
 * wall clock stands in: it grows with every restart, which is what a sender
 * that cached our boots needs (RFC 3414 §2.2), and 0 -- "time not yet
 * initialised" -- is excluded. Seconds since 2020-01-01, so it stays well
 * inside 2^31 - 1; the Rust connector derives it the same way. */
static uint32_t local_engine_boots(void) {
    const time_t EPOCH_2020 = 1577836800;
    time_t now = time(NULL);
    if (now <= EPOCH_2020)
        return 1;
    long long boots = (long long)(now - EPOCH_2020);
    if (boots > 2147483646LL)
        boots = 2147483646LL;
    return (uint32_t)boots;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                     size_t errlen) {
    snmpc_state_t *st = self->state;
    free_strings(st->communities, st->ncommunities);
    free_strings(st->forwarders, st->nforwarders);
    st->communities = st->forwarders = NULL;
    st->ncommunities = st->nforwarders = 0;
    st->debug = cfg->log_level && (strcmp(cfg->log_level, "debug") == 0 ||
                                   strcmp(cfg->log_level, "trace") == 0);
    st->timeout_s = 2.0;
    st->retries = 1;
    st->max_varbinds = 20;
    memset(st->sources, 0, sizeof st->sources);

    char listen[128] = "0.0.0.0:162";
    bool engine_set = false;
    if (cfg->connection) {
        const toml_table_t *c = cfg->connection;
        const char *where = "[connection]";
        static const char *const KEYS[] = {"listen",  "community",
                                           "forwarders", "engine_id",
                                           "request_timeout", "retries",
                                           "max_varbinds", NULL};
        if (check_keys(c, KEYS, where, err, errlen) != 0)
            return -1;
        if (opt_string(c, "listen", where, listen, sizeof listen, err, errlen) < 0)
            return -1;
        int n = string_or_list(c, "community", where, false, &st->communities,
                               err, errlen);
        if (n < 0)
            return -1;
        st->ncommunities = (size_t)n;
        if (toml_key_exists(c, "forwarders") && !toml_array_in(c, "forwarders")) {
            snprintf(err, errlen, "%s: forwarders must be a list of addresses",
                     where);
            return -1;
        }
        n = string_or_list(c, "forwarders", where, true, &st->forwarders, err,
                           errlen);
        if (n < 0)
            return -1;
        st->nforwarders = (size_t)n;
        int rc = opt_engine_id(c, where, st->engine, &st->engine_len, err, errlen);
        if (rc < 0)
            return -1;
        engine_set = rc > 0;
        if (opt_duration(c, "request_timeout", where, &st->timeout_s, err,
                         errlen) < 0 ||
            opt_int(c, "retries", where, 0, 100, &st->retries, err, errlen) < 0 ||
            opt_int(c, "max_varbinds", where, 1, TSNMP_MAX_VARBINDS,
                    &st->max_varbinds, err, errlen) < 0)
            return -1;
    }
    if (parse_listen(st, listen, err, errlen) != 0)
        return -1;
    if (!engine_set)
        st->engine_len = tsnmp_default_engine_id(st->engine);

    for (size_t i = 0; i < cfg->ndevices; i++)
        if (configure_device(st, cfg, i, err, errlen) != 0)
            return -1;

    /* The receiver's engine ID is process-wide in net-snmp. */
    tsnmp_lock();
    tsnmp_set_local_engine_id(st->engine, st->engine_len);
    tsnmp_set_local_engine_boots(st->engine, st->engine_len, local_engine_boots());
    tsnmp_unlock();
    return 0;
}

/* ---- listener, sessions and the connected-device registry ---------------- */

static void close_listener(snmpc_state_t *st) {
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    free(st->forwarder_ips);
    st->forwarder_ips = NULL;
    st->nforwarder_ips = 0;
}

static int open_listener(snmpc_state_t *st, char *err, size_t errlen) {
    struct addrinfo hints = {.ai_socktype = SOCK_DGRAM,
                             .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV |
                                         AI_PASSIVE};
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(st->listen_host, st->listen_port, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, errlen, "cannot listen on udp %s: %s", st->listen,
                 gai_strerror(rc));
        return -1;
    }
    int fd = socket(res->ai_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "cannot listen on udp %s: %s", st->listen,
                 strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    /* No SO_REUSEADDR: a port another receiver holds must fail loudly (§3.1). */
    int zero = 0;
    if (res->ai_family == AF_INET6)
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        snprintf(err, errlen, "cannot listen on udp %s: %s", st->listen,
                 strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    freeaddrinfo(res);
    st->fd = fd;

    /* Forwarders are resolved when the listener opens. */
    if (st->nforwarders) {
        st->forwarder_ips = calloc(st->nforwarders, sizeof *st->forwarder_ips);
        for (size_t i = 0; st->forwarder_ips && i < st->nforwarders; i++) {
            char why[200];
            if (resolve(st->forwarders[i], &st->forwarder_ips[st->nforwarder_ips],
                        why, sizeof why) == 0)
                st->nforwarder_ips++;
            else
                snmpc_log("warn", "snmp: forwarder ignored: %s", why);
        }
    }
    return 0;
}

static void release_listener(snmpc_state_t *st) {
    if (st->listeners && --st->listeners == 0)
        close_listener(st);
}

static void queue_clear(snmpc_device_t *sd) {
    while (sd->count) {
        snmpc_queued_t *q = sd->queue[sd->head];
        sd->queue[sd->head] = NULL;
        sd->head = (sd->head + 1) % SNMPC_QUEUE_LEN;
        sd->count--;
        snmp_free_pdu(q->pdu);
        free(q);
    }
    sd->head = 0;
    sd->dropped = 0;
}

static void disconnect_device(tdot_connector_t *self, tdot_device_t *dev) {
    snmpc_state_t *st = self->state;
    snmpc_device_t *sd = dev->proto;
    if (!sd || !sd->connected)
        return;
    for (size_t i = 0; i < st->ndevices; i++)
        if (st->devices[i] == dev) {
            memmove(&st->devices[i], &st->devices[i + 1],
                    (st->ndevices - i - 1) * sizeof st->devices[0]);
            st->ndevices--;
            break;
        }
    tsnmp_lock();
    queue_clear(sd);
    tsnmp_unlock();
    tsnmp_session_close(sd->sess);
    sd->sess = NULL;
    if (sd->listening)
        release_listener(st);
    sd->listening = false;
    for (size_t j = 0; j < dev->npoints; j++) {
        snmpc_point_t *sp = dev->points[j].proto;
        if (sp)
            sp->cached = false;
    }
    sd->connected = false;
}

static int connect_device(tdot_connector_t *self, tdot_device_t *dev,
                          char *err, size_t errlen) {
    snmpc_state_t *st = self->state;
    snmpc_device_t *sd = dev->proto;
    if (!sd) {
        snprintf(err, errlen, "device not configured");
        return -1;
    }
    disconnect_device(self, dev);

    if (sd->unsupported[0]) {
        /* §10: SHA-2 authentication and AES-192/256 need a real OpenSSL, which
         * this build does not link. The device stays disconnected, and says so. */
        snprintf(err, errlen,
                 "SNMPv3 %s needs the snmpv3-sha2 capability, which the C build "
                 "does not have (see impl/c/README.md)",
                 sd->unsupported);
        return -1;
    }

    /* A name is resolved on every (re)connect; the first address wins. */
    snmpc_ip_t ip;
    if (resolve(sd->host, &ip, err, errlen) != 0)
        return -1;
    for (size_t i = 0; i < st->ndevices; i++) {
        const snmpc_device_t *other = st->devices[i]->proto;
        if (ip_equal(&other->addr, &ip)) {
            char text[INET6_ADDRSTRLEN];
            ip_format(&ip, text, sizeof text);
            snprintf(err, errlen,
                     "host '%s' resolves to %s, which device %s already holds",
                     sd->host, text, st->devices[i]->name);
            return -1;
        }
    }
    if (st->ndevices == st->capdevices) {
        size_t cap = st->capdevices ? st->capdevices * 2 : 8;
        tdot_device_t **grown = realloc(st->devices, cap * sizeof *grown);
        if (!grown) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        st->devices = grown;
        st->capdevices = cap;
    }

    /* The listening socket is bound only while a device has trap points, and
     * never for a CLI read or write: it would take the service's port. */
    if (sd->has_notify && !self->no_push) {
        if (st->fd < 0 && open_listener(st, err, errlen) != 0)
            return -1;
        st->listeners++;
        sd->listening = true;
    }

    if (sd->has_objects) {
        char addrtext[INET6_ADDRSTRLEN], peer[INET6_ADDRSTRLEN + 32];
        ip_format(&ip, addrtext, sizeof addrtext);
        if (ip.family == AF_INET6)
            snprintf(peer, sizeof peer, "udp6:[%s]:%d", addrtext, sd->port);
        else
            snprintf(peer, sizeof peer, "udp:%s:%d", addrtext, sd->port);
        tsnmp_session_params_t params = {
            .peer = peer,
            .version = sd->version,
            .community = sd->community,
            .timeout_us = (long)(sd->timeout_s * 1e6),
            .retries = sd->retries,
            .v3 = &sd->v3,
            .engine = sd->engine_known ? sd->engine : NULL,
            .engine_len = sd->engine_known ? sd->engine_len : 0,
        };
        char why[200];
        sd->sess = tsnmp_session_open(&params, why, sizeof why);
        if (!sd->sess) {
            snprintf(err, errlen, "snmp session to %s: %s", peer, why);
            if (sd->listening)
                release_listener(st);
            sd->listening = false;
            return -1;
        }
        /* A v3 session discovered the agent's engine: that engine is also the
         * one this device's traps must come from (§3.2). */
        if (sd->version == TSNMP_V3 && !sd->engine_known) {
            size_t n = tsnmp_session_engine(sd->sess, sd->engine,
                                            sizeof sd->engine);
            if (n) {
                sd->engine_len = n;
                sd->engine_known = true;
            }
        }
    }

    st->devices[st->ndevices++] = dev;
    sd->addr = ip;
    sd->connected = true;
    return 0;
}

static int subscribe_device(tdot_connector_t *self, tdot_device_t *dev,
                            char *err, size_t errlen) {
    (void)self;
    (void)err;
    (void)errlen;
    /* trap and varbind points are pushed; object points stay polled (§10) */
    for (size_t j = 0; j < dev->npoints; j++) {
        const snmpc_point_t *sp = dev->points[j].proto;
        if (sp && sp->kind != PT_OBJECT)
            dev->points[j].subscribed = true;
    }
    return 0;
}

/* ---- values -> samples --------------------------------------------------- */

/* A value (a polled object or a selected varbind) into a sample (§6). */
static void fill_value(const tdot_point_t *pt, const netsnmp_variable_list *v,
                       tdot_sample_t *s) {
    uint8_t canon[TSNMP_CANON_MAX];
    const uint8_t *content;
    size_t len;
    tsnmp_type_t type = tsnmp_var_content(v, canon, &content, &len);
    if (tsnmp_type_is_exception(type)) {
        tdot_sample_bad(s, "%s", tsnmp_exception_name(type)); /* empty raw */
        return;
    }
    s->raw_len = len < TDOT_RAW_MAX ? len : TDOT_RAW_MAX;
    memcpy(s->raw, content, s->raw_len);
    if (pt->mode == TDOT_MODE_RAW)
        return; /* always good; the runtime publishes the bytes only */
    char why[TDOT_ERR_MAX];
    if (tsnmp_convert(type, content, len, pt->datatype, &s->value, why,
                      sizeof why) != 0)
        tdot_sample_bad(s, "%s", why); /* keeps raw */
    else if (s->value.kind == TDOT_VAL_NUM && pt->has_transform)
        s->value.num = tdot_transform_apply(&pt->transform, s->value.num);
}

/* ---- polling (§5.1) ------------------------------------------------------ */

static void cache_point(tdot_point_t *pt, const tdot_sample_t *s, int rc) {
    snmpc_point_t *sp = pt->proto;
    sp->cached_sample = *s;
    sp->cached_rc = rc;
    sp->cached = true;
}

static void cache_bad(tdot_point_t *pt, int rc, const char *fmt, const char *why) {
    tdot_sample_t s;
    tdot_sample_init(&s);
    tdot_sample_bad(&s, fmt, why);
    cache_point(pt, &s, rc);
}

/* A scalar instance OID ends in .0 ("1.3.6.1.2.1.1.1.0"); the object it
 * instantiates is its parent. Only such a point can be fetched with GETBULK
 * (see fetch_batch). */
static bool scalar_instance(const tsnmp_oid_t *o) {
    return o->n >= 3 && o->arcs[o->n - 1] == 0;
}

/* One batch: a GETBULK (non-repeaters = N, max-repetitions 0) or a GET.
 *
 * GETBULK's non-repeaters are answered with each requested OID's lexicographic
 * SUCCESSOR (RFC 3416 §4.2.3) -- it is a GETNEXT with a count -- so a scalar is
 * requested by its PARENT and the response names the instance itself. Asking
 * for the instance would return the NEXT object instead, which the name check
 * below then rejects. `bulk` is false for a batch that cannot be asked that
 * way (v1, bulk = false, or a point that is not a scalar instance). */
static void fetch_batch(tdot_device_t *dev, tdot_point_t **pts, size_t count,
                        bool bulk) {
    snmpc_device_t *sd = dev->proto;
    tdot_point_t **todo = malloc(count * sizeof *todo);
    if (!todo) {
        for (size_t i = 0; i < count; i++)
            cache_bad(pts[i], 0, "%s", "out of memory");
        return;
    }
    memcpy(todo, pts, count * sizeof *todo);
    size_t ntodo = count;

    while (ntodo > 0) {
        tsnmp_lock();
        netsnmp_pdu *req = snmp_pdu_create(bulk ? SNMP_MSG_GETBULK : SNMP_MSG_GET);
        if (req) {
            if (bulk) {
                req->non_repeaters = (long)ntodo;
                req->max_repetitions = 0;
            }
            for (size_t i = 0; i < ntodo; i++) {
                const snmpc_point_t *sp = todo[i]->proto;
                oid name[MAX_OID_LEN];
                size_t n = tsnmp_oid_to_net(&sp->oid, name);
                if (bulk)
                    n--; /* the scalar's parent: GETBULK answers with its successor */
                snmp_add_null_var(req, name, n);
            }
        }
        tsnmp_unlock();
        if (!req) {
            for (size_t i = 0; i < ntodo; i++)
                cache_bad(todo[i], 0, "%s", "out of memory");
            break;
        }

        netsnmp_pdu *resp = NULL;
        char why[160];
        tsnmp_req_status_t status = tsnmp_request(sd->sess, req, &resp, why,
                                                  sizeof why);
        if (status != TSNMP_REQ_OK) {
            /* A timeout or a send failure means the transport is down, so the
             * runtime reconnects with its backoff (§5.1).
             *
             * A USM failure does not: the credentials are wrong, and a
             * reconnect only re-runs engine discovery, which is unauthenticated
             * and succeeds -- the device would announce `connected` again a
             * second later, and flap for as long as the password stays wrong.
             * Reported as bad samples on a link the runtime then marks
             * degraded (every polled point bad), which is where it settles. */
            char text[TDOT_ERR_MAX];
            if (status == TSNMP_REQ_SECURITY)
                snprintf(text, sizeof text, "SNMPv3 authentication failed: %s",
                         why);
            else
                snprintf(text, sizeof text, "%s", why);
            for (size_t i = 0; i < ntodo; i++)
                cache_bad(todo[i], status == TSNMP_REQ_SECURITY ? 0 : -1, "%s",
                          text);
            break;
        }

        tsnmp_lock();
        if (resp->errstat != SNMP_ERR_NOERROR) {
            const char *name = tsnmp_error_status_name(resp->errstat);
            long index = resp->errindex;
            if (sd->version == TSNMP_V1 && resp->errstat == SNMP_ERR_NOSUCHNAME &&
                index >= 1 && (size_t)index <= ntodo) {
                /* v1: that point is bad; ask again for the rest without it */
                snmp_free_pdu(resp);
                tsnmp_unlock();
                cache_bad(todo[index - 1], 0, "%s", name);
                memmove(&todo[index - 1], &todo[index],
                        (ntodo - (size_t)index) * sizeof *todo);
                ntodo--;
                continue;
            }
            snmp_free_pdu(resp);
            tsnmp_unlock();
            char text[80];
            snprintf(text, sizeof text, "%s (error-index %ld)", name, index);
            for (size_t i = 0; i < ntodo; i++)
                cache_bad(todo[i], 0, "%s", text);
            break;
        }
        netsnmp_variable_list *v = resp->variables;
        for (size_t i = 0; i < ntodo; i++, v = v ? v->next_variable : NULL) {
            tdot_sample_t s;
            tdot_sample_init(&s);
            const snmpc_point_t *sp = todo[i]->proto;
            tsnmp_oid_t name;
            if (!v) {
                tdot_sample_bad(&s, "the response carried no value for %s",
                                "this OID");
            } else if (v->type == SNMP_NOSUCHOBJECT ||
                       v->type == SNMP_NOSUCHINSTANCE ||
                       v->type == SNMP_ENDOFMIBVIEW) {
                /* An exception carries no value, and an agent answering a
                 * GETBULK past the end of a subtree may echo the name that was
                 * asked for (the scalar's parent), so it is reported as the
                 * exception it is rather than as a name mismatch (§5.1). */
                fill_value(todo[i], v, &s);
            } else if (!tsnmp_var_name(v, &name) ||
                       !tsnmp_oid_equal(&name, &sp->oid)) {
                char text[TSNMP_OID_STR_MAX];
                if (tsnmp_var_name(v, &name))
                    tsnmp_oid_format(&name, text, sizeof text);
                else
                    snprintf(text, sizeof text, "an oversized OID");
                tdot_sample_bad(&s, "the response named %s instead", text);
            } else {
                fill_value(todo[i], v, &s);
            }
            cache_point(todo[i], &s, 0);
        }
        snmp_free_pdu(resp);
        tsnmp_unlock();
        break;
    }
    free(todo);
}

static int read_point(tdot_connector_t *self, tdot_device_t *dev,
                      tdot_point_t *pt, tdot_sample_t *out) {
    (void)self;
    snmpc_point_t *sp = pt->proto;
    snmpc_device_t *sd = dev->proto;
    /* Trap and varbind points are delivered by notifications only: there is
     * nothing to read on demand, which is not a failure -- a heartbeat read
     * (contract §5.3) must neither publish bad quality nor retry. */
    if (!sp || sp->kind != PT_OBJECT)
        return TDOT_READ_NO_DATA;
    if (!sd || !sd->connected || !sd->sess) {
        tdot_sample_bad(out, "device not connected");
        return -1;
    }
    double now = tdot_mono();
    if (!(sp->cached && now - sd->batch_at < SNMPC_BATCH_FRESH_S)) {
        /* The first read of a tick: fetch every due object point at once. */
        size_t n = 0;
        tdot_point_t **due = malloc(dev->npoints * sizeof *due);
        if (!due) {
            tdot_sample_bad(out, "out of memory");
            return 0;
        }
        for (size_t j = 0; j < dev->npoints; j++) {
            tdot_point_t *q = &dev->points[j];
            snmpc_point_t *qp = q->proto;
            if (!qp || qp->kind != PT_OBJECT)
                continue;
            qp->cached = false;
            if (q == pt || ((q->access & TDOT_ACCESS_READ) && !q->subscribed &&
                            q->next_due <= now))
                due[n++] = q;
        }
        /* Scalars first, so they keep their GETBULK; anything else (a point
         * whose OID is not an instance) is fetched with GET. */
        size_t nscalar = 0;
        for (size_t i = 0; i < n; i++) {
            const snmpc_point_t *qp = due[i]->proto;
            if (scalar_instance(&qp->oid)) {
                tdot_point_t *swap = due[nscalar];
                due[nscalar++] = due[i];
                due[i] = swap;
            }
        }
        size_t step = (size_t)sd->max_varbinds;
        bool bulk = sd->bulk && sd->version != TSNMP_V1;
        for (size_t i = 0; i < nscalar; i += step)
            fetch_batch(dev, due + i, nscalar - i < step ? nscalar - i : step,
                        bulk);
        for (size_t i = nscalar; i < n; i += step)
            fetch_batch(dev, due + i, n - i < step ? n - i : step, false);
        free(due);
        /* The runtime ends a device's cycle at the first read that reports the
         * transport down, so every other point fetched with it would never
         * publish the bad sample it already has. Hold that verdict back to the
         * last of them: the whole batch is published, and the link still drops
         * on the final read. */
        sd->transport_pending = 0;
        for (size_t j = 0; j < dev->npoints; j++) {
            const snmpc_point_t *qp = dev->points[j].proto;
            if (qp && qp->cached && qp->cached_rc != 0)
                sd->transport_pending++;
        }
        sd->batch_at = tdot_mono();
    }
    if (!sp->cached) {
        tdot_sample_bad(out, "no value fetched");
        return 0;
    }
    *out = sp->cached_sample;
    sp->cached = false;
    int rc = sp->cached_rc;
    if (rc != 0 && sd->transport_pending > 0 && --sd->transport_pending > 0)
        rc = 0; /* a later point of the same batch carries the verdict */
    return rc;
}

/* ---- writes (§5.2) ------------------------------------------------------- */

static int value_i64(const tdot_value_t *v, int64_t *out, char *err,
                     size_t errlen) {
    if (v->kind == TDOT_VAL_BOOL) {
        *out = v->b ? 1 : 0;
        return 0;
    }
    if (v->kind == TDOT_VAL_NUM) {
        if (!isfinite(v->num) || v->num != floor(v->num) ||
            v->num < -9223372036854775808.0 || v->num >= 9223372036854775808.0) {
            snprintf(err, errlen, "value %g is not an integer", v->num);
            return -1;
        }
        *out = (int64_t)v->num;
        return 0;
    }
    if (v->kind == TDOT_VAL_STR) {
        char *end;
        errno = 0;
        long long x = strtoll(v->str, &end, 10);
        if (errno || end == v->str || *end) {
            snprintf(err, errlen, "value '%s' is not an integer", v->str);
            return -1;
        }
        *out = x;
        return 0;
    }
    snprintf(err, errlen, "a value is required");
    return -1;
}

static int value_u64(const tdot_value_t *v, uint64_t *out, char *err,
                     size_t errlen) {
    if (v->kind == TDOT_VAL_STR) {
        char *end;
        errno = 0;
        if (v->str[0] == '-') {
            snprintf(err, errlen, "value %s is negative", v->str);
            return -1;
        }
        unsigned long long x = strtoull(v->str, &end, 10);
        if (errno || end == v->str || *end) {
            snprintf(err, errlen, "value '%s' is not an integer", v->str);
            return -1;
        }
        *out = x;
        return 0;
    }
    if (v->kind == TDOT_VAL_NUM && v->num >= 9223372036854775808.0) {
        if (!isfinite(v->num) || v->num != floor(v->num) ||
            v->num >= 18446744073709551616.0) {
            snprintf(err, errlen, "value %g is out of range", v->num);
            return -1;
        }
        *out = (uint64_t)v->num;
        return 0;
    }
    int64_t s;
    if (value_i64(v, &s, err, errlen) != 0)
        return -1;
    if (s < 0) {
        snprintf(err, errlen, "value %lld is negative", (long long)s);
        return -1;
    }
    *out = (uint64_t)s;
    return 0;
}

/* Add the SET varbind for `value` (lock held). 0, or -1 with err. */
static int add_set_value(netsnmp_pdu *req, const tdot_point_t *pt,
                         const tdot_value_t *value, char *err, size_t errlen) {
    const snmpc_point_t *sp = pt->proto;
    tsnmp_type_t type = sp->set_type;
    oid name[MAX_OID_LEN];
    size_t name_len = tsnmp_oid_to_net(&sp->oid, name);
    const char *type_name = type == TSNMP_TYPE_GAUGE32 ? "unsigned32"
                                                       : tsnmp_type_name(type);
    int64_t i64 = 0;
    uint64_t u64 = 0;
    uint8_t bytes[TDOT_RAW_MAX];
    size_t nbytes = 0;
    tsnmp_oid_t o;
    char why[120];

    if (pt->mode == TDOT_MODE_RAW) {
        /* raw hex: the content octets of `type` */
        int n = value->kind == TDOT_VAL_STR
                    ? tdot_hex_parse(value->str, bytes, sizeof bytes)
                    : -1;
        if (n < 0) {
            snprintf(err, errlen, "a raw write takes the content octets as hex");
            return -1;
        }
        nbytes = (size_t)n;
        switch (type) {
        case TSNMP_TYPE_INTEGER:
            if (tsnmp_integer_decode(bytes, nbytes, &i64) != 0)
                goto bad_content;
            break;
        case TSNMP_TYPE_GAUGE32:
        case TSNMP_TYPE_COUNTER32:
        case TSNMP_TYPE_TIMETICKS:
            if (tsnmp_unsigned_decode(bytes, nbytes, 5, UINT32_MAX, &u64) != 0)
                goto bad_content;
            break;
        case TSNMP_TYPE_COUNTER64:
            if (tsnmp_unsigned_decode(bytes, nbytes, 9, UINT64_MAX, &u64) != 0)
                goto bad_content;
            break;
        case TSNMP_TYPE_OID:
            if (tsnmp_oid_decode(bytes, nbytes, &o, why, sizeof why) != 0)
                goto bad_content;
            break;
        case TSNMP_TYPE_IP_ADDRESS:
            if (nbytes != 4)
                goto bad_content;
            break;
        default:
            break;
        }
    } else {
        switch (type) {
        case TSNMP_TYPE_INTEGER:
            if (value_i64(value, &i64, err, errlen) != 0)
                return -1;
            break;
        case TSNMP_TYPE_GAUGE32:
        case TSNMP_TYPE_COUNTER32:
        case TSNMP_TYPE_TIMETICKS:
        case TSNMP_TYPE_COUNTER64:
            if (value_u64(value, &u64, err, errlen) != 0)
                return -1;
            break;
        case TSNMP_TYPE_OCTET_STRING:
            if (value->kind != TDOT_VAL_STR) {
                snprintf(err, errlen, "an octet_string write takes a string");
                return -1;
            }
            nbytes = strlen(value->str);
            memcpy(bytes, value->str, nbytes);
            break;
        case TSNMP_TYPE_IP_ADDRESS:
            if (value->kind != TDOT_VAL_STR ||
                inet_pton(AF_INET, value->str, bytes) != 1) {
                snprintf(err, errlen, "an ip_address write takes a dotted IPv4 "
                                      "address");
                return -1;
            }
            nbytes = 4;
            break;
        case TSNMP_TYPE_OID:
            if (value->kind != TDOT_VAL_STR ||
                tsnmp_oid_parse(value->str, &o, why, sizeof why) != 0) {
                snprintf(err, errlen, "an oid write takes a dotted OID");
                return -1;
            }
            break;
        default:
            break;
        }
    }

    switch (type) {
    case TSNMP_TYPE_INTEGER: {
        if (i64 < INT32_MIN || i64 > INT32_MAX) {
            snprintf(err, errlen, "value %lld is out of range for integer "
                                  "(32-bit)",
                     (long long)i64);
            return -1;
        }
        long l = (long)i64;
        snmp_pdu_add_variable(req, name, name_len, ASN_INTEGER, &l, sizeof l);
        return 0;
    }
    case TSNMP_TYPE_GAUGE32:
    case TSNMP_TYPE_COUNTER32:
    case TSNMP_TYPE_TIMETICKS: {
        if (u64 > UINT32_MAX) {
            snprintf(err, errlen, "value %llu is out of range for %s",
                     (unsigned long long)u64, type_name);
            return -1;
        }
        u_long ul = (u_long)u64;
        u_char asn = type == TSNMP_TYPE_GAUGE32     ? ASN_GAUGE
                     : type == TSNMP_TYPE_COUNTER32 ? ASN_COUNTER
                                                    : ASN_TIMETICKS;
        snmp_pdu_add_variable(req, name, name_len, asn, &ul, sizeof ul);
        return 0;
    }
    case TSNMP_TYPE_COUNTER64: {
        struct counter64 c64 = {.high = (u_long)(u64 >> 32),
                                .low = (u_long)(u64 & 0xFFFFFFFFULL)};
        snmp_pdu_add_variable(req, name, name_len, ASN_COUNTER64, &c64,
                              sizeof c64);
        return 0;
    }
    case TSNMP_TYPE_OCTET_STRING:
        snmp_pdu_add_variable(req, name, name_len, ASN_OCTET_STR, bytes, nbytes);
        return 0;
    case TSNMP_TYPE_IP_ADDRESS:
        snmp_pdu_add_variable(req, name, name_len, ASN_IPADDRESS, bytes, 4);
        return 0;
    case TSNMP_TYPE_OID: {
        oid arcs[MAX_OID_LEN];
        size_t n = tsnmp_oid_to_net(&o, arcs);
        snmp_pdu_add_variable(req, name, name_len, ASN_OBJECT_ID, arcs,
                              n * sizeof(oid));
        return 0;
    }
    default:
        snprintf(err, errlen, "type %s cannot be written", type_name);
        return -1;
    }
bad_content:
    snprintf(err, errlen, "the hex is not a valid %s content", type_name);
    return -1;
}

static int write_point(tdot_connector_t *self, tdot_device_t *dev,
                       tdot_point_t *pt, const tdot_value_t *value, char *err,
                       size_t errlen) {
    (void)self;
    const snmpc_point_t *sp = pt->proto;
    snmpc_device_t *sd = dev->proto;
    if (!sp || sp->kind != PT_OBJECT || !(pt->access & TDOT_ACCESS_WRITE) ||
        !sp->has_set_type) {
        snprintf(err, errlen, "point %s is not writable", pt->id);
        return -1;
    }
    if (!sd || !sd->connected || !sd->sess) {
        snprintf(err, errlen, "device not connected");
        return -1;
    }
    tsnmp_lock();
    netsnmp_pdu *req = snmp_pdu_create(SNMP_MSG_SET);
    int rc = req ? 0 : -1;
    if (!req)
        snprintf(err, errlen, "out of memory");
    if (req && sd->version != TSNMP_V3) {
        req->community = (u_char *)strdup(sd->write_community);
        req->community_len = strlen(sd->write_community);
    }
    if (req && add_set_value(req, pt, value, err, errlen) != 0) {
        snmp_free_pdu(req);
        rc = -1;
    }
    tsnmp_unlock();
    if (rc != 0)
        return -1;

    netsnmp_pdu *resp = NULL;
    char why[160];
    if (tsnmp_request(sd->sess, req, &resp, why, sizeof why) != TSNMP_REQ_OK) {
        snprintf(err, errlen, "SET failed: %s", why);
        return -1;
    }
    tsnmp_lock();
    long status = resp->errstat;
    snmp_free_pdu(resp);
    tsnmp_unlock();
    if (status != SNMP_ERR_NOERROR) {
        snprintf(err, errlen, "%s", tsnmp_error_status_name(status));
        return -1;
    }
    return 0;
}

/* ---- notifications (§4) -------------------------------------------------- */

static bool source_log_due(snmpc_state_t *st, const snmpc_ip_t *ip,
                           bool unknown) {
    double now = tdot_mono();
    snmpc_source_log_t *slot = NULL, *victim = NULL;
    for (size_t i = 0; i < SNMPC_SOURCE_LOG_LEN && !slot; i++) {
        snmpc_source_log_t *e = &st->sources[i];
        if (e->used && ip_equal(&e->ip, ip))
            slot = e;
        else if (!victim || (victim->used &&
                             (!e->used || e->seen_at < victim->seen_at)))
            victim = e;
    }
    if (!slot) {
        slot = victim;
        slot->used = true;
        slot->ip = *ip;
        slot->undecodable_at = slot->unknown_at = -1e18;
    }
    slot->seen_at = now;
    double *at = unknown ? &slot->unknown_at : &slot->undecodable_at;
    if (now - *at < SNMPC_LOG_INTERVAL_S)
        return false;
    *at = now;
    return true;
}

static void warn_device(snmpc_device_t *sd, const char *fmt, ...) {
    double now = tdot_mono();
    if (now - sd->auth_warned_at < SNMPC_LOG_INTERVAL_S)
        return;
    sd->auth_warned_at = now;
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    snmpc_log("warn", "%s", text);
}

static tdot_device_t *device_by_ip(snmpc_state_t *st, const snmpc_ip_t *ip) {
    for (size_t i = 0; i < st->ndevices; i++) {
        const snmpc_device_t *sd = st->devices[i]->proto;
        if (ip_equal(&sd->addr, ip))
            return st->devices[i];
    }
    return NULL;
}

static bool any_subscribed(const tdot_device_t *dev) {
    for (size_t j = 0; j < dev->npoints; j++)
        if (dev->points[j].subscribed)
            return true;
    return false;
}

static bool community_accepted(const snmpc_state_t *st, const snmpc_device_t *sd,
                               const netsnmp_pdu *pdu) {
    size_t n = sd->naccept ? sd->naccept : st->ncommunities;
    if (n == 0)
        return true;
    for (size_t i = 0; i < n; i++) {
        const char *c = sd->naccept ? device_community(sd, i) : st->communities[i];
        if (strlen(c) == pdu->community_len &&
            (pdu->community_len == 0 ||
             memcmp(c, pdu->community, pdu->community_len) == 0))
            return true;
    }
    return false;
}

static void send_pdu(snmpc_state_t *st, netsnmp_pdu *pdu,
                     const struct sockaddr *to, socklen_t tolen,
                     const char *what, const char *device) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    if (tsnmp_pdu_to_datagram(pdu, &bytes, &len) != 0) {
        snmpc_log("warn", "device %s: cannot encode the %s", device, what);
        return;
    }
    if (sendto(st->fd, bytes, len, 0, to, tolen) < 0)
        snmpc_log("warn", "device %s: cannot send the %s: %s", device, what,
                  strerror(errno));
    free(bytes);
}

/* A v3 message from `dev`: put this device's user in the USM table for the
 * engine the message names, so the library authenticates it with this
 * device's keys (lock held). false when it must be dropped unparsed. */
static bool v3_prepare(snmpc_device_t *sd, const tsnmp_v3_header_t *h,
                       const uint8_t *local, size_t local_len,
                       char *why, size_t whylen) {
    if (h->engine_len == 0)
        return true; /* a discovery probe: answered with a Report */
    if (strcmp(h->user, sd->v3.user) != 0)
        return true; /* unknown user: the library reports it */
    bool is_local = h->engine_len == local_len &&
                    memcmp(h->engine, local, local_len) == 0;
    if (is_local) {
        /* an inform: we are the authoritative engine */
        tsnmp_usm_install(&sd->v3, h->engine, h->engine_len, false);
        return true;
    }
    if (sd->engine_known && (h->engine_len != sd->engine_len ||
                             memcmp(h->engine, sd->engine, sd->engine_len) != 0)) {
        snprintf(why, whylen, "its engine ID is not the device's");
        return false;
    }
    /* Authenticating the message needs keys localized to the engine it CLAIMS, so this install
     * cannot wait until the message is verified. net-snmp's user table and engine-time cache
     * are process-wide and never shrink, so a spoofed source sending forged engine IDs grows
     * them without bound (see TODO.md). Removing the stale entries again is not the fix it
     * appears to be: net-snmp's free_enginetime() empties the whole 1-of-23 hash bucket
     * without comparing engine IDs, so it would discard the boots/time of unrelated engines --
     * this connector's own among them -- and usm_remove_user() would delete an entry another
     * device's session owns and never rebuilds. */
    tsnmp_usm_install(&sd->v3, h->engine, h->engine_len, true);
    return true;
}

static void handle_datagram(snmpc_state_t *st, size_t len,
                            const struct sockaddr *from, socklen_t fromlen) {
    const uint8_t *buf = st->rx;
    snmpc_ip_t src;
    ip_from_sockaddr(from, &src);
    char source[INET6_ADDRSTRLEN], forwarder[INET6_ADDRSTRLEN] = "";
    ip_format(&src, source, sizeof source);

    bool forwarded = false;
    for (size_t i = 0; i < st->nforwarder_ips && !forwarded; i++)
        forwarded = ip_equal(&st->forwarder_ips[i], &src);

    long version = tsnmp_message_version(buf, len);
    netsnmp_pdu *pdu = NULL;
    int lib_errno = 0;
    tdot_device_t *dev = NULL;
    char err[200];

    if (forwarded) {
        /* §3.1: route by the forwarder's last snmpTrapAddress.0 -- which
         * needs the message decoded first. */
        snprintf(forwarder, sizeof forwarder, "%s", source);
        if (version == SNMP_VERSION_3) {
            if (source_log_due(st, &src, false))
                snmpc_log("warn",
                          "snmp: dropped a v3 notification from forwarder %s: "
                          "v3 through a forwarder is not supported",
                          source);
            return;
        }
        if (tsnmp_parse_datagram(buf, len, &pdu, &lib_errno) != 0) {
            if (source_log_due(st, &src, false))
                snmpc_log("warn",
                          "snmp: dropped an undecodable datagram from forwarder "
                          "%s: %s",
                          source, snmp_api_errstring(lib_errno));
            goto drop;
        }
        uint8_t a[4];
        if (!tsnmp_trap_address(pdu, a)) {
            if (source_log_due(st, &src, true) && st->debug)
                snmpc_log("debug",
                          "snmp: dropped a notification from forwarder %s "
                          "without snmpTrapAddress.0",
                          source);
            goto drop;
        }
        snmpc_ip_t routed = {.family = AF_INET};
        memcpy(routed.bytes, a, 4);
        ip_format(&routed, source, sizeof source);
        dev = device_by_ip(st, &routed);
        if (!dev) {
            if (source_log_due(st, &routed, true) && st->debug)
                snmpc_log("debug",
                          "snmp: dropped a notification forwarded by %s for %s: "
                          "no connected device has that address",
                          forwarder, source);
            goto drop;
        }
    } else {
        /* §4.1 step 1: route by source before anything else */
        dev = device_by_ip(st, &src);
        if (!dev) {
            if (source_log_due(st, &src, true) && st->debug)
                snmpc_log("debug",
                          "snmp: dropped a datagram from %s: no connected device "
                          "has that address",
                          source);
            return;
        }
        snmpc_device_t *sd = dev->proto;
        if (version == SNMP_VERSION_3) {
            tsnmp_v3_header_t h;
            if (tsnmp_v3_peek(buf, len, &h) != 0) {
                if (source_log_due(st, &src, false))
                    snmpc_log("warn",
                              "device %s: dropped an undecodable datagram from %s",
                              dev->name, source);
                return;
            }
            if (!sd->has_v3) {
                warn_device(sd,
                            "device %s: dropped a v3 notification from %s: the "
                            "device has no v3 credentials",
                            dev->name, source);
                return;
            }
            uint8_t local[32];
            size_t local_len = tsnmp_local_engine_id(local, sizeof local);
            if (!v3_prepare(sd, &h, local, local_len, err, sizeof err)) {
                warn_device(sd, "device %s: dropped a v3 notification from %s: %s",
                            dev->name, source, err);
                return;
            }
        }
        if (tsnmp_parse_datagram(buf, len, &pdu, &lib_errno) != 0) {
            netsnmp_pdu *report =
                version == SNMP_VERSION_3 ? tsnmp_report_for(pdu, lib_errno) : NULL;
            if (report) {
                send_pdu(st, report, from, fromlen, "USM report", dev->name);
                snmp_free_pdu(report);
            }
            if (version == SNMP_VERSION_3 && tsnmp_usm_failure(lib_errno)) {
                bool discovery = lib_errno == SNMPERR_USM_UNKNOWNENGINEID ||
                                 lib_errno == SNMPERR_USM_NOTINTIMEWINDOW;
                if (discovery) {
                    if (source_log_due(st, &src, true) && st->debug)
                        snmpc_log("debug",
                                  "device %s: answered a v3 discovery from %s "
                                  "with a report (%s)",
                                  dev->name, source, snmp_api_errstring(lib_errno));
                } else {
                    warn_device(sd,
                                "device %s: dropped a v3 message from %s: %s",
                                dev->name, source, snmp_api_errstring(lib_errno));
                }
            } else if (source_log_due(st, &src, false)) {
                snmpc_log("warn",
                          "device %s: dropped an undecodable datagram from %s: %s",
                          dev->name, source, snmp_api_errstring(lib_errno));
            }
            goto drop;
        }
    }

    snmpc_device_t *sd = dev->proto;

    /* §4.1 step 4: a v3 sender that has not discovered this receiver probes it
     * with an empty engine ID. The library accepts such a message (discovery is
     * allowed at noAuthNoPriv), so the Report is ours to send -- without it an
     * inform sender never learns our engine ID and never gets to the inform. */
    if (pdu->version == SNMP_VERSION_3 && pdu->securityEngineIDLen == 0) {
        netsnmp_pdu *report = tsnmp_report_for(pdu, SNMPERR_USM_UNKNOWNENGINEID);
        if (report) {
            send_pdu(st, report, from, fromlen, "USM report", dev->name);
            snmp_free_pdu(report);
        }
        if (source_log_due(st, &src, true) && st->debug)
            snmpc_log("debug",
                      "device %s: answered a v3 engine discovery from %s",
                      dev->name, source);
        goto drop;
    }

    tsnmp_notification_t n;
    if (tsnmp_notification_check(pdu, &n, err, sizeof err) != 0) {
        if (source_log_due(st, &src, false))
            snmpc_log("warn", "device %s: dropped an undecodable datagram from %s: %s",
                      dev->name, forwarded ? forwarder : source, err);
        goto drop;
    }

    /* §4.1 step 3: authenticate */
    if (n.version == TSNMP_V3) {
        if (!pdu->securityName || strcmp(pdu->securityName, sd->v3.user) != 0 ||
            pdu->securityLevel < sd->v3.level) {
            warn_device(sd,
                        "device %s: dropped a v3 notification from %s: not the "
                        "device's user at its security level",
                        dev->name, source);
            goto drop;
        }
        uint8_t local[32];
        size_t local_len = tsnmp_local_engine_id(local, sizeof local);
        bool is_local = pdu->securityEngineIDLen == local_len &&
                        memcmp(pdu->securityEngineID, local, local_len) == 0;
        if (!is_local && !sd->engine_known && n.kind == TSNMP_PDU_TRAP &&
            pdu->securityEngineIDLen <= sizeof sd->engine) {
            /* learned from the first authenticated trap (§3.2) */
            memcpy(sd->engine, pdu->securityEngineID, pdu->securityEngineIDLen);
            sd->engine_len = pdu->securityEngineIDLen;
            sd->engine_known = true;
        }
    } else if (!community_accepted(st, sd, pdu)) {
        warn_device(sd,
                    "device %s: dropped a notification from %s: its community is "
                    "not accepted",
                    dev->name, source);
        goto drop;
    }

    /* §4.1 step 4: acknowledge an inform, to where it came from */
    if (n.kind == TSNMP_PDU_INFORM) {
        netsnmp_pdu *resp = tsnmp_inform_response(pdu);
        if (resp) {
            send_pdu(st, resp, from, fromlen, "inform response", dev->name);
            snmp_free_pdu(resp);
        }
    }

    if (any_subscribed(dev)) {
        snmpc_queued_t *q = calloc(1, sizeof *q);
        if (q) {
            q->pdu = pdu;
            pdu = NULL;
            snprintf(q->source, sizeof q->source, "%s", source);
            snprintf(q->forwarder, sizeof q->forwarder, "%s", forwarder);
            if (sd->count == SNMPC_QUEUE_LEN) {
                snmpc_queued_t *old = sd->queue[sd->head];
                snmp_free_pdu(old->pdu);
                free(old);
                sd->head = (sd->head + 1) % SNMPC_QUEUE_LEN;
                sd->count--;
                sd->dropped++;
            }
            sd->queue[(sd->head + sd->count) % SNMPC_QUEUE_LEN] = q;
            sd->count++;
        }
    }
drop:
    if (pdu)
        snmp_free_pdu(pdu);
}

static int receive_pending(snmpc_state_t *st) {
    for (int i = 0; i < SNMPC_RX_BURST; i++) {
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof from;
        ssize_t n = recvfrom(st->fd, st->rx, SNMPC_RX_MAX, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            /* EINTR, and the ICMP errors an earlier send can leave (§7) */
            if (errno == EINTR || errno == ECONNREFUSED || errno == ECONNRESET)
                continue;
            snmpc_log("warn", "snmp: receive on udp %s failed: %s", st->listen,
                      strerror(errno));
            return -1;
        }
        handle_datagram(st, (size_t)n, (struct sockaddr *)&from, fromlen);
    }
    return 0;
}

static char *notification_addr(const char *source, const char *forwarder,
                               const tsnmp_notification_t *n, const char *trap,
                               const char *oid_text) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "source", source);
    cJSON_AddStringToObject(obj, "version", tsnmp_version_name(n->version));
    cJSON_AddStringToObject(obj, "pdu",
                            n->kind == TSNMP_PDU_INFORM ? "inform" : "trap");
    cJSON_AddStringToObject(obj, "trap", trap);
    if (oid_text)
        cJSON_AddStringToObject(obj, "oid", oid_text);
    if (forwarder[0])
        cJSON_AddStringToObject(obj, "forwarder", forwarder);
    if (n->has_agent_addr) {
        char agent[16];
        snprintf(agent, sizeof agent, "%u.%u.%u.%u", n->agent_addr[0],
                 n->agent_addr[1], n->agent_addr[2], n->agent_addr[3]);
        cJSON_AddStringToObject(obj, "agent_addr", agent);
    }
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

static bool trap_matches(const snmpc_point_t *sp, const tsnmp_oid_t *trap) {
    for (size_t i = 0; i < sp->ntraps; i++)
        if (tsnmp_oid_under(trap, &sp->traps[i]))
            return true;
    return false;
}

/* One sample per subscribed trap/varbind point the notification matches, in
 * configuration order (§4.1 step 5). Lock held. */
static void emit_notification(tdot_device_t *dev, const snmpc_queued_t *q,
                              const tsnmp_notification_t *n,
                              tdot_sample_sink_t sink, void *sink_ctx) {
    static char trap_text[TSNMP_OID_STR_MAX], oid_text[TSNMP_OID_STR_MAX];
    tsnmp_oid_format(&n->trap, trap_text, sizeof trap_text);
    uint8_t trap_raw[TDOT_RAW_MAX];
    size_t trap_raw_len = tsnmp_oid_encode(&n->trap, trap_raw, sizeof trap_raw);
    if (trap_raw_len > sizeof trap_raw)
        trap_raw_len = sizeof trap_raw;

    for (size_t j = 0; j < dev->npoints; j++) {
        tdot_point_t *pt = &dev->points[j];
        const snmpc_point_t *sp = pt->proto;
        if (!sp || sp->kind == PT_OBJECT || !pt->subscribed)
            continue;
        if (!trap_matches(sp, &n->trap))
            continue;
        tdot_sample_t s;
        tdot_sample_init(&s);
        char *addr = NULL;
        if (sp->kind == PT_TRAP) {
            addr = notification_addr(q->source, q->forwarder, n, trap_text, NULL);
            memcpy(s.raw, trap_raw, trap_raw_len);
            s.raw_len = trap_raw_len;
            if (pt->datatype == TDOT_DT_STRING) {
                s.value.kind = TDOT_VAL_STR;
                snprintf(s.value.str, sizeof s.value.str, "%s", trap_text);
            } else {
                s.value.kind = TDOT_VAL_BOOL;
                s.value.b = true;
            }
        } else {
            const netsnmp_variable_list *found = NULL;
            tsnmp_oid_t name;
            for (const netsnmp_variable_list *v = n->pdu->variables; v && !found;
                 v = v->next_variable)
                if (tsnmp_var_name(v, &name) && tsnmp_oid_under(&name, &sp->oid))
                    found = v;
            if (!found) {
                tsnmp_oid_format(&sp->oid, oid_text, sizeof oid_text);
                addr = notification_addr(q->source, q->forwarder, n, trap_text,
                                         NULL);
                tdot_sample_bad(&s, "notification %s carried no varbind under %s",
                                trap_text, oid_text);
            } else {
                tsnmp_oid_format(&name, oid_text, sizeof oid_text);
                addr = notification_addr(q->source, q->forwarder, n, trap_text,
                                         oid_text);
                fill_value(pt, found, &s);
            }
        }
        s.addr_json = addr;
        sink(sink_ctx, dev, pt, &s);
        free(addr);
    }
}

static int drain_subscriptions(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_sample_sink_t sink, void *sink_ctx) {
    snmpc_state_t *st = self->state;
    snmpc_device_t *sd = dev->proto;
    if (!sd || !sd->connected || !sd->has_notify)
        return 0;
    if (st->fd < 0)
        return -1;
    tsnmp_lock();
    /* §7: a receive error other than would-block reports the link down */
    int rc = receive_pending(st);
    while (rc == 0 && sd->count) {
        snmpc_queued_t *q = sd->queue[sd->head];
        sd->queue[sd->head] = NULL;
        sd->head = (sd->head + 1) % SNMPC_QUEUE_LEN;
        sd->count--;
        tsnmp_notification_t n;
        char err[160];
        if (tsnmp_notification_check(q->pdu, &n, err, sizeof err) == 0)
            emit_notification(dev, q, &n, sink, sink_ctx);
        snmp_free_pdu(q->pdu);
        free(q);
    }
    if (sd->dropped) {
        snmpc_log("warn",
                  "device %s: dropped the %lu oldest notification(s); its queue "
                  "of %d filled between two runtime ticks",
                  dev->name, sd->dropped, SNMPC_QUEUE_LEN);
        sd->dropped = 0;
    }
    tsnmp_unlock();
    return rc;
}

/* ---- the rest of the vtable --------------------------------------------- */

static char *device_info(tdot_connector_t *self, const tdot_device_t *dev) {
    (void)self;
    const snmpc_device_t *sd = dev->proto;
    if (!sd)
        return NULL;
    /* never the credentials (§3.2) */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "host", sd->host);
    cJSON_AddNumberToObject(obj, "port", sd->port);
    cJSON_AddStringToObject(obj, "version", tsnmp_version_name(sd->version));
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return out;
}

static void destroy(tdot_connector_t *self) {
    snmpc_state_t *st = self->state;
    if (st) {
        close_listener(st);
        free_strings(st->communities, st->ncommunities);
        free_strings(st->forwarders, st->nforwarders);
        free(st->devices);
        free(st->rx);
        free(st);
    }
    free(self);
}

tdot_connector_t *tdot_connector_snmp_new(void) {
    tdot_connector_t *c = calloc(1, sizeof *c);
    snmpc_state_t *st = calloc(1, sizeof *st);
    uint8_t *rx = malloc(SNMPC_RX_MAX);
    if (!c || !st || !rx) {
        free(c);
        free(st);
        free(rx);
        return NULL;
    }
    st->fd = -1;
    st->rx = rx;
    c->protocol = "snmp";
    c->capabilities_json = CAPABILITIES;
    c->local_only_settings = LOCAL_ONLY_SETTINGS;
    c->state = st;
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
