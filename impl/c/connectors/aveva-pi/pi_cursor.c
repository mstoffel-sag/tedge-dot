/* AVEVA PI connector -- delivery cursors. See pi_cursor.h. */
#include "pi_cursor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "pi_log.h"
#include "pi_value.h"

#define PI_CURSOR_VERSION 1

/* ---- time ----------------------------------------------------------------------------------- */

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool digits(const char *p, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return true;
}

bool pi_time_parse(const char *text, pi_time_t *out) {
    if (!text)
        return false;
    int y, mo, d, h, mi, s;
    const char *p = text;
    if (strlen(p) < 20 || !digits(p, 4, &y) || p[4] != '-' || !digits(p + 5, 2, &mo) ||
        p[7] != '-' || !digits(p + 8, 2, &d) || (p[10] != 'T' && p[10] != 't') ||
        !digits(p + 11, 2, &h) || p[13] != ':' || !digits(p + 14, 2, &mi) || p[16] != ':' ||
        !digits(p + 17, 2, &s))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        return false;
    p += 19;
    int32_t nsec = 0;
    if (*p == '.') {
        p++;
        int n = 0;
        while (*p >= '0' && *p <= '9') {
            if (n < 9) {
                nsec = nsec * 10 + (*p - '0');
                n++;
            }
            p++;
        }
        if (n == 0)
            return false;
        for (; n < 9; n++)
            nsec *= 10;
    }
    int64_t offset = 0;
    if (*p == 'Z' || *p == 'z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        int oh, om;
        if (!digits(p + 1, 2, &oh) || p[3] != ':' || !digits(p + 4, 2, &om))
            return false;
        offset = (int64_t)(oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
        p += 6;
    } else {
        return false;
    }
    if (*p)
        return false;
    out->sec = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s -
               offset;
    out->nsec = nsec;
    return true;
}

int pi_time_cmp(pi_time_t a, pi_time_t b) {
    if (a.sec != b.sec)
        return a.sec < b.sec ? -1 : 1;
    if (a.nsec != b.nsec)
        return a.nsec < b.nsec ? -1 : 1;
    return 0;
}

pi_time_t pi_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (pi_time_t){.sec = ts.tv_sec, .nsec = (int32_t)ts.tv_nsec};
}

pi_time_t pi_time_minus(pi_time_t t, double seconds) {
    int64_t whole = (int64_t)seconds;
    int64_t frac = (int64_t)((seconds - (double)whole) * 1e9);
    t.sec -= whole;
    int64_t ns = (int64_t)t.nsec - frac;
    while (ns < 0) {
        ns += 1000000000;
        t.sec--;
    }
    t.nsec = (int32_t)ns;
    return t;
}

void pi_time_format(pi_time_t t, char *dst, size_t cap) {
    time_t secs = (time_t)t.sec;
    struct tm tm;
    gmtime_r(&secs, &tm);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    /* Milliseconds at most: these times only start a `recorded` read (a floor or "an interval
     * ago"), and PI Web API parses at most 7 fractional digits. */
    int msec = (int)(t.nsec / 1000000);
    if (msec == 0) {
        snprintf(dst, cap, "%sZ", base);
        return;
    }
    char frac[16];
    snprintf(frac, sizeof frac, "%03d", msec);
    size_t n = strlen(frac);
    while (n > 1 && frac[n - 1] == '0')
        frac[--n] = '\0';
    snprintf(dst, cap, "%s.%sZ", base, frac);
}

double pi_time_ms(pi_time_t t) { return (double)t.sec * 1000.0 + (double)t.nsec / 1e6; }

/* ---- cursors -------------------------------------------------------------------------------- */

bool pi_cursor_admits(const pi_cursor_t *c, pi_time_t at, const char *value) {
    if (!c->set)
        return true;
    int cmp = pi_time_cmp(at, c->at);
    if (cmp != 0)
        return cmp > 0;
    for (size_t i = 0; i < c->nvalues; i++)
        if (strcmp(c->values[i], value) == 0)
            return false;
    return true;
}

void pi_cursor_clear(pi_cursor_t *c) {
    for (size_t i = 0; i < c->nvalues; i++)
        free(c->values[i]);
    free(c->values);
    memset(c, 0, sizeof *c);
}

int pi_cursor_advance(pi_cursor_t *c, const char *ts, pi_time_t at, const char *value) {
    int cmp = c->set ? pi_time_cmp(at, c->at) : 1;
    if (cmp < 0)
        return 0; /* an older event never moves the cursor back */
    if (cmp == 0) {
        for (size_t i = 0; i < c->nvalues; i++)
            if (strcmp(c->values[i], value) == 0)
                return 0;
    } else {
        pi_cursor_clear(c);
        c->set = true;
        snprintf(c->ts, sizeof c->ts, "%s", ts);
        c->at = at;
    }
    char **values = realloc(c->values, (c->nvalues + 1) * sizeof *values);
    if (!values)
        return -1;
    c->values = values;
    c->values[c->nvalues] = strdup(value);
    if (!c->values[c->nvalues])
        return -1;
    c->nvalues++;
    return 0;
}

int pi_cursor_copy(pi_cursor_t *dst, const pi_cursor_t *src) {
    pi_cursor_clear(dst);
    if (!src->set)
        return 0;
    dst->set = true;
    memcpy(dst->ts, src->ts, sizeof dst->ts);
    dst->at = src->at;
    if (src->nvalues) {
        dst->values = calloc(src->nvalues, sizeof *dst->values);
        if (!dst->values)
            return -1;
        for (size_t i = 0; i < src->nvalues; i++) {
            dst->values[i] = strdup(src->values[i]);
            if (!dst->values[i])
                return -1;
            dst->nvalues++;
        }
    }
    return 0;
}

/* ---- the file ------------------------------------------------------------------------------- */

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *read_file(const char *path, size_t *len, int *err) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        *err = errno;
        return NULL;
    }
    char *buf = NULL;
    size_t n = 0, cap = 0;
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (n + got + 1 > cap) {
            cap = cap ? cap * 2 : 16384;
            while (cap < n + got + 1)
                cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                fclose(fp);
                *err = ENOMEM;
                return NULL;
            }
            buf = p;
        }
        memcpy(buf + n, chunk, got);
        n += got;
    }
    fclose(fp);
    if (!buf)
        buf = calloc(1, 1);
    *len = n;
    *err = 0;
    return buf;
}

/* Load the cursors of the file's points into the store's entries. Returns how many it set, or
 * -1 with `why` when the file is unreadable. */
static int load_points(pi_store_t *s, const cJSON *doc, char *why, size_t wlen) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version");
    const cJSON *points = cJSON_GetObjectItemCaseSensitive(doc, "points");
    if (!cJSON_IsNumber(version) || !cJSON_IsObject(points)) {
        snprintf(why, wlen, "cursor file is unreadable (not a cursor file)");
        return -1;
    }
    if (version->valuedouble != PI_CURSOR_VERSION) {
        snprintf(why, wlen, "cursor file version %g is not %d", version->valuedouble,
                 PI_CURSOR_VERSION);
        return -1;
    }
    int loaded = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, points) {
        const cJSON *web_id = cJSON_GetObjectItemCaseSensitive(e, "web_id");
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(e, "ts");
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(e, "values");
        if (!cJSON_IsString(web_id) || !cJSON_IsString(ts) || !cJSON_IsArray(values)) {
            snprintf(why, wlen, "cursor file is unreadable (point '%s' is malformed)", e->string);
            return -1;
        }
        pi_time_t at;
        if (!pi_time_parse(ts->valuestring, &at))
            continue;
        for (size_t k = 0; k < s->n; k++) {
            if (strcmp(s->ids[k], e->string) != 0 || strcmp(s->web_ids[k], web_id->valuestring))
                continue;
            pi_cursor_t *c = &s->cur[k];
            pi_cursor_clear(c);
            c->set = true;
            snprintf(c->ts, sizeof c->ts, "%s", ts->valuestring);
            c->at = at;
            const cJSON *v;
            cJSON_ArrayForEach(v, values) {
                char *text = pi_canonical(v);
                if (!text)
                    return -1;
                char **grown = realloc(c->values, (c->nvalues + 1) * sizeof *grown);
                if (!grown) {
                    free(text);
                    return -1;
                }
                c->values = grown;
                c->values[c->nvalues++] = text;
            }
            loaded++;
        }
    }
    return loaded;
}

int pi_store_load(pi_store_t *s, const char *path, const char *const *ids,
                  const char *const *web_ids, size_t n, double every) {
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->path = strdup(path);
    s->n = n;
    s->every = every;
    s->last_flush = mono();
    s->ids = calloc(n ? n : 1, sizeof *s->ids);
    s->web_ids = calloc(n ? n : 1, sizeof *s->web_ids);
    s->cur = calloc(n ? n : 1, sizeof *s->cur);
    if (!s->path || !s->ids || !s->web_ids || !s->cur)
        return -1;
    for (size_t k = 0; k < n; k++) {
        s->ids[k] = strdup(ids[k]);
        s->web_ids[k] = strdup(web_ids[k]);
        if (!s->ids[k] || !s->web_ids[k])
            return -1;
    }
    size_t len = 0;
    int err = 0;
    char *text = read_file(path, &len, &err);
    int loaded = 0;
    if (!text) {
        if (err != ENOENT)
            pi_log("warn", "%s: cursor file is unreadable (%s); starting live", path,
                   strerror(err));
    } else {
        cJSON *doc = pi_json_parse(text, len);
        char why[256] = "cursor file is unreadable (not JSON)";
        int got = doc ? load_points(s, doc, why, sizeof why) : -1;
        cJSON_Delete(doc);
        if (got < 0) {
            for (size_t k = 0; k < n; k++)
                pi_cursor_clear(&s->cur[k]);
            pi_log("warn", "%s: %s; starting live", path, why);
        } else {
            loaded = got;
        }
        free(text);
    }
    /* A point no longer configured (or re-addressed) is dropped on the next write. */
    s->dirty = (size_t)loaded != n;
    return 0;
}

void pi_store_free(pi_store_t *s) {
    for (size_t k = 0; k < s->n; k++) {
        if (s->ids)
            free(s->ids[k]);
        if (s->web_ids)
            free(s->web_ids[k]);
        if (s->cur)
            pi_cursor_clear(&s->cur[k]);
    }
    free(s->ids);
    free(s->web_ids);
    free(s->cur);
    free(s->path);
    pthread_mutex_destroy(&s->mu);
    memset(s, 0, sizeof *s);
}

void pi_store_maybe_flush(pi_store_t *s) {
    if (s->dirty && mono() - s->last_flush >= s->every)
        pi_store_flush(s);
}

/* mkdir -p of the file's directory. */
static int make_parent(const char *path) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

void pi_store_flush(pi_store_t *s) {
    if (!s->dirty || !s->path)
        return;
    s->last_flush = mono();
    cJSON *doc = cJSON_CreateObject();
    cJSON *points = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "version", PI_CURSOR_VERSION);
    cJSON_AddItemToObject(doc, "points", points);
    for (size_t k = 0; k < s->n; k++) {
        const pi_cursor_t *c = &s->cur[k];
        if (!c->set)
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "web_id", s->web_ids[k]);
        cJSON_AddStringToObject(e, "ts", c->ts);
        cJSON *values = cJSON_AddArrayToObject(e, "values");
        for (size_t i = 0; i < c->nvalues; i++) {
            cJSON *v = pi_json_parse(c->values[i], strlen(c->values[i]));
            cJSON_AddItemToArray(values, v ? v : cJSON_CreateNull());
        }
        cJSON_AddItemToObject(points, s->ids[k], e);
    }
    char *text = pi_canonical(doc);
    cJSON_Delete(doc);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
    const char *failed = NULL;
    int err = 0;
    if (!text) {
        failed = "out of memory";
    } else if (make_parent(s->path) != 0) {
        err = errno;
        failed = "cannot create its directory";
    } else {
        FILE *fp = fopen(tmp, "wb");
        if (!fp) {
            err = errno;
            failed = "cannot open";
        } else {
            size_t n = strlen(text);
            bool ok = fwrite(text, 1, n, fp) == n;
            if (fclose(fp) != 0 || !ok) {
                err = errno;
                failed = "cannot write";
            } else if (rename(tmp, s->path) != 0) {
                err = errno;
                failed = "cannot rename";
            }
        }
    }
    free(text);
    if (failed)
        pi_log("warn", "%s: cannot write the cursor file: %s%s%s", s->path, failed,
               err ? ": " : "", err ? strerror(err) : "");
    else
        s->dirty = false;
}
