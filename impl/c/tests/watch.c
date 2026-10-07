/* Config file watching (watch.h), the same cases as the Rust watcher's tests
 * (impl/rust/src/watch.rs): what counts as a change, the settle rule, a write
 * during a reload, and which point libraries are watched. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tedge_dot/watch.h"

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

static void mkdtemp_into(char *out, size_t len) {
    char template[] = "/tmp/tdot-watch-XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(out, len, "%s", dir);
}

static void rm_rf(const char *dir) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        printf("warn: could not clean %s\n", dir);
}

/* Write through a temporary file and a rename, as tedge-write and the
 * service's own persist do: a new inode makes the change visible even when
 * size and modification time match. */
static void put(const char *dir, const char *rel, const char *text) {
    char path[600], tmp[620], cmd[700];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    char *slash = strrchr(path, '/');
    *slash = '\0';
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", path);
    if (system(cmd) != 0)
        exit(2);
    *slash = '/';
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    fputs(text, fp);
    fclose(fp);
    rename(tmp, path);
}

/* The *.toml files of one directory, as the run service lists them. */
static int list_dir(void *ctx, char ***paths, size_t *n) {
    const char *dir = ctx;
    DIR *d = opendir(dir);
    *paths = NULL;
    *n = 0;
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".toml") != 0)
            continue;
        char path[600];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        *paths = realloc(*paths, (*n + 1) * sizeof **paths);
        (*paths)[(*n)++] = strdup(path);
    }
    closedir(d);
    return 0;
}

/* Poll until settled: two polls are needed after a change. */
static bool settle(tdot_watch_t *w, char *changed, size_t len) {
    return tdot_watch_poll(w, changed, len) || tdot_watch_poll(w, changed, len);
}

static void check_interval(void) {
    char warning[256];
    CHECK(tdot_watch_interval(NULL, warning, sizeof warning) == TDOT_WATCH_DEFAULT_S &&
              !warning[0],
          "unset -> default");
    CHECK(tdot_watch_interval("500ms", warning, sizeof warning) == 0.5, "500ms");
    CHECK(tdot_watch_interval("0", warning, sizeof warning) == 0.0 && !warning[0], "0 -> off");
    CHECK(tdot_watch_interval("100ms", warning, sizeof warning) == TDOT_WATCH_MIN_S &&
              strstr(warning, "below the minimum"),
          "below the minimum -> minimum: %s", warning);
    CHECK(tdot_watch_interval("often", warning, sizeof warning) == TDOT_WATCH_DEFAULT_S &&
              strstr(warning, "not a duration"),
          "invalid -> default: %s", warning);
}

static void check_config_changes(void) {
    char dir[256], changed[1024], b[300], a[300];
    mkdtemp_into(dir, sizeof dir);
    put(dir, "a.toml", "x = 1");
    snprintf(a, sizeof a, "%s/a.toml", dir);
    snprintf(b, sizeof b, "%s/b.toml", dir);
    tdot_watch_t *w = tdot_watch_new(list_dir, dir);

    CHECK(!settle(w, changed, sizeof changed), "nothing changed, nothing reloads");

    put(dir, "notes.txt", "hello");
    CHECK(!settle(w, changed, sizeof changed), "a non-toml file is ignored");

    put(dir, "b.toml", "y = 1");
    CHECK(!tdot_watch_poll(w, changed, sizeof changed), "added: not yet settled");
    CHECK(tdot_watch_poll(w, changed, sizeof changed) && strcmp(changed, b) == 0,
          "added: reload once settled, got '%s'", changed);
    CHECK(!tdot_watch_poll(w, changed, sizeof changed), "acted on once");

    put(dir, "a.toml", "x = 2");
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, a) == 0, "changed: '%s'", changed);

    unlink(b);
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, b) == 0, "removed: '%s'", changed);

    /* Writes close together: one reload, after the last. */
    put(dir, "a.toml", "x = 3");
    CHECK(!tdot_watch_poll(w, changed, sizeof changed), "burst: first poll defers");
    put(dir, "a.toml", "x = 4");
    CHECK(!tdot_watch_poll(w, changed, sizeof changed), "burst: still changing, deferred");
    CHECK(tdot_watch_poll(w, changed, sizeof changed), "burst: one reload once settled");
    CHECK(!tdot_watch_poll(w, changed, sizeof changed), "burst: only one");

    /* A write while the reload runs gets a reload of its own. */
    put(dir, "a.toml", "x = 5");
    CHECK(settle(w, changed, sizeof changed), "during: first reload");
    put(dir, "a.toml", "x = 6"); /* the reload is running now */
    tdot_watch_rebuild(w);
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, a) == 0,
          "during: the write gets its own reload, got '%s'", changed);

    tdot_watch_free(w);
    rm_rf(dir);
}

static void plant(const char *dir, const char *libs, const char *refs) {
    char text[1024];
    snprintf(text, sizeof text,
             "[connector]\nprotocol = \"modbus\"\npoint_library_path = [\"%s\"]\n\n"
             "[[device]]\nname = \"plc1\"\npoints_from = [%s]\n",
             libs, refs);
    put(dir, "plant.toml", text);
}

static void check_libraries(void) {
    char dir[256], libs[256], changed[1024], used[400], extra[400], later[400];
    mkdtemp_into(dir, sizeof dir);
    mkdtemp_into(libs, sizeof libs);
    put(libs, "modbus/used.toml", "[[point]]\nid = \"a\"\n");
    put(libs, "modbus/unused.toml", "[[point]]\nid = \"b\"\n");
    put(libs, "modbus/extra.toml", "[[point]]\nid = \"e\"\n");
    snprintf(used, sizeof used, "%s/modbus/used.toml", libs);
    snprintf(extra, sizeof extra, "%s/modbus/extra.toml", libs);
    snprintf(later, sizeof later, "%s/modbus/later.toml", libs);
    plant(dir, libs, "\"used\", \"later\"");
    tdot_watch_t *w = tdot_watch_new(list_dir, dir);

    put(libs, "modbus/unused.toml", "[[point]]\nid = \"c\"\n");
    CHECK(!settle(w, changed, sizeof changed), "a library no config references is ignored");

    put(libs, "modbus/used.toml", "[[point]]\nid = \"d\"\n");
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, used) == 0,
          "a referenced library: '%s'", changed);

    put(libs, "modbus/later.toml", "[[point]]\nid = \"l\"\n");
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, later) == 0,
          "a missing library is watched where it would be found: '%s'", changed);

    /* Newly referenced: counted as added once after the reload, then watched. */
    plant(dir, libs, "\"used\", \"later\", \"extra\"");
    CHECK(settle(w, changed, sizeof changed), "the config edit reloads");
    tdot_watch_rebuild(w);
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, extra) == 0,
          "newly referenced library counts as added, once: '%s'", changed);
    CHECK(!settle(w, changed, sizeof changed), "then quiet");
    put(libs, "modbus/extra.toml", "[[point]]\nid = \"f\"\n");
    CHECK(settle(w, changed, sizeof changed) && strcmp(changed, extra) == 0,
          "and watched from then on: '%s'", changed);

    tdot_watch_free(w);
    rm_rf(dir);
    rm_rf(libs);
}

int main(void) {
    check_interval();
    check_config_changes();
    check_libraries();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("watch: all checks passed\n");
    return 0;
}
