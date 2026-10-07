#include "tedge_dot/watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tedge_dot/config.h"

double tdot_watch_interval(const char *value, char *warning, size_t warnlen) {
    if (warnlen)
        warning[0] = '\0';
    if (!value)
        return TDOT_WATCH_DEFAULT_S;
    double s = tdot_duration_parse(value);
    if (s == 0.0)
        return 0.0;
    if (s < 0) {
        snprintf(warning, warnlen,
                 "%s=%s is not a duration (e.g. \"2s\", \"500ms\", \"0\" to turn "
                 "watching off); watching every %.0fs",
                 TDOT_WATCH_INTERVAL_ENV, value, TDOT_WATCH_DEFAULT_S);
        return TDOT_WATCH_DEFAULT_S;
    }
    if (s < TDOT_WATCH_MIN_S) {
        snprintf(warning, warnlen, "%s=%s is below the minimum; watching every %.0fms",
                 TDOT_WATCH_INTERVAL_ENV, value, TDOT_WATCH_MIN_S * 1000);
        return TDOT_WATCH_MIN_S;
    }
    return s;
}

/* One version of a file. A rename-into-place gets a new inode even when size
 * and modification time happen to match. */
typedef struct {
    char *path;
    bool exists;
    long long size;
    long long mtime_s, mtime_ns;
    unsigned long long inode;
} stamp_t;

typedef struct {
    stamp_t *items;
    size_t n;
} snapshot_t;

struct tdot_watch {
    tdot_watch_list_fn list;
    void *ctx;
    char **libs;
    size_t nlibs;
    snapshot_t baseline, pending;
    bool has_pending;
};

static void snapshot_free(snapshot_t *s) {
    for (size_t i = 0; i < s->n; i++)
        free(s->items[i].path);
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

static void stamp_of(stamp_t *st) {
    struct stat sb;
    st->exists = stat(st->path, &sb) == 0 && S_ISREG(sb.st_mode);
    if (!st->exists)
        return;
    st->size = (long long)sb.st_size;
    st->inode = (unsigned long long)sb.st_ino;
#if defined(__APPLE__)
    st->mtime_s = (long long)sb.st_mtimespec.tv_sec;
    st->mtime_ns = (long long)sb.st_mtimespec.tv_nsec;
#else
    st->mtime_s = (long long)sb.st_mtim.tv_sec;
    st->mtime_ns = (long long)sb.st_mtim.tv_nsec;
#endif
}

static bool stamp_eq(const stamp_t *a, const stamp_t *b) {
    if (a->exists != b->exists)
        return false;
    return !a->exists || (a->size == b->size && a->mtime_s == b->mtime_s &&
                          a->mtime_ns == b->mtime_ns && a->inode == b->inode);
}

static int stamp_cmp(const void *a, const void *b) {
    return strcmp(((const stamp_t *)a)->path, ((const stamp_t *)b)->path);
}

static void snapshot_add(snapshot_t *s, const char *path) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->items[i].path, path) == 0)
            return;
    s->items = realloc(s->items, (s->n + 1) * sizeof *s->items);
    stamp_t *st = &s->items[s->n++];
    memset(st, 0, sizeof *st);
    st->path = strdup(path);
    stamp_of(st);
}

/* The config files the argument lists now, plus the referenced libraries. */
static snapshot_t take_snapshot(const tdot_watch_t *w) {
    snapshot_t s = {0};
    char **paths = NULL;
    size_t npaths = 0;
    if (w->list(w->ctx, &paths, &npaths) == 0) {
        for (size_t i = 0; i < npaths; i++) {
            snapshot_add(&s, paths[i]);
            free(paths[i]);
        }
        free(paths);
    }
    for (size_t i = 0; i < w->nlibs; i++)
        snapshot_add(&s, w->libs[i]);
    if (s.n > 1)
        qsort(s.items, s.n, sizeof *s.items, stamp_cmp);
    return s;
}

static bool snapshot_eq(const snapshot_t *a, const snapshot_t *b) {
    if (a->n != b->n)
        return false;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->items[i].path, b->items[i].path) != 0 ||
            !stamp_eq(&a->items[i], &b->items[i]))
            return false;
    return true;
}

static const stamp_t *snapshot_find(const snapshot_t *s, const char *path) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->items[i].path, path) == 0)
            return &s->items[i];
    return NULL;
}

static void append(char *out, size_t outlen, size_t *used, const char *path) {
    if (*used >= outlen)
        return;
    *used += (size_t)snprintf(out + *used, outlen - *used, "%s%s", *used ? ", " : "", path);
}

/* The paths that differ between two snapshots: added, removed or changed. */
static void changed_paths(const snapshot_t *before, const snapshot_t *after, char *out,
                          size_t outlen) {
    size_t used = 0;
    if (outlen)
        out[0] = '\0';
    for (size_t i = 0; i < after->n; i++) {
        const stamp_t *old = snapshot_find(before, after->items[i].path);
        if (!old || !stamp_eq(old, &after->items[i]))
            append(out, outlen, &used, after->items[i].path);
    }
    for (size_t i = 0; i < before->n; i++)
        if (!snapshot_find(after, before->items[i].path))
            append(out, outlen, &used, before->items[i].path);
}

tdot_watch_t *tdot_watch_new(tdot_watch_list_fn list, void *ctx) {
    tdot_watch_t *w = calloc(1, sizeof *w);
    w->list = list;
    w->ctx = ctx;
    tdot_watch_rebuild(w);
    w->baseline = take_snapshot(w);
    return w;
}

void tdot_watch_free(tdot_watch_t *w) {
    if (!w)
        return;
    for (size_t i = 0; i < w->nlibs; i++)
        free(w->libs[i]);
    free(w->libs);
    snapshot_free(&w->baseline);
    snapshot_free(&w->pending);
    free(w);
}

void tdot_watch_rebuild(tdot_watch_t *w) {
    for (size_t i = 0; i < w->nlibs; i++)
        free(w->libs[i]);
    free(w->libs);
    w->libs = NULL;
    w->nlibs = 0;
    char **paths = NULL;
    size_t npaths = 0;
    if (w->list(w->ctx, &paths, &npaths) != 0)
        return;
    for (size_t i = 0; i < npaths; i++) {
        char **libs = NULL;
        size_t nlibs = 0;
        tdot_config_referenced_libraries(paths[i], &libs, &nlibs);
        for (size_t j = 0; j < nlibs; j++) {
            bool known = false;
            for (size_t k = 0; k < w->nlibs && !known; k++)
                known = strcmp(w->libs[k], libs[j]) == 0;
            if (known) {
                free(libs[j]);
                continue;
            }
            w->libs = realloc(w->libs, (w->nlibs + 1) * sizeof *w->libs);
            w->libs[w->nlibs++] = libs[j];
        }
        free(libs);
        free(paths[i]);
    }
    free(paths);
}

bool tdot_watch_poll(tdot_watch_t *w, char *changed, size_t changedlen) {
    snapshot_t now = take_snapshot(w);
    if (snapshot_eq(&now, &w->baseline)) {
        snapshot_free(&now);
        if (w->has_pending) {
            snapshot_free(&w->pending);
            w->has_pending = false;
        }
        return false;
    }
    if (!w->has_pending || !snapshot_eq(&now, &w->pending)) {
        /* Changed since the last poll: wait for one more interval without one. */
        snapshot_free(&w->pending);
        w->pending = now;
        w->has_pending = true;
        return false;
    }
    changed_paths(&w->baseline, &now, changed, changedlen);
    /* The baseline moves to what this reload is about to read, before it reads
     * it: a write landing during the reload is a difference at the next poll. */
    snapshot_free(&w->baseline);
    w->baseline = now;
    snapshot_free(&w->pending);
    w->has_pending = false;
    return true;
}
