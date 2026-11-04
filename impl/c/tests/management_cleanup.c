/* Inject file I/O failures without requiring a full disk or an MQTT broker. */
#include <errno.h>
#include <stdio.h>
#include <mosquitto.h>

static FILE *test_fopen(const char *path, const char *mode);
static int test_fputs(const char *text, FILE *fp);
static int test_fclose(FILE *fp);
static int test_publish(struct mosquitto *mosq, int *mid, const char *topic,
                        int len, const void *payload, int qos, bool retain);

#define fopen test_fopen
#define fputs test_fputs
#define fclose test_fclose
#define mosquitto_publish test_publish
#include "../sdk/src/runtime.c"
#undef mosquitto_publish
#undef fclose
#undef fputs
#undef fopen

static int failures, opens, closes;
static enum { FAIL_OPEN, FAIL_WRITE, FAIL_CLOSE, SUCCEED } fault;
static char last_status[32], last_reason[TDOT_ERR_MAX];

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static FILE *test_fopen(const char *path, const char *mode) {
    if (fault == FAIL_OPEN) {
        errno = EACCES;
        return NULL;
    }
    FILE *fp = fopen(path, mode);
    if (fp)
        opens++;
    return fp;
}

static int test_fputs(const char *text, FILE *fp) {
    if (fault == FAIL_WRITE) {
        errno = ENOSPC;
        return EOF;
    }
    return fputs(text, fp);
}

static int test_fclose(FILE *fp) {
    closes++;
    int rc = fclose(fp);
    /* Closing must not replace the original write error. */
    errno = EIO;
    return fault == FAIL_CLOSE ? EOF : rc;
}

static int test_publish(struct mosquitto *mosq, int *mid, const char *topic,
                        int len, const void *payload, int qos, bool retain) {
    (void)mosq, (void)mid, (void)topic, (void)len, (void)qos, (void)retain;
    cJSON *doc = cJSON_Parse(payload);
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(doc, "status");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(doc, "reason");
    snprintf(last_status, sizeof last_status, "%s",
             cJSON_IsString(status) ? status->valuestring : "");
    snprintf(last_reason, sizeof last_reason, "%s",
             cJSON_IsString(reason) ? reason->valuestring : "");
    cJSON_Delete(doc);
    return MOSQ_ERR_SUCCESS;
}

static int configure(tdot_connector_t *self, tdot_config_t *cfg, char *err, size_t len) {
    (void)self, (void)cfg, (void)err, (void)len;
    return 0;
}

int main(void) {
    char path[] = "/tmp/tdot-management-cleanup-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        perror("fdopen");
        close(fd);
        unlink(path);
        return 1;
    }
    fputs("[connector]\nprotocol = 'fake'\npoll_interval = '10s'\n", fp);
    if (fclose(fp) != 0) {
        perror("fclose");
        unlink(path);
        return 1;
    }
    char err[256];
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    if (!cfg) {
        fprintf(stderr, "%s\n", err);
        unlink(path);
        return 1;
    }
    tdot_connector_t conn = {.configure = configure};
    rt_t rt = {.cfg = cfg, .conn = &conn, .output = TDOT_OUTPUT_MQTT};
    cJSON *req = cJSON_Parse(
        "{\"target\":\"connector\",\"config\":{\"poll_interval\":\"20s\"}}");
    for (fault = FAIL_OPEN; fault <= FAIL_CLOSE; fault++) {
        opens = closes = 0;
        int expected_errno = fault == FAIL_OPEN ? EACCES : fault == FAIL_WRITE ? ENOSPC : EIO;
        for (int i = 0; i < 10; i++) {
            handle_management(&rt, "unused", "set-config", req);
            CHECK(strcmp(last_status, "failed") == 0);
            CHECK(strstr(last_reason, strerror(expected_errno)) != NULL);
            CHECK(cfg->poll_interval_s == 10);
        }
        CHECK(opens == (fault == FAIL_OPEN ? 0 : 10));
        CHECK(closes == opens);
        tdot_config_t *saved = tdot_config_load(path, err, sizeof err);
        CHECK(saved && saved->poll_interval_s == 10);
        tdot_config_free(saved);
    }
    fault = SUCCEED;
    opens = closes = 0;
    handle_management(&rt, "unused", "set-config", req);
    CHECK(strcmp(last_status, "successful") == 0);
    CHECK(opens == 1 && closes == 1);
    CHECK(cfg->poll_interval_s == 20);
    tdot_config_t *saved = tdot_config_load(path, err, sizeof err);
    CHECK(saved && saved->poll_interval_s == 20);
    tdot_config_free(saved);
    cJSON_Delete(req);
    tdot_config_free(cfg);
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    unlink(path);
    return failures ? 1 : 0;
}
