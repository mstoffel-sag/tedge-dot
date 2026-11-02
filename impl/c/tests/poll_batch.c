/* Polling through a module's read_points (connector.h): the runtime hands a
 * device's due points over TDOT_READ_POINTS_MAX at a time, publishes each
 * point's sample as it would read_point's, and stops the pass at the first
 * point that takes the link down -- the same outcome as polling one point at a
 * time. Runs the runtime in stdout mode against a fake module, one pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

#define POINTS 1200
/* The point whose module sets a source time, and the time (contract §5). */
#define SOURCE_TIMED 5
#define SOURCE_TS_MS 1767225600913.0

/* The fake module's behaviour, set before each run: the point whose read
 * takes the link down and the one with nothing to read (-1: none). */
static int lose_at = -1;
static int no_data_at = -1;
/* Where the fake appends the size of each read_points call, one per line. */
static FILE *calls;

static int point_index(const tdot_point_t *pt) { return atoi(pt->id + 1); }

static int fake_configure(tdot_connector_t *self, tdot_config_t *cfg, char *err,
                          size_t errlen) {
    (void)self, (void)cfg, (void)err, (void)errlen;
    return 0;
}

static int fake_connect(tdot_connector_t *self, tdot_device_t *dev, char *err,
                        size_t errlen) {
    (void)self, (void)dev, (void)err, (void)errlen;
    return 0;
}

static int fake_read(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                     tdot_sample_t *out) {
    (void)self, (void)dev;
    int i = point_index(pt);
    if (i == no_data_at)
        return TDOT_READ_NO_DATA;
    if (i == lose_at) {
        tdot_sample_bad(out, "bad status: BadConnectionClosed");
        return -1;
    }
    out->value.kind = TDOT_VAL_NUM;
    out->value.num = i;
    if (i == SOURCE_TIMED)
        out->source_ts_ms = SOURCE_TS_MS;
    return 0;
}

/* read_point for each point, stopping after the first that takes the link
 * down: what a module's read_points must return. */
static size_t fake_read_points(tdot_connector_t *self, tdot_device_t *dev,
                               tdot_point_t *const *pts, size_t n, tdot_sample_t *out,
                               int *rc) {
    fprintf(calls, "%zu\n", n);
    for (size_t k = 0; k < n; k++) {
        rc[k] = fake_read(self, dev, pts[k], &out[k]);
        if (rc[k] == -1)
            return k + 1;
    }
    return n;
}

static int fake_write(tdot_connector_t *self, tdot_device_t *dev, tdot_point_t *pt,
                      const tdot_value_t *value, char *err, size_t errlen) {
    (void)self, (void)dev, (void)pt, (void)value;
    snprintf(err, errlen, "read-only");
    return -1;
}

static void fake_disconnect(tdot_connector_t *self, tdot_device_t *dev) {
    (void)self, (void)dev;
}

static void fake_destroy(tdot_connector_t *self) { free(self); }

/* What one run published and how the module was called. */
typedef struct {
    int samples;              /* sample lines on stdout */
    int good;                 /* of which good, with the point's index as value */
    int bad;
    int max_index;            /* the highest point index published */
    bool seen[POINTS];
    double ts_ms[POINTS];     /* each point's published ts_ms */
    char source_ts[40];       /* the published ts of SOURCE_TIMED */
    size_t sizes[16];         /* read_points call sizes, in order */
    size_t ncalls;
} run_t;

/* Run the runtime for one pass over a device of POINTS points, with or
 * without read_points. */
static void run(bool batched, run_t *r) {
    memset(r, 0, sizeof *r);
    r->max_index = -1;
    char dir[] = "/tmp/tdot-poll-batch-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        failures++;
        return;
    }
    char cfg_path[256], out_path[256], calls_path[256];
    snprintf(cfg_path, sizeof cfg_path, "%s/fake.toml", dir);
    snprintf(out_path, sizeof out_path, "%s/out.jsonl", dir);
    snprintf(calls_path, sizeof calls_path, "%s/calls.txt", dir);
    FILE *fp = fopen(cfg_path, "w");
    /* A poll interval longer than the run: exactly one pass. */
    fputs("[connector]\nprotocol = \"fake\"\npoll_interval = \"60s\"\n"
          "[[device]]\nname = \"d\"\nprotocol_address = {}\n",
          fp);
    for (int i = 0; i < POINTS; i++)
        fprintf(fp, "  [[device.point]]\n  id = \"p%d\"\n  datatype = \"uint16\"\n"
                    "  address = {}\n",
                i);
    fclose(fp);

    fflush(stdout); /* or the child writes what is buffered a second time */
    pid_t child = fork();
    if (child == 0) {
        if (!freopen(out_path, "w", stdout))
            _exit(3);
        calls = fopen(calls_path, "w");
        char err[256];
        tdot_config_t *cfg = tdot_config_load(cfg_path, err, sizeof err);
        if (!cfg || !calls)
            _exit(4);
        tdot_connector_t *conn = calloc(1, sizeof *conn);
        conn->protocol = "fake";
        conn->configure = fake_configure;
        conn->connect_device = fake_connect;
        conn->read_point = fake_read;
        if (batched)
            conn->read_points = fake_read_points;
        conn->write_point = fake_write;
        conn->disconnect_device = fake_disconnect;
        conn->destroy = fake_destroy;
        tdot_run_opts_t opts = {.output = TDOT_OUTPUT_STDOUT, .duration_s = 0.5};
        int rc = tdot_runtime_run(conn, cfg, &opts);
        fflush(stdout);
        fclose(calls);
        _exit(rc != 0 ? 5 : 0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the runtime run failed (exit %d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    fp = fopen(out_path, "r");
    char line[4096];
    while (fp && fgets(line, sizeof line, fp)) {
        cJSON *sample = cJSON_Parse(line);
        const cJSON *point = cJSON_GetObjectItem(sample, "point");
        const cJSON *quality = cJSON_GetObjectItem(sample, "quality");
        const cJSON *value = cJSON_GetObjectItem(sample, "value");
        if (cJSON_IsString(point)) {
            int i = atoi(point->valuestring + 1);
            r->samples++;
            if (i >= 0 && i < POINTS) {
                CHECK(!r->seen[i], "point %d published twice in one pass", i);
                r->seen[i] = true;
                const cJSON *ts_ms = cJSON_GetObjectItem(sample, "ts_ms");
                r->ts_ms[i] = cJSON_IsNumber(ts_ms) ? ts_ms->valuedouble : -1;
                const cJSON *ts = cJSON_GetObjectItem(sample, "ts");
                if (i == SOURCE_TIMED && cJSON_IsString(ts))
                    snprintf(r->source_ts, sizeof r->source_ts, "%s", ts->valuestring);
            }
            if (i > r->max_index)
                r->max_index = i;
            if (cJSON_IsString(quality) && strcmp(quality->valuestring, "good") == 0 &&
                cJSON_IsNumber(value) && value->valuedouble == i)
                r->good++;
            else
                r->bad++;
        }
        cJSON_Delete(sample);
    }
    if (fp)
        fclose(fp);
    fp = fopen(calls_path, "r");
    size_t size;
    while (fp && r->ncalls < sizeof r->sizes / sizeof r->sizes[0] &&
           fscanf(fp, "%zu", &size) == 1)
        r->sizes[r->ncalls++] = size;
    if (fp)
        fclose(fp);
}

static void check_batches(void) {
    run_t r;
    lose_at = no_data_at = -1;
    run(true, &r);
    CHECK(r.ncalls == 3 && r.sizes[0] == TDOT_READ_POINTS_MAX &&
              r.sizes[1] == TDOT_READ_POINTS_MAX && r.sizes[2] == POINTS - 2 * TDOT_READ_POINTS_MAX,
          "one pass of %d points must be read in calls of %d, %d, %d: %zu call(s) of %zu, %zu, %zu",
          POINTS, TDOT_READ_POINTS_MAX, TDOT_READ_POINTS_MAX, POINTS - 2 * TDOT_READ_POINTS_MAX,
          r.ncalls, r.sizes[0], r.sizes[1], r.sizes[2]);
    CHECK(r.samples == POINTS && r.good == POINTS,
          "every point is published good with its own value: %d sample(s), %d good", r.samples,
          r.good);
    /* One read time per call (contract §5), unless the module gave a source time. */
    for (int i = 0; i < POINTS; i++) {
        int first = i / TDOT_READ_POINTS_MAX * TDOT_READ_POINTS_MAX;
        if (first == SOURCE_TIMED)
            first++;
        if (i == SOURCE_TIMED)
            CHECK(r.ts_ms[i] == SOURCE_TS_MS, "p%d keeps its source time: %.3f", i, r.ts_ms[i]);
        else
            CHECK(r.ts_ms[i] == r.ts_ms[first], "p%d shares its call's read time: %.3f vs %.3f",
                  i, r.ts_ms[i], r.ts_ms[first]);
    }
    CHECK(strcmp(r.source_ts, "2026-01-01T00:00:00.913Z") == 0, "source ts: %s", r.source_ts);
}

/* Without read_points the pass over the device is the read call: one time. */
static void check_point_by_point_pass_time(void) {
    run_t r;
    lose_at = no_data_at = -1;
    run(false, &r);
    int first = SOURCE_TIMED == 0 ? 1 : 0;
    for (int i = 0; i < POINTS; i++)
        if (i != SOURCE_TIMED)
            CHECK(r.ts_ms[i] == r.ts_ms[first], "p%d shares the pass's read time: %.3f vs %.3f", i,
                  r.ts_ms[i], r.ts_ms[first]);
}

/* A lost link in the middle of a call: the points before it and the one that
 * lost it are published, nothing after, and no further call is made -- what
 * polling one point at a time does. */
static void check_lost_link(void) {
    run_t batched, single;
    lose_at = 700;
    no_data_at = -1;
    run(true, &batched);
    run(false, &single);
    CHECK(batched.ncalls == 2, "no call after the link is lost: %zu call(s)", batched.ncalls);
    CHECK(batched.samples == 701 && batched.good == 700 && batched.bad == 1 &&
              batched.max_index == 700,
          "points 0..700 published, the last bad: %d sample(s), %d good, %d bad, up to p%d",
          batched.samples, batched.good, batched.bad, batched.max_index);
    CHECK(single.samples == batched.samples && single.good == batched.good &&
              single.bad == batched.bad,
          "the same samples as one point at a time: %d/%d/%d vs %d/%d/%d", batched.samples,
          batched.good, batched.bad, single.samples, single.good, single.bad);
    lose_at = -1;
}

/* A point with nothing to read publishes nothing, and the others are not
 * affected. */
static void check_no_data(void) {
    run_t r;
    lose_at = -1;
    no_data_at = 3;
    run(true, &r);
    CHECK(r.samples == POINTS - 1 && r.good == POINTS - 1 && !r.seen[3],
          "p3 has nothing to read and the rest are published: %d sample(s), %d good, p3 %s",
          r.samples, r.good, r.seen[3] ? "published" : "not published");
    no_data_at = -1;
}

int main(void) {
    check_batches();
    check_point_by_point_pass_time();
    check_lost_link();
    check_no_data();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("poll-batch: all checks passed\n");
    return 0;
}
