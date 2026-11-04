/* AVEVA PI: delivery cursors (the cases of cursor.rs: dedup at one timestamp, round trip with
 * the WebId check, a corrupt file, an older event) and the push ring: a producer that queues
 * far more events than the ring's bound while a slow consumer drains loses nothing and keeps
 * the order -- what a catch-up longer than the bound does (spec §5.4 rule 5).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "pi_cursor.h"
#include "pi_ring.h"
#include "pi_value.h"

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

static pi_time_t at(const char *text) {
    pi_time_t t = {0};
    CHECK(pi_time_parse(text, &t), "parse %s", text);
    return t;
}

static void times(void) {
    pi_time_t a = at("2026-11-02T08:00:00.1234567Z");
    CHECK(a.nsec == 123456700, "nsec %d", a.nsec);
    CHECK(pi_time_cmp(a, at("2026-11-02T09:00:00.1234567+01:00")) == 0, "offsets");
    CHECK(pi_time_cmp(at("1970-01-01T00:00:00Z"), (pi_time_t){0, 0}) == 0, "epoch");
    CHECK(at("2026-11-02T08:00:00Z").sec == 1793606400, "sec %lld",
          (long long)at("2026-11-02T08:00:00Z").sec);
    pi_time_t t;
    CHECK(!pi_time_parse("2026-11-02 08:00:00Z", &t), "a space is not RFC 3339");
    CHECK(!pi_time_parse("2026-11-02T08:00:00", &t), "an offset is required");
    char text[64];
    pi_time_format(at("2026-11-02T08:00:00.5Z"), text, sizeof text);
    CHECK(strcmp(text, "2026-11-02T08:00:00.5Z") == 0, "format %s", text);
    pi_time_format(pi_time_minus(at("2026-11-02T08:00:00Z"), 0.25), text, sizeof text);
    CHECK(strcmp(text, "2026-11-02T07:59:59.75Z") == 0, "minus %s", text);
}

/* cursor.rs dedup_keeps_several_events_at_one_time */
static void dedup(void) {
    pi_cursor_t c = {0};
    pi_time_t t = at("2026-11-02T08:00:00.1234567Z");
    CHECK(pi_cursor_admits(&c, t, "1"), "no cursor admits all");
    pi_cursor_advance(&c, "2026-11-02T08:00:00.1234567Z", t, "1");
    CHECK(!pi_cursor_admits(&c, t, "1"), "same time, same value");
    CHECK(pi_cursor_admits(&c, t, "2"), "same time, another value");
    pi_cursor_advance(&c, "2026-11-02T08:00:00.1234567Z", t, "2");
    CHECK(!pi_cursor_admits(&c, t, "2"), "2 delivered");
    CHECK(c.nvalues == 2, "two values at one time");
    CHECK(pi_cursor_admits(&c, at("2026-11-02T08:00:00.1234568Z"), "1"), "later");
    CHECK(!pi_cursor_admits(&c, at("2026-11-02T07:59:59Z"), "9"), "earlier");
    /* an_older_event_does_not_move_the_cursor_back */
    pi_cursor_advance(&c, "2026-11-02T07:00:00Z", at("2026-11-02T07:00:00Z"), "3");
    CHECK(strcmp(c.ts, "2026-11-02T08:00:00.1234567Z") == 0, "cursor moved back to %s", c.ts);
    pi_cursor_advance(&c, "2026-11-02T09:00:00Z", at("2026-11-02T09:00:00Z"), "4");
    CHECK(c.nvalues == 1 && strcmp(c.values[0], "4") == 0, "a later event replaces the values");
    pi_cursor_clear(&c);
}

static char g_dir[64];

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

/* cursor.rs round_trip_and_web_id_check */
static void round_trip(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/svc/pi.json", g_dir);
    const char *ids[] = {"a", "b"};
    const char *web_ids[] = {"W1", "W2"};
    pi_store_t s;
    CHECK(pi_store_load(&s, path, ids, web_ids, 2, 0) == 0, "load");
    pi_time_t t = at("2026-11-02T08:00:00Z");
    pi_cursor_advance(&s.cur[0], "2026-11-02T08:00:00Z", t, "1.5");
    pi_cursor_advance(&s.cur[1], "2026-11-02T08:00:00Z", t, "\"x\"");
    s.dirty = true;
    pi_store_flush(&s);
    pi_store_free(&s);
    const char *text = slurp(path);
    CHECK(text && strcmp(text, "{\"points\":{\"a\":{\"ts\":\"2026-11-02T08:00:00Z\",\"values\":[1.5],"
                               "\"web_id\":\"W1\"},\"b\":{\"ts\":\"2026-11-02T08:00:00Z\","
                               "\"values\":[\"x\"],\"web_id\":\"W2\"}},\"version\":1}") == 0,
          "file %s", text ? text : "(missing)");
    const char *changed[] = {"W1", "W-changed"};
    CHECK(pi_store_load(&s, path, ids, changed, 2, 0) == 0, "reload");
    CHECK(s.cur[0].set && s.cur[0].nvalues == 1 && strcmp(s.cur[0].values[0], "1.5") == 0,
          "a kept");
    CHECK(!s.cur[1].set, "a re-addressed point starts live");
    CHECK(s.dirty, "the dropped point is written out next time");
    pi_store_free(&s);
    /* a point no longer configured is dropped on the next write */
    const char *only[] = {"a"};
    CHECK(pi_store_load(&s, path, only, web_ids, 1, 0) == 0, "reload one");
    CHECK(!s.dirty, "nothing changed for the one point configured");
    s.dirty = true;
    pi_store_flush(&s);
    pi_store_free(&s);
    text = slurp(path);
    CHECK(text && !strstr(text, "\"b\""), "b dropped: %s", text ? text : "");
    unlink(path);
    snprintf(path, sizeof path, "%s/svc", g_dir);
    rmdir(path);
}

/* cursor.rs corrupt_file_starts_live_and_is_replaced */
static void corrupt_file(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/corrupt.json", g_dir);
    FILE *fp = fopen(path, "w");
    fputs("{not json", fp);
    fclose(fp);
    const char *ids[] = {"a"};
    const char *web_ids[] = {"W1"};
    pi_store_t s;
    CHECK(pi_store_load(&s, path, ids, web_ids, 1, 0) == 0, "load");
    CHECK(!s.cur[0].set, "starts live");
    pi_cursor_advance(&s.cur[0], "2026-11-02T08:00:00Z", at("2026-11-02T08:00:00Z"), "1");
    s.dirty = true;
    pi_store_maybe_flush(&s);
    pi_store_free(&s);
    cJSON *back = cJSON_Parse(slurp(path));
    CHECK(back && cJSON_GetObjectItem(back, "version")->valueint == 1, "version");
    const cJSON *a = cJSON_GetObjectItem(cJSON_GetObjectItem(back, "points"), "a");
    CHECK(a && strcmp(cJSON_GetObjectItem(a, "web_id")->valuestring, "W1") == 0, "web_id");
    cJSON_Delete(back);
    /* another version: starts live too */
    fp = fopen(path, "w");
    fputs("{\"version\":2,\"points\":{}}", fp);
    fclose(fp);
    CHECK(pi_store_load(&s, path, ids, web_ids, 1, 0) == 0 && !s.cur[0].set, "version 2");
    pi_store_free(&s);
    /* maybe_flush waits for cursor_flush */
    CHECK(pi_store_load(&s, path, ids, web_ids, 1, 3600) == 0, "load");
    pi_cursor_advance(&s.cur[0], "2026-11-02T09:00:00Z", at("2026-11-02T09:00:00Z"), "1");
    s.dirty = true;
    pi_store_maybe_flush(&s);
    CHECK(strstr(slurp(path), "09:00") == NULL, "flushed before cursor_flush");
    pi_store_flush(&s);
    CHECK(strstr(slurp(path), "09:00") != NULL, "flush writes");
    pi_store_free(&s);
    unlink(path);
}

/* ---- the ring ------------------------------------------------------------------------------- */

#define PRODUCED (3 * PI_RING_BOUND_MIN + 12345)

typedef struct {
    pi_ring_t *ring;
    unsigned long idles;
    int failed;
} producer_t;

static bool count_idle(void *ctx) {
    ((producer_t *)ctx)->idles++;
    return true;
}

static void *produce(void *arg) {
    producer_t *p = arg;
    for (unsigned i = 0; i < PRODUCED; i++) {
        char text[32];
        snprintf(text, sizeof text, "%u", i);
        pi_event_t ev = {.point = i % 7, .replayed = 1, .event = strdup(text)};
        if (pi_ring_push(p->ring, &ev, count_idle, p) != 0) {
            free(ev.event);
            p->failed++;
        }
    }
    return NULL;
}

static void ring_backpressure(void) {
    pi_ring_t ring;
    CHECK(pi_ring_init(&ring, 10) == 0, "init");
    CHECK(ring.bound == PI_RING_BOUND_MIN, "bound %zu", ring.bound);
    producer_t p = {.ring = &ring};
    pthread_t th;
    pthread_create(&th, NULL, produce, &p);
    unsigned next = 0;
    size_t most = 0;
    int rounds = 0;
    while (next < PRODUCED && rounds < 100000) {
        rounds++;
        /* a slow runtime: one drain every 2 ms */
        struct timespec ts = {0, 2000000L};
        nanosleep(&ts, NULL);
        pi_event_t *evs = NULL;
        size_t n = pi_ring_take(&ring, &evs);
        if (n > most)
            most = n;
        for (size_t i = 0; i < n; i++) {
            unsigned got = (unsigned)strtoul(evs[i].event, NULL, 10);
            if (got != next) {
                CHECK(got == next, "event %u arrived as #%u", got, next);
                next = got;
            }
            next++;
            free(evs[i].event);
        }
        free(evs);
    }
    pthread_join(th, NULL);
    CHECK(next == PRODUCED, "received %u of %u", next, PRODUCED);
    CHECK(p.failed == 0 && ring.dropped == 0, "dropped %d / %lu", p.failed, ring.dropped);
    CHECK(most <= pi_ring_high_water(&ring) + 1, "the ring held %zu, above its high water %zu",
          most, pi_ring_high_water(&ring));
    CHECK(ring.cap <= ring.bound, "cap %zu above bound %zu", ring.cap, ring.bound);
    pi_ring_destroy(&ring);

    /* close wakes a waiting producer */
    CHECK(pi_ring_init(&ring, 0) == 0, "init");
    producer_t q = {.ring = &ring};
    pthread_create(&th, NULL, produce, &q);
    while (pi_ring_len(&ring) < pi_ring_high_water(&ring)) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
    pi_ring_close(&ring);
    pthread_join(th, NULL);
    CHECK(q.failed == (int)(PRODUCED - pi_ring_high_water(&ring)), "after close %d refused",
          q.failed);
    pi_ring_destroy(&ring);
    printf("aveva-pi-push: %u events through a ring bounded at %d (largest drain %zu)\n",
           PRODUCED, PI_RING_BOUND_MIN, most);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/tdot-aveva-pi-push-XXXXXX");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    times();
    dedup();
    round_trip();
    corrupt_file();
    ring_backpressure();
    rmdir(g_dir);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
