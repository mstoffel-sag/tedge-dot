/* Concurrent reads (contract §5.6): the endpoint and total budgets. The same
 * cases as impl/rust/crates/sdk/src/concurrency.rs's tests. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/concurrency.h"
#include "tedge_dot/config.h"

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

static tdot_config_t *load(const char *text) {
    char path[] = "/tmp/tedge-dot-concurrency-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
    fputs(text, fp);
    fclose(fp);
    char err[512];
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    unlink(path);
    if (!cfg)
        printf("FAIL config: %s\n", err);
    return cfg;
}

static const char *TWO_ON_ONE =
    "[connector]\nprotocol = \"bench\"\n"
    "[[device]]\nname = \"a\"\nprotocol_address = {}\n"
    "[[device]]\nname = \"b\"\nprotocol_address = {}\nmax_inflight = 2\n"
    "[[device]]\nname = \"c\"\nprotocol_address = {}\n";

static int shared(void *ctx, const tdot_device_t *dev, char *buf, size_t len) {
    (void)ctx;
    snprintf(buf, len, "%s", strcmp(dev->name, "c") == 0 ? "other" : "plc");
    return 0;
}

static int all_plc(void *ctx, const tdot_device_t *dev, char *buf, size_t len) {
    (void)ctx;
    (void)dev;
    snprintf(buf, len, "plc");
    return 0;
}

static int serial_bus(void *ctx, const tdot_device_t *dev, char *buf, size_t len) {
    (void)ctx;
    (void)dev;
    snprintf(buf, len, "/dev/ttyUSB0");
    return 1;
}

static void the_smallest_budget_of_an_endpoint_wins(tdot_config_t *cfg) {
    tdot_budgets_t *b = tdot_budgets_new(cfg, shared, NULL);
    CHECK(tdot_budgets_configured(b, 0) == 2, "a: %zu", tdot_budgets_configured(b, 0));
    CHECK(strcmp(tdot_budgets_endpoint(b, 1), "plc") == 0, "b on plc");
    CHECK(tdot_budgets_configured(b, 2) == TDOT_DEFAULT_MAX_INFLIGHT, "c: default");
    cJSON *d = tdot_budgets_describe(b);
    cJSON *want = cJSON_Parse("{\"endpoint\":\"plc\",\"devices\":[\"a\",\"b\"],\"max_inflight\":2}");
    CHECK(cJSON_Compare(cJSON_GetArrayItem(d, 0), want, true), "describe");
    cJSON_Delete(d);
    cJSON_Delete(want);
    tdot_budgets_free(b);
}

static void the_module_cap_wins(tdot_config_t *cfg) {
    tdot_budgets_t *b = tdot_budgets_new(cfg, serial_bus, NULL);
    CHECK(tdot_budgets_configured(b, 0) == 1, "serial: %zu", tdot_budgets_configured(b, 0));
    tdot_budgets_free(b);
    b = tdot_budgets_new(cfg, NULL, NULL);
    CHECK(strcmp(tdot_budgets_endpoint(b, 0), "a") == 0, "own endpoint");
    CHECK(tdot_budgets_configured(b, 1) == 2, "b's own");
    tdot_budgets_free(b);
}

typedef struct {
    tdot_budgets_t *b;
    size_t index, wanted, got;
} waiter_t;

static void *acquire_one(void *arg) {
    waiter_t *w = arg;
    tdot_inflight_t f;
    tdot_budgets_acquire(w->b, w->index, w->wanted, &f);
    w->got = tdot_inflight_allowance(&f);
    tdot_inflight_release(&f);
    return NULL;
}

static void a_read_takes_one_then_what_is_free(tdot_config_t *cfg) {
    tdot_budgets_t *b = tdot_budgets_new(cfg, all_plc, NULL);
    tdot_inflight_t first;
    tdot_budgets_acquire(b, 0, 4, &first);
    CHECK(tdot_inflight_allowance(&first) == 2, "first: %zu", tdot_inflight_allowance(&first));
    waiter_t w = {.b = b, .index = 2, .wanted = 4};
    pthread_t t;
    pthread_create(&t, NULL, acquire_one, &w);
    usleep(20000);
    CHECK(w.got == 0, "waits while the endpoint is full");
    tdot_inflight_release(&first);
    pthread_join(t, NULL);
    CHECK(w.got == 2, "then gets two: %zu", w.got);
    tdot_budgets_free(b);
}

static void the_total_bounds_every_endpoint(void) {
    tdot_config_t *cfg = load("[connector]\nprotocol = \"bench\"\nmax_inflight_total = 3\n"
                              "[[device]]\nname = \"a\"\nprotocol_address = {}\n"
                              "[[device]]\nname = \"b\"\nprotocol_address = {}\n");
    if (!cfg)
        return;
    tdot_budgets_t *b = tdot_budgets_new(cfg, NULL, NULL);
    tdot_inflight_t a;
    tdot_budgets_acquire(b, 0, 4, &a);
    CHECK(tdot_inflight_allowance(&a) == 3, "a: %zu", tdot_inflight_allowance(&a));
    waiter_t w = {.b = b, .index = 1, .wanted = 4};
    pthread_t t;
    pthread_create(&t, NULL, acquire_one, &w);
    usleep(20000);
    CHECK(w.got == 0, "b waits for the total");
    tdot_inflight_release(&a);
    pthread_join(t, NULL);
    CHECK(w.got == 3, "b: %zu", w.got);
    tdot_budgets_free(b);
    tdot_config_free(cfg);
}

static void busy_halves_and_successes_regrow(void) {
    tdot_config_t *cfg = load("[connector]\nprotocol = \"bench\"\nmax_inflight = 8\n"
                              "[[device]]\nname = \"a\"\nprotocol_address = {}\n");
    if (!cfg)
        return;
    tdot_budgets_t *b = tdot_budgets_new(cfg, NULL, NULL);
    tdot_inflight_t f;
    tdot_budgets_acquire(b, 0, 8, &f);
    CHECK(tdot_inflight_allowance(&f) == 8, "eight");
    tdot_inflight_busy(&f);
    CHECK(tdot_budgets_effective(b, 0) == 4, "halved: %zu", tdot_budgets_effective(b, 0));
    CHECK(tdot_inflight_current(&f) == 4, "current follows");
    tdot_inflight_busy(&f);
    tdot_inflight_busy(&f);
    tdot_inflight_busy(&f);
    CHECK(tdot_budgets_effective(b, 0) == 1, "never below one");
    for (int i = 0; i < TDOT_REGROW_AFTER; i++)
        tdot_inflight_ok(&f);
    CHECK(tdot_budgets_effective(b, 0) == 2, "regrown: %zu", tdot_budgets_effective(b, 0));
    tdot_inflight_timed_out(&f, false);
    CHECK(tdot_budgets_effective(b, 0) == 2, "a lone timeout is a link failure");
    tdot_inflight_timed_out(&f, true);
    CHECK(tdot_budgets_effective(b, 0) == 1, "a timeout behind others backs off");
    tdot_inflight_release(&f);
    tdot_budgets_free(b);
    tdot_config_free(cfg);
}

int main(void) {
    tdot_config_t *cfg = load(TWO_ON_ONE);
    if (cfg) {
        the_smallest_budget_of_an_endpoint_wins(cfg);
        the_module_cap_wins(cfg);
        a_read_takes_one_then_what_is_free(cfg);
        tdot_config_free(cfg);
    }
    the_total_bounds_every_endpoint();
    busy_halves_and_successes_regrow();
    printf("concurrency: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
