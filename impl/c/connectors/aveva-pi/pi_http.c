/* AVEVA PI connector -- the HTTP layer on libcurl. See pi_http.h. */
#include "pi_http.h"

/* The verify callback curl installs is a private field of the mbedTLS configuration. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pi_log.h"
#include "pi_value.h"

/* The system CA bundles tried, in order, when tls.ca_file is not set: libcurl is built without
 * a compiled-in path (cmake/curl.cmake), because a cross build would bake in the build host's. */
static const char *const SYSTEM_BUNDLES[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Alpine */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
    "/etc/ssl/cert.pem",                  /* macOS, BSD */
    NULL,
};

void pi_settings_default(pi_settings_t *s) {
    memset(s, 0, sizeof *s);
    s->auth = PI_AUTH_NONE;
    s->request_timeout_s = 10;
    s->connect_timeout_s = 5;
    s->resolve_retry_s = 300;
    s->max_web_ids_per_request = 50;
    s->max_url_bytes = 8192;
    s->max_concurrent_requests = 4;
    snprintf(s->update_option, sizeof s->update_option, "Replace");
    snprintf(s->buffer_option, sizeof s->buffer_option, "BufferIfPossible");
    snprintf(s->state_dir, sizeof s->state_dir, "/var/lib/tedge-dot/aveva-pi");
    s->backfill = true;
    s->max_age_s = 3600;
    s->max_events = 10000;
    s->page_size = 1000;
    s->live_buffer = 10000;
    s->cursor_flush_s = 10;
    s->heartbeat_rate = 5;
    s->polling_interval_s = 1;
    s->include_initial_values = true;
}

/* ---- errors --------------------------------------------------------------------------------- */

void pi_error_format(const pi_error_t *e, char *dst, size_t cap) {
    switch (e->kind) {
    case PI_E_NONE:
        snprintf(dst, cap, "ok");
        break;
    case PI_E_UNAUTHORIZED:
        snprintf(dst, cap, "unauthorized");
        break;
    case PI_E_TOO_LONG:
        snprintf(dst, cap, "HTTP %ld: request too long", e->code);
        break;
    case PI_E_STATUS:
        if (e->text[0])
            snprintf(dst, cap, "HTTP %ld: %s", e->code, e->text);
        else
            snprintf(dst, cap, "HTTP %ld", e->code);
        break;
    case PI_E_TIMEOUT:
        snprintf(dst, cap, "timeout: %s", e->text);
        break;
    case PI_E_TRANSPORT:
        snprintf(dst, cap, "%s", e->text);
        break;
    case PI_E_STOPPED:
        snprintf(dst, cap, "stopped");
        break;
    }
}

bool pi_error_is_link_failure(const pi_error_t *e) {
    switch (e->kind) {
    case PI_E_TIMEOUT:
    case PI_E_TRANSPORT:
    case PI_E_UNAUTHORIZED:
        return true;
    case PI_E_STATUS:
        return e->code >= 500;
    default:
        return false;
    }
}

/* ---- credentials ---------------------------------------------------------------------------- */

/* A secret file's content, trailing newlines stripped. Caller frees (and wipes). */
static char *read_secret(const char *path, const char *what, char *err, size_t errlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "%s %s: %s", what, path, strerror(errno));
        return NULL;
    }
    pi_buf_t b = {0};
    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
        pi_buf_add_n(&b, chunk, n);
    bool failed = ferror(fp) != 0;
    fclose(fp);
    memset(chunk, 0, sizeof chunk);
    if (failed || b.oom) {
        snprintf(err, errlen, "%s %s: %s", what, path, failed ? "read error" : "out of memory");
        pi_buf_free(&b);
        return NULL;
    }
    if (!b.s)
        return strdup("");
    while (b.len && (b.s[b.len - 1] == '\n' || b.s[b.len - 1] == '\r'))
        b.s[--b.len] = '\0';
    return b.s;
}

static void wipe_free(char *s) {
    if (s) {
        memset(s, 0, strlen(s));
        free(s);
    }
}

static char *base64(const char *in) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in);
    char *out = malloc(4 * ((n + 2) / 3) + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n)
            v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n)
            v |= (unsigned char)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static char *bearer_header(const char *token) {
    size_t n = strlen(token) + 32;
    char *h = malloc(n);
    if (h)
        snprintf(h, n, "Authorization: Bearer %s", token);
    return h;
}

int pi_creds_init(pi_creds_t *c, const pi_settings_t *s, char *err, size_t errlen) {
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    c->method = s->auth;
    if (s->auth == PI_AUTH_BASIC) {
        char *password = read_secret(s->password_file, "auth.password_file", err, errlen);
        if (!password)
            return -1;
        size_t n = strlen(s->username) + strlen(password) + 2;
        char *pair = malloc(n);
        char *token = NULL;
        if (pair) {
            snprintf(pair, n, "%s:%s", s->username, password);
            token = base64(pair);
            wipe_free(pair);
        }
        wipe_free(password);
        if (token) {
            n = strlen(token) + 32;
            c->header = malloc(n);
            if (c->header)
                snprintf(c->header, n, "Authorization: Basic %s", token);
        }
        wipe_free(token);
        if (!c->header) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
    } else if (s->auth == PI_AUTH_BEARER) {
        snprintf(c->token_file, sizeof c->token_file, "%s", s->token_file);
        char *token = read_secret(s->token_file, "auth.token_file", err, errlen);
        if (!token)
            return -1;
        c->header = bearer_header(token);
        wipe_free(token);
        if (!c->header) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
    }
    return 0;
}

void pi_creds_free(pi_creds_t *c) {
    wipe_free(c->header);
    c->header = NULL;
    pthread_mutex_destroy(&c->mu);
}

bool pi_creds_refresh(pi_creds_t *c) {
    if (c->method != PI_AUTH_BEARER)
        return false;
    char err[TDOT_REASON_MAX];
    char *token = read_secret(c->token_file, "auth.token_file", err, sizeof err);
    if (!token) {
        pi_log("warn", "%s", err);
        return false;
    }
    char *header = bearer_header(token);
    wipe_free(token);
    if (!header)
        return false;
    bool changed = false;
    pthread_mutex_lock(&c->mu);
    if (!c->header || strcmp(c->header, header) != 0) {
        wipe_free(c->header);
        c->header = header;
        header = NULL;
        changed = true;
    }
    pthread_mutex_unlock(&c->mu);
    wipe_free(header);
    return changed;
}

/* Append the Authorization header (curl copies it). */
static struct curl_slist *with_auth(pi_creds_t *c, struct curl_slist *h) {
    pthread_mutex_lock(&c->mu);
    if (c->header)
        h = curl_slist_append(h, c->header);
    pthread_mutex_unlock(&c->mu);
    return h;
}

/* ---- query strings -------------------------------------------------------------------------- */

void pi_buf_add_n(pi_buf_t *b, const char *s, size_t n) {
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) {
            b->oom = true;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

void pi_buf_add(pi_buf_t *b, const char *s) { pi_buf_add_n(b, s, strlen(s)); }

static void add_encoded(pi_buf_t *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            pi_buf_add_n(b, (const char *)p, 1);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            pi_buf_add_n(b, hex, 3);
        }
    }
}

void pi_buf_pair(pi_buf_t *b, const char *key, const char *value) {
    if (b->len)
        pi_buf_add_n(b, "&", 1);
    add_encoded(b, key);
    pi_buf_add_n(b, "=", 1);
    add_encoded(b, value);
}

void pi_buf_free(pi_buf_t *b) {
    free(b->s);
    memset(b, 0, sizeof *b);
}

size_t pi_chunks(const char *const *ids, size_t n, size_t chunk, size_t max_url_bytes,
                 size_t fixed, size_t *starts) {
    size_t count = 0, in_chunk = 0, len = fixed;
    if (chunk < 1)
        chunk = 1;
    for (size_t i = 0; i < n; i++) {
        size_t add = strlen("&webId=") + strlen(ids[i]);
        if (in_chunk > 0 && (in_chunk >= chunk || len + add > max_url_bytes)) {
            in_chunk = 0;
            len = fixed;
        }
        if (in_chunk == 0)
            starts[count++] = i;
        len += add;
        in_chunk++;
    }
    return count;
}

bool pi_chunk_halve(pi_client_t *c, size_t tried, long code) {
    if (tried <= 1)
        return false;
    size_t next = tried / 2 ? tried / 2 : 1;
    size_t cur = atomic_load(c->chunk);
    while (next < cur && !atomic_compare_exchange_weak(c->chunk, &cur, next)) {
    }
    if (!atomic_exchange(c->chunk_warned, true))
        pi_log("warn",
               "device %s: the server answered %ld to a request with %zu WebIds; using at most "
               "%zu per request",
               c->device, code, tried, next);
    return true;
}

/* ---- the client ----------------------------------------------------------------------------- */

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_init_once(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

int pi_client_init(pi_client_t *c, const pi_settings_t *s, const char *base_url,
                   pi_creds_t *creds, atomic_size_t *chunk, atomic_bool *chunk_warned,
                   const char *device, char *err, size_t errlen) {
    pthread_once(&g_curl_once, curl_init_once);
    memset(c, 0, sizeof *c);
    c->s = s;
    c->creds = creds;
    c->chunk = chunk;
    c->chunk_warned = chunk_warned;
    c->device = device;
    snprintf(c->base, sizeof c->base, "%s", base_url);
    size_t n = strlen(c->base);
    while (n && c->base[n - 1] == '/')
        c->base[--n] = '\0';
    if (s->ca_file[0]) {
        if (access(s->ca_file, R_OK) != 0) {
            snprintf(err, errlen, "tls.ca_file %s: %s", s->ca_file, strerror(errno));
            return -1;
        }
        snprintf(c->ca_bundle, sizeof c->ca_bundle, "%s", s->ca_file);
    } else {
        for (const char *const *p = SYSTEM_BUNDLES; *p; p++)
            if (access(*p, R_OK) == 0) {
                snprintf(c->ca_bundle, sizeof c->ca_bundle, "%s", *p);
                break;
            }
        if (!c->ca_bundle[0] && !s->insecure_skip_verify && strncmp(c->base, "https:", 6) == 0)
            pi_log("warn", "device %s: no trusted CA certificates found in the system store; "
                           "set tls.ca_file",
                   device);
    }
    if (s->auth == PI_AUTH_CERTIFICATE) {
        if (access(s->cert_file, R_OK) != 0) {
            snprintf(err, errlen, "auth.cert_file %s: %s", s->cert_file, strerror(errno));
            return -1;
        }
        if (access(s->key_file, R_OK) != 0) {
            snprintf(err, errlen, "auth.key_file %s: %s", s->key_file, strerror(errno));
            return -1;
        }
    }
    c->multi = curl_multi_init();
    if (!c->multi) {
        snprintf(err, errlen, "cannot create a curl multi handle");
        return -1;
    }
    curl_multi_setopt(c->multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                      (long)(s->max_concurrent_requests ? s->max_concurrent_requests : 1));
    return 0;
}

void pi_client_free(pi_client_t *c) {
    if (c->multi)
        curl_multi_cleanup(c->multi);
    c->multi = NULL;
}

char *pi_url(const pi_client_t *c, const char *path, const char *query) {
    pi_buf_t b = {0};
    pi_buf_add(&b, c->base);
    pi_buf_add(&b, "/");
    pi_buf_add(&b, path);
    if (query && *query) {
        pi_buf_add(&b, "?");
        pi_buf_add(&b, query);
    }
    if (b.oom) {
        pi_buf_free(&b);
        return NULL;
    }
    return b.s;
}

static size_t on_body(char *data, size_t size, size_t nmemb, void *ud) {
    pi_req_t *r = ud;
    size_t n = size * nmemb;
    if (r->resp_len + n + 1 > r->resp_cap) {
        size_t cap = r->resp_cap ? r->resp_cap : 4096;
        while (cap < r->resp_len + n + 1)
            cap *= 2;
        char *p = realloc(r->resp, cap);
        if (!p)
            return 0; /* aborts the transfer: CURLE_WRITE_ERROR */
        r->resp = p;
        r->resp_cap = cap;
    }
    memcpy(r->resp + r->resp_len, data, n);
    r->resp_len += n;
    r->resp[r->resp_len] = '\0';
    return n;
}

static long ms(double s) { return s > 0 ? (long)(s * 1000.0 + 0.5) : 0; }

/* Abandons a blocking transfer (the WebSocket upgrade) once the worker is stopping. */
static int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    atomic_bool *stop = ud;
    return atomic_load(stop) ? 1 : 0;
}

typedef int (*pi_vrfy_t)(void *, mbedtls_x509_crt *, int, uint32_t *);

/* A server certificate without a subject alternative name is refused, as rustls refuses it
 * (mbedTLS would match the host against the CN). The flag goes through curl's own callback,
 * which clears it when the host is not verified (tls.insecure_skip_verify). */
static int verify_san(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    pi_tls_hook_t *h = p;
    if (depth == 0 && crt->subject_alt_names.buf.p == NULL) {
        *flags |= MBEDTLS_X509_BADCERT_CN_MISMATCH;
        h->no_san = true;
    }
    return h->orig ? ((pi_vrfy_t)h->orig)(h->orig_p, crt, depth, flags) : 0;
}

static CURLcode on_ssl_ctx(CURL *curl, void *ssl_ctx, void *userptr) {
    (void)curl;
    mbedtls_ssl_config *conf = ssl_ctx;
    pi_tls_hook_t *h = userptr;
    h->orig = (void (*)(void))conf->f_vrfy;
    h->orig_p = conf->p_vrfy;
    h->no_san = false;
    mbedtls_ssl_conf_verify(conf, verify_san, h);
    return CURLE_OK;
}

/* The TLS and timeout options every handle of a client takes. */
static void common_options(pi_client_t *c, CURL *e, char *errbuf, pi_tls_hook_t *tls) {
    const pi_settings_t *s = c->s;
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    if (c->stop) {
        curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(e, CURLOPT_XFERINFODATA, c->stop);
        curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    }
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, ms(s->connect_timeout_s));
    if (s->insecure_skip_verify) {
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    } else if (c->ca_bundle[0]) {
        curl_easy_setopt(e, CURLOPT_CAINFO, c->ca_bundle);
    }
    memset(tls, 0, sizeof *tls);
    curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, on_ssl_ctx);
    curl_easy_setopt(e, CURLOPT_SSL_CTX_DATA, tls);
    if (s->auth == PI_AUTH_CERTIFICATE) {
        curl_easy_setopt(e, CURLOPT_SSLCERT, s->cert_file);
        curl_easy_setopt(e, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(e, CURLOPT_SSLKEY, s->key_file);
        curl_easy_setopt(e, CURLOPT_SSLKEYTYPE, "PEM");
    }
}

static int prepare(pi_client_t *c, pi_req_t *r) {
    if (r->easy)
        curl_easy_cleanup(r->easy);
    curl_slist_free_all(r->headers);
    r->headers = NULL;
    r->resp_len = 0;
    r->errbuf[0] = '\0';
    r->easy = curl_easy_init();
    if (!r->easy)
        return -1;
    CURL *e = r->easy;
    common_options(c, e, r->errbuf, &r->tls);
    curl_easy_setopt(e, CURLOPT_URL, r->url);
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, ms(c->s->request_timeout_s));
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(e, CURLOPT_PRIVATE, r);
    r->headers = curl_slist_append(r->headers, "Accept: application/json");
    if (r->body) {
        /* PI Web API's CSRF defence rejects a non-GET request without it (spec §6). */
        r->headers = curl_slist_append(r->headers, "X-Requested-With: XMLHttpRequest");
        r->headers = curl_slist_append(r->headers, "Content-Type: application/json");
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)strlen(r->body));
        curl_easy_setopt(e, CURLOPT_POSTFIELDS, r->body);
    }
    r->headers = with_auth(c->creds, r->headers);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, r->headers);
    return 0;
}

/* A failed transfer, named by its cause. */
static void transport_error(CURLcode rc, const char *errbuf, const pi_tls_hook_t *tls,
                            pi_error_t *e) {
    e->kind = rc == CURLE_OPERATION_TIMEDOUT ? PI_E_TIMEOUT : PI_E_TRANSPORT;
    if (tls && tls->no_san &&
        (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR))
        snprintf(e->text, sizeof e->text, "certificate has no subject alternative name");
    else if (errbuf && errbuf[0])
        snprintf(e->text, sizeof e->text, "%s", errbuf);
    else
        snprintf(e->text, sizeof e->text, "%s", curl_easy_strerror(rc));
    size_t n = strlen(e->text); /* mbedTLS's texts end in a newline */
    while (n && (e->text[n - 1] == '\n' || e->text[n - 1] == ' '))
        e->text[--n] = '\0';
}

/* Classify a completed transfer. Returns true when it is worth one retry (a 401 after the
 * token file changed). */
static bool classify(pi_client_t *c, pi_req_t *r, CURLcode rc, bool refused_is_auth) {
    memset(&r->err, 0, sizeof r->err);
    if (rc != CURLE_OK) {
        transport_error(rc, r->errbuf, &r->tls, &r->err);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &status);
    r->status = status;
    cJSON *json = r->resp_len ? pi_json_parse(r->resp, r->resp_len) : NULL;
    char errors[TDOT_REASON_MAX] = "";
    bool has_errors = json && pi_errors_of(json, errors, sizeof errors);
    if (status >= 200 && status <= 299) {
        if (!json)
            json = cJSON_CreateNull();
        /* A 200 can still carry a failure of the request as a whole. */
        const cJSON *ex =
            cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "WebException") : NULL;
        if (ex) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(ex, "StatusCode");
            r->err.kind = PI_E_STATUS;
            r->err.code = cJSON_IsNumber(code) && code->valuedouble >= 0
                              ? (long)code->valuedouble
                              : 500;
            if (!pi_errors_of(ex, r->err.text, sizeof r->err.text))
                r->err.text[0] = '\0';
            cJSON_Delete(json);
            return false;
        }
        r->json = json;
        return false;
    }
    cJSON_Delete(json);
    if (status == 401 && !r->retried && pi_creds_refresh(c->creds)) {
        r->retried = true;
        return true;
    }
    r->err.code = status;
    if (status == 401 || (status == 403 && refused_is_auth)) {
        r->err.kind = PI_E_UNAUTHORIZED;
    } else if (status == 414 || (status == 400 && !has_errors)) {
        r->err.kind = PI_E_TOO_LONG;
    } else {
        r->err.kind = PI_E_STATUS;
        snprintf(r->err.text, sizeof r->err.text, "%s", has_errors ? errors : "");
    }
    return false;
}

static bool stopping(const pi_client_t *c) { return c->stop && atomic_load(c->stop); }

void pi_http_run(pi_client_t *c, pi_req_t *reqs, size_t n, bool refused_is_auth) {
    size_t active = 0;
    for (size_t i = 0; i < n; i++) {
        pi_req_t *r = &reqs[i];
        cJSON_Delete(r->json);
        r->json = NULL;
        r->retried = false;
        memset(&r->err, 0, sizeof r->err);
        if (!r->url || prepare(c, r) != 0) {
            r->err.kind = PI_E_TRANSPORT;
            snprintf(r->err.text, sizeof r->err.text, "out of memory");
            continue;
        }
        if (curl_multi_add_handle(c->multi, r->easy) == CURLM_OK)
            active++;
        else {
            r->err.kind = PI_E_TRANSPORT;
            snprintf(r->err.text, sizeof r->err.text, "cannot start the request");
        }
    }
    while (active > 0) {
        int running = 0;
        curl_multi_perform(c->multi, &running);
        CURLMsg *m;
        int left;
        while ((m = curl_multi_info_read(c->multi, &left))) {
            if (m->msg != CURLMSG_DONE)
                continue;
            CURL *easy = m->easy_handle;
            CURLcode rc = m->data.result;
            pi_req_t *r = NULL;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&r);
            curl_multi_remove_handle(c->multi, easy);
            active--;
            if (classify(c, r, rc, refused_is_auth)) {
                if (prepare(c, r) == 0 && curl_multi_add_handle(c->multi, r->easy) == CURLM_OK) {
                    active++;
                } else {
                    r->err.kind = PI_E_UNAUTHORIZED;
                    r->err.code = 401;
                }
            }
        }
        if (active == 0)
            break;
        if (stopping(c)) {
            for (size_t i = 0; i < n; i++) {
                pi_req_t *r = &reqs[i];
                if (r->easy && r->err.kind == PI_E_NONE && !r->json) {
                    curl_multi_remove_handle(c->multi, r->easy);
                    r->err.kind = PI_E_STOPPED;
                }
            }
            break;
        }
        curl_multi_poll(c->multi, NULL, 0, 100, NULL);
    }
    for (size_t i = 0; i < n; i++) {
        if (reqs[i].easy) {
            curl_easy_cleanup(reqs[i].easy);
            reqs[i].easy = NULL;
        }
        curl_slist_free_all(reqs[i].headers);
        reqs[i].headers = NULL;
    }
}

void pi_req_free(pi_req_t *r) {
    if (r->easy)
        curl_easy_cleanup(r->easy);
    curl_slist_free_all(r->headers);
    cJSON_Delete(r->json);
    free(r->url);
    free(r->body);
    free(r->resp);
    memset(r, 0, sizeof *r);
}

int pi_http_get(pi_client_t *c, const char *path, const char *query, cJSON **out,
                pi_error_t *e) {
    pi_req_t r = {0};
    r.url = pi_url(c, path, query);
    pi_http_run(c, &r, 1, true);
    int rc = r.err.kind == PI_E_NONE ? 0 : -1;
    if (rc == 0) {
        *out = r.json;
        r.json = NULL;
    } else {
        *e = r.err;
    }
    pi_req_free(&r);
    return rc;
}

int pi_http_post(pi_client_t *c, const char *path, const char *query, const char *body,
                 long *status, pi_error_t *e) {
    pi_req_t r = {0};
    r.url = pi_url(c, path, query);
    r.body = strdup(body);
    pi_http_run(c, &r, 1, false);
    int rc = r.err.kind == PI_E_NONE ? 0 : -1;
    if (rc == 0)
        *status = r.status;
    else
        *e = r.err;
    pi_req_free(&r);
    return rc;
}

/* ---- the Channels WebSocket ---------------------------------------------------------------- */

int pi_ws_open(pi_client_t *c, const char *query, pi_ws_t **out, pi_error_t *e) {
    memset(e, 0, sizeof *e);
    char *url = pi_url(c, "streamsets/channel", query);
    /* The handle's TLS hook must outlive the handshake: it lives in the channel. */
    pi_ws_t *ws = calloc(1, sizeof *ws);
    pi_buf_t ws_url = {0};
    if (url && strncmp(url, "https://", 8) == 0) {
        pi_buf_add(&ws_url, "wss://");
        pi_buf_add(&ws_url, url + 8);
    } else if (url && strncmp(url, "http://", 7) == 0) {
        pi_buf_add(&ws_url, "ws://");
        pi_buf_add(&ws_url, url + 7);
    } else if (url) {
        pi_buf_add(&ws_url, url);
    }
    free(url);
    if (!ws || !ws_url.s || ws_url.oom) {
        free(ws);
        pi_buf_free(&ws_url);
        e->kind = PI_E_TRANSPORT;
        snprintf(e->text, sizeof e->text, "out of memory");
        return -1;
    }
    bool retried = false;
    for (;;) {
        char errbuf[CURL_ERROR_SIZE] = "";
        pi_req_t body = {0}; /* the refusal's body, for PI's Errors */
        CURL *easy = curl_easy_init();
        if (!easy) {
            free(ws);
            pi_buf_free(&ws_url);
            e->kind = PI_E_TRANSPORT;
            snprintf(e->text, sizeof e->text, "out of memory");
            return -1;
        }
        common_options(c, easy, errbuf, &ws->tls);
        curl_easy_setopt(easy, CURLOPT_URL, ws_url.s);
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "ws,wss");
        curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                         ms(c->s->connect_timeout_s + c->s->request_timeout_s));
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
        struct curl_slist *headers = with_auth(c->creds, NULL);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        CURLcode rc = curl_easy_perform(easy);
        curl_slist_free_all(headers);
        long code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
        free(body.resp);
        if (rc == CURLE_OK && code == 101) {
            curl_socket_t fd = CURL_SOCKET_BAD;
            curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, NULL);
            ws->easy = easy;
            ws->fd = fd;
            *out = ws;
            pi_buf_free(&ws_url);
            return 0;
        }
        curl_easy_cleanup(easy);
        if (code >= 200 && code != 101) {
            if (code == 401 && !retried && pi_creds_refresh(c->creds)) {
                retried = true;
                continue;
            }
            /* Only 401: a 403 to the upgrade, after REST requests with the same credentials
             * succeeded, is a refusal of the WebSocket (spec §5.2). curl does not read the body
             * of a refused upgrade, so PI's Errors text is not available here. */
            e->code = code;
            e->kind = code == 401 ? PI_E_UNAUTHORIZED : PI_E_STATUS;
        } else {
            transport_error(rc == CURLE_OK ? CURLE_RECV_ERROR : rc, errbuf, &ws->tls, e);
        }
        free(ws);
        pi_buf_free(&ws_url);
        return -1;
    }
}

static void ws_append(pi_ws_t *ws, const char *data, size_t n) {
    if (ws->len + n + 1 > ws->cap) {
        size_t cap = ws->cap ? ws->cap : 8192;
        while (cap < ws->len + n + 1)
            cap *= 2;
        char *p = realloc(ws->msg, cap);
        if (!p)
            return; /* the message is cut; the decoder will not parse it */
        ws->msg = p;
        ws->cap = cap;
    }
    memcpy(ws->msg + ws->len, data, n);
    ws->len += n;
    ws->msg[ws->len] = '\0';
}

pi_ws_event_t pi_ws_recv(pi_ws_t *ws, const char **msg, size_t *len, char *reason,
                         size_t rlen) {
    char buf[16384];
    if (ws->returned) {
        ws->len = 0;
        ws->returned = false;
    }
    for (;;) {
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(ws->easy, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN)
            return PI_WS_NONE;
        if (rc != CURLE_OK) {
            if (rc == CURLE_GOT_NOTHING)
                snprintf(reason, rlen, "the server closed the channel");
            else
                snprintf(reason, rlen, "channel: %s", curl_easy_strerror(rc));
            return PI_WS_CLOSED;
        }
        if (!meta)
            return PI_WS_NONE;
        if (meta->flags & CURLWS_CLOSE) {
            snprintf(reason, rlen, "the server closed the channel");
            return PI_WS_CLOSED;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
            /* curl answers pings itself; a control frame only proves the server alive */
            if (meta->bytesleft == 0)
                return PI_WS_FRAME;
            continue;
        }
        /* A data frame, possibly one fragment of a message: the message's kind is its first
         * frame's. */
        if (ws->kind == 0)
            ws->kind = meta->flags & CURLWS_TEXT ? 1 : 2;
        if (ws->kind == 1)
            ws_append(ws, buf, got);
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            int kind = ws->kind;
            ws->kind = 0;
            ws->returned = true;
            if (kind != 1)
                return PI_WS_FRAME; /* binary: alive, nothing to deliver */
            *msg = ws->msg ? ws->msg : "";
            *len = ws->len;
            return PI_WS_MESSAGE;
        }
    }
}

void pi_ws_close(pi_ws_t *ws) {
    if (!ws)
        return;
    size_t sent = 0;
    (void)curl_ws_send(ws->easy, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(ws->easy);
    free(ws->msg);
    free(ws);
}
