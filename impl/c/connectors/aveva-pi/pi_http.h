/* AVEVA PI connector -- the HTTP layer on libcurl (spec §3.1, §7). Mirrors
 * impl/rust/crates/connector-aveva-pi/src/client.rs: authentication, TLS, timeouts, the CSRF
 * header, request chunking, the Channels WebSocket, and PI's error bodies classified the way
 * the link status needs them. Nothing here knows about points or samples.
 *
 * Threads: a pi_client_t (and every handle it owns) is used by one thread at a time -- the
 * runtime thread has one per device, and so has the device's push worker. The credentials are
 * shared by both and locked, because a 401 re-reads the token file.
 */
#ifndef TDOT_PI_HTTP_H
#define TDOT_PI_HTTP_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>

#include "cjson/cJSON.h"
#include "tedge_dot/model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PI_PATH_MAX 512

typedef enum {
    PI_AUTH_NONE = 0,
    PI_AUTH_BASIC,
    PI_AUTH_BEARER,
    PI_AUTH_CERTIFICATE,
} pi_auth_method_t;

/* The settings a device's requests run with: [connection] merged with the device's overrides
 * (config.rs `Settings`). Flat, so it lives in the device's configure-time state. */
typedef struct {
    pi_auth_method_t auth;
    char username[256];
    char password_file[PI_PATH_MAX];
    char token_file[PI_PATH_MAX];
    char cert_file[PI_PATH_MAX];
    char key_file[PI_PATH_MAX];
    char ca_file[PI_PATH_MAX];
    bool insecure_skip_verify;
    double request_timeout_s;
    double connect_timeout_s;
    double resolve_retry_s;
    size_t max_web_ids_per_request;
    size_t max_url_bytes;
    size_t max_concurrent_requests;
    char update_option[24];
    char buffer_option[24];
    char state_dir[PI_PATH_MAX];
    bool backfill; /* false: `backfill = false` */
    double max_age_s;
    size_t max_events;
    size_t page_size;
    size_t live_buffer;
    double cursor_flush_s;
    unsigned heartbeat_rate;
    double polling_interval_s;
    bool include_initial_values;
} pi_settings_t;

void pi_settings_default(pi_settings_t *s);

/* What went wrong with a request (client.rs `HttpError`). */
typedef enum {
    PI_E_NONE = 0,
    PI_E_UNAUTHORIZED, /* 401, or 403 on a read: the credentials were refused */
    PI_E_TOO_LONG,     /* 414, or a 400 without a PI Errors body */
    PI_E_STATUS,       /* any other unsuccessful status, with PI's Errors text */
    PI_E_TIMEOUT,
    PI_E_TRANSPORT,    /* DNS, TCP, TLS or I/O, with curl's cause */
    PI_E_STOPPED,      /* abandoned: the worker is stopping */
} pi_err_kind_t;

typedef struct {
    pi_err_kind_t kind;
    long code;                   /* the HTTP status, when there is one */
    char text[TDOT_REASON_MAX];  /* PI's Errors (status) or the cause (timeout, transport) */
} pi_error_t;

/* The error as the link status and the command results name it (client.rs Display):
 * "unauthorized", "HTTP 414: request too long", "HTTP 404: <errors>", "timeout: ...". */
void pi_error_format(const pi_error_t *e, char *dst, size_t cap);
/* Whether the failure says the device is unreachable (the link degrades), rather than that one
 * request was refused. */
bool pi_error_is_link_failure(const pi_error_t *e);

/* How requests authenticate, shared by a device's threads. */
typedef struct {
    pthread_mutex_t mu;
    pi_auth_method_t method;
    char *header;    /* "Authorization: ..." or NULL */
    char token_file[PI_PATH_MAX];
} pi_creds_t;

/* Read the secret files (trailing newlines stripped). 0, or -1 with err naming the key and the
 * file, never its content. */
int pi_creds_init(pi_creds_t *c, const pi_settings_t *s, char *err, size_t errlen);
void pi_creds_free(pi_creds_t *c);
/* After a 401: re-read the token file. True when it held a different token, which is then
 * worth one retry. */
bool pi_creds_refresh(pi_creds_t *c);

/* A PI Web API client of one thread. */
typedef struct {
    const pi_settings_t *s;
    char base[1024]; /* the PI Web API root, without a trailing / */
    pi_creds_t *creds;
    char ca_bundle[PI_PATH_MAX]; /* "" when none was found */
    CURLM *multi;                /* keeps the connections between requests */
    atomic_bool *stop;           /* abandon requests in flight once set; NULL: never */
    /* The device's chunk size, halved on a "too long" answer (shared by its threads). */
    atomic_size_t *chunk;
    atomic_bool *chunk_warned;
    const char *device;
} pi_client_t;

int pi_client_init(pi_client_t *c, const pi_settings_t *s, const char *base_url,
                   pi_creds_t *creds, atomic_size_t *chunk, atomic_bool *chunk_warned,
                   const char *device, char *err, size_t errlen);
void pi_client_free(pi_client_t *c);

/* The hook that refuses a server certificate without a subject alternative name (rustls does;
 * mbedTLS falls back to the CN): curl's own verify callback, wrapped. */
typedef struct {
    void (*orig)(void); /* curl's mbedTLS verify callback */
    void *orig_p;
    bool no_san;        /* the last handshake was refused for it */
} pi_tls_hook_t;

/* One request. Fill url (and body for a POST) before pi_http_run. */
typedef struct {
    char *url;              /* owned */
    char *body;             /* owned; NULL: GET */
    long status;            /* the HTTP status of a successful answer */
    cJSON *json;            /* the body of a successful answer (a JSON null when empty) */
    pi_error_t err;         /* kind PI_E_NONE on success */
    /* internal */
    CURL *easy;
    struct curl_slist *headers;
    char *resp;
    size_t resp_len, resp_cap;
    bool retried;
    char errbuf[CURL_ERROR_SIZE];
    pi_tls_hook_t tls;
} pi_req_t;

/* Run the requests in parallel on the client's multi handle, at most max_concurrent_requests
 * at once; each ends within request_timeout. `refused_is_auth`: a 403 means the credentials
 * were refused (a read), rather than this one request (a write). */
void pi_http_run(pi_client_t *c, pi_req_t *reqs, size_t n, bool refused_is_auth);
void pi_req_free(pi_req_t *r);

/* `GET path?query`: one request, its JSON in *out (caller cJSON_Delete()s). 0, or -1 with *e. */
int pi_http_get(pi_client_t *c, const char *path, const char *query, cJSON **out,
                pi_error_t *e);
/* `POST path?query` with a JSON body and the CSRF header. 0 with *status, or -1 with *e. */
int pi_http_post(pi_client_t *c, const char *path, const char *query, const char *body,
                 long *status, pi_error_t *e);

/* The URL of `path` (relative to the root) with an encoded query. Caller frees. */
char *pi_url(const pi_client_t *c, const char *path, const char *query);

/* Percent-encode a query component (RFC 3986 unreserved characters stay), appended to a
 * growable buffer. */
typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
} pi_buf_t;
void pi_buf_add(pi_buf_t *b, const char *s);
void pi_buf_add_n(pi_buf_t *b, const char *s, size_t n);
/* `&key=value` (no `&` when the buffer is empty), both encoded. */
void pi_buf_pair(pi_buf_t *b, const char *key, const char *value);
void pi_buf_free(pi_buf_t *b);

/* Split ids[0..n) into chunks of at most `chunk` ids whose URL stays within max_url_bytes
 * (the fixed part of the URL is `fixed` bytes): starts[k] is the first id of chunk k, and the
 * function returns the number of chunks (starts has room for n). */
size_t pi_chunks(const char *const *ids, size_t n, size_t chunk, size_t max_url_bytes,
                 size_t fixed, size_t *starts);

/* Halve the device's chunk size after a "too long" answer for a chunk of `tried` ids, logging
 * once. False when it cannot shrink. */
bool pi_chunk_halve(pi_client_t *c, size_t tried, long code);

/* ---- the Channels WebSocket ---------------------------------------------------------------- */

typedef struct {
    CURL *easy;
    curl_socket_t fd;
    char *msg;   /* the message being assembled */
    size_t len, cap;
    int kind;      /* of the message being assembled: 0 none, 1 text, 2 binary */
    bool returned; /* msg was handed out by the previous pi_ws_recv */
    pi_tls_hook_t tls;
} pi_ws_t;

typedef enum {
    PI_WS_NONE = 0, /* nothing complete yet */
    PI_WS_FRAME,    /* a non-text frame (alive, nothing to deliver) */
    PI_WS_MESSAGE,  /* a text message: *msg / *len, valid until the next call */
    PI_WS_CLOSED,   /* the server closed the channel; reason filled */
} pi_ws_event_t;

/* Open `streamsets/channel?<query>`. An answer other than 101 is PI_E_STATUS (the caller decides
 * whether to fall back); a 401 after the token re-read is PI_E_UNAUTHORIZED. */
int pi_ws_open(pi_client_t *c, const char *query, pi_ws_t **out, pi_error_t *e);
/* Non-blocking read of the next frame. */
pi_ws_event_t pi_ws_recv(pi_ws_t *ws, const char **msg, size_t *len, char *reason,
                         size_t rlen);
/* Send a close frame (best effort) and release the handle. */
void pi_ws_close(pi_ws_t *ws);

#ifdef __cplusplus
}
#endif

#endif
