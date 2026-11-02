/* Concurrent reads (contract §5.6) under the C runtime: a module with
 * concurrent_reads has its devices read on the runtime's workers, so a slow
 * device does not delay the others, and no device is read twice at once. Runs
 * the runtime in stdout mode against a fake module, as poll_batch.c does. The
 * same case as impl/rust/crates/ot-conformance/tests/concurrent_reads.rs. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/connector.h"
#include "tedge_dot/runtime.h"

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* Reads in flight per device (0: slow, 1: fast) and the most ever seen. */
static _Atomic int now_reading[2];
static _Atomic int peak;

static int fake_configure(tdot_connector_t *self, tdot_config_t *cfg, char *err, size_t errlen) {
    (void)self, (void)cfg, (void)err, (void)errlen;
    return 0;
}

static int fake_connect(tdot_connector_t *self, tdot_device_t *dev, char *err, size_t errlen) {
    (void)self, (void)dev, (void)err, (void)errlen;
    return 0;
}

static int fake_read(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                     tdot_sample_t *out) {
    (void)self, (void)pt;
    int d = strcmp(dev->name, "slow") == 0 ? 0 : 1;
    int n = atomic_fetch_add(&now_reading[d], 1) + 1;
    int p = atomic_load(&peak);
    while (n > p && !atomic_compare_exchange_weak(&peak, &p, n)) {
    }
    if (d == 0) {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);
    }
    atomic_fetch_sub(&now_reading[d], 1);
    out->value.kind = TDOT_VAL_NUM;
    out->value.num = 1;
    return 0;
}

static int fake_write(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      const tdot_value_t *value, char *err, size_t errlen) {
    (void)self, (void)dev, (void)pt, (void)value;
    snprintf(err, errlen, "read-only");
    return -1;
}

static void fake_disconnect(tdot_connector_t *self, tdot_device_t *dev) { (void)self, (void)dev; }

static void fake_destroy(tdot_connector_t *self) { free(self); }

int main(void) {
    char dir[] = "/tmp/tdot-concurrent-reads-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char cfg_path[256], out_path[256];
    snprintf(cfg_path, sizeof cfg_path, "%s/fake.toml", dir);
    snprintf(out_path, sizeof out_path, "%s/out.jsonl", dir);
    FILE *fp = fopen(cfg_path, "w");
    fputs("[connector]\nprotocol = \"fake\"\npoll_interval = \"100ms\"\n"
          "operation_timeout = \"10s\"\n"
          "[[device]]\nname = \"slow\"\nprotocol_address = {}\n"
          "  [[device.point]]\n  id = \"p\"\n  datatype = \"uint16\"\n  address = {}\n"
          "[[device]]\nname = \"fast\"\nprotocol_address = {}\n"
          "  [[device.point]]\n  id = \"p\"\n  datatype = \"uint16\"\n  address = {}\n",
          fp);
    fclose(fp);

    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        if (!freopen(out_path, "w", stdout))
            _exit(3);
        char err[256];
        tdot_config_t *cfg = tdot_config_load(cfg_path, err, sizeof err);
        if (!cfg)
            _exit(4);
        tdot_connector_t *conn = calloc(1, sizeof *conn);
        conn->protocol = "fake";
        conn->configure = fake_configure;
        conn->connect_device = fake_connect;
        conn->read_point = fake_read;
        conn->write_point = fake_write;
        conn->disconnect_device = fake_disconnect;
        conn->destroy = fake_destroy;
        conn->concurrent_reads = true;
        tdot_run_opts_t opts = {.output = TDOT_OUTPUT_STDOUT, .duration_s = 1.5};
        int rc = tdot_runtime_run(conn, cfg, &opts);
        fflush(stdout);
        /* The peak travels back as the exit code. */
        _exit(rc != 0 ? 5 : 10 + atomic_load(&peak));
    }
    int status = 0;
    waitpid(child, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(code >= 10, "the runtime run failed (exit %d)", code);
    CHECK(code == 11, "a device had %d reads in flight at once", code - 10);

    int fast = 0, slow = 0;
    fp = fopen(out_path, "r");
    char line[4096];
    while (fp && fgets(line, sizeof line, fp)) {
        cJSON *sample = cJSON_Parse(line);
        const cJSON *device = cJSON_GetObjectItem(sample, "device");
        if (cJSON_IsString(device)) {
            if (strcmp(device->valuestring, "fast") == 0)
                fast++;
            else if (strcmp(device->valuestring, "slow") == 0)
                slow++;
        }
        cJSON_Delete(sample);
    }
    if (fp)
        fclose(fp);
    /* 100 ms polls over 1.5 s, at most one per pass of the loop (a 200 ms tick
     * in stdout mode), while the slow device's 1 s reads are in flight: about
     * 7. A runtime that waited for the slow reads would publish two. */
    CHECK(fast >= 5, "the fast device was polled %d times", fast);
    CHECK(slow >= 1 && slow <= 2, "the slow device was read %d times", slow);
    unlink(out_path);
    unlink(cfg_path);
    rmdir(dir);
    printf("concurrent-reads: %s (fast %d, slow %d)\n", failures ? "FAILED" : "all checks passed",
           fast, slow);
    return failures ? 1 : 0;
}
