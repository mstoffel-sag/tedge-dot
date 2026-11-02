/* Concurrent reads (contract §5.6): the endpoint and total budgets. See
 * tedge_dot/concurrency.h; the same rules as concurrency.rs. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "tedge_dot/concurrency.h"

struct tdot_budget {
    char *name;
    size_t configured;
    bool backs_off; /* an endpoint does; the total does not */
    size_t limit;   /* the budget in effect, after any backoff */
    size_t in_use;
    unsigned successes; /* since the budget last changed */
    pthread_mutex_t mu;
    pthread_cond_t freed;
};

struct tdot_budgets {
    tdot_budget_t total;
    tdot_budget_t **endpoints; /* distinct, in the order they first appear */
    size_t nendpoints;
    size_t *device_endpoint; /* by device index */
    char **device_names;     /* by device index, for describe */
    size_t ndevices;
};

static void budget_init(tdot_budget_t *b, const char *name, size_t limit, bool backs_off) {
    b->name = strdup(name);
    b->configured = limit;
    b->limit = limit;
    b->backs_off = backs_off;
    b->in_use = 0;
    b->successes = 0;
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->freed, NULL);
}

static void budget_fini(tdot_budget_t *b) {
    free(b->name);
    pthread_mutex_destroy(&b->mu);
    pthread_cond_destroy(&b->freed);
}

static bool budget_try_take(tdot_budget_t *b) {
    pthread_mutex_lock(&b->mu);
    bool ok = b->in_use < b->limit;
    if (ok)
        b->in_use++;
    pthread_mutex_unlock(&b->mu);
    return ok;
}

static void budget_take(tdot_budget_t *b) {
    pthread_mutex_lock(&b->mu);
    while (b->in_use >= b->limit)
        pthread_cond_wait(&b->freed, &b->mu);
    b->in_use++;
    pthread_mutex_unlock(&b->mu);
}

static void budget_give_back(tdot_budget_t *b, size_t n) {
    if (n == 0)
        return;
    pthread_mutex_lock(&b->mu);
    b->in_use = b->in_use > n ? b->in_use - n : 0;
    pthread_cond_broadcast(&b->freed);
    pthread_mutex_unlock(&b->mu);
}

static size_t budget_effective(tdot_budget_t *b) {
    pthread_mutex_lock(&b->mu);
    size_t limit = b->limit;
    pthread_mutex_unlock(&b->mu);
    return limit;
}

static void budget_back_off(tdot_budget_t *b, const char *why) {
    if (!b->backs_off)
        return;
    pthread_mutex_lock(&b->mu);
    size_t halved = b->limit / 2 ? b->limit / 2 : 1;
    b->successes = 0;
    bool changed = halved != b->limit;
    b->limit = halved;
    pthread_mutex_unlock(&b->mu);
    if (changed)
        fprintf(stderr, "info  endpoint %s: %s: requests in flight lowered to %zu\n", b->name, why,
                halved);
}

static void budget_success(tdot_budget_t *b) {
    pthread_mutex_lock(&b->mu);
    size_t raised = 0;
    if (b->limit < b->configured && ++b->successes >= TDOT_REGROW_AFTER) {
        b->successes = 0;
        raised = ++b->limit;
        pthread_cond_broadcast(&b->freed);
    }
    pthread_mutex_unlock(&b->mu);
    if (raised)
        fprintf(stderr, "info  endpoint %s: requests in flight raised to %zu\n", b->name, raised);
}

static size_t positive_or(long v, size_t fallback) { return v > 0 ? (size_t)v : fallback; }

tdot_budgets_t *tdot_budgets_new(const tdot_config_t *cfg, tdot_endpoint_fn endpoint, void *ctx) {
    tdot_budgets_t *b = calloc(1, sizeof *b);
    size_t n = cfg->ndevices;
    if (!b)
        return NULL;
    b->ndevices = n;
    b->endpoints = calloc(n ? n : 1, sizeof *b->endpoints);
    b->device_endpoint = calloc(n ? n : 1, sizeof *b->device_endpoint);
    b->device_names = calloc(n ? n : 1, sizeof *b->device_names);
    /* The wanted budget and cap of each distinct endpoint while the table is
     * built; the budgets are made from them at the end. */
    size_t *wanted = calloc(n ? n : 1, sizeof *wanted);
    long *cap = calloc(n ? n : 1, sizeof *cap);
    char **names = calloc(n ? n : 1, sizeof *names);
    if (!b->endpoints || !b->device_endpoint || !b->device_names || !wanted || !cap || !names) {
        free(wanted);
        free(cap);
        free(names);
        tdot_budgets_free(b);
        return NULL;
    }
    size_t dflt = positive_or(cfg->max_inflight, TDOT_DEFAULT_MAX_INFLIGHT);
    budget_init(&b->total, "total", positive_or(cfg->max_inflight_total, TDOT_DEFAULT_MAX_INFLIGHT_TOTAL),
                false);
    for (size_t i = 0; i < n; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        b->device_names[i] = strdup(dev->name);
        char buf[512];
        int c = endpoint ? endpoint(ctx, dev, buf, sizeof buf) : -1;
        const char *name = c < 0 ? dev->name : buf;
        size_t want = positive_or(dev->max_inflight, dflt);
        size_t e = 0;
        while (e < b->nendpoints && strcmp(names[e], name) != 0)
            e++;
        if (e == b->nendpoints) {
            names[e] = strdup(name);
            wanted[e] = want;
            cap[e] = c > 0 ? c : 0;
            b->nendpoints++;
        } else {
            if (want < wanted[e])
                wanted[e] = want;
            if (c > 0 && (cap[e] == 0 || c < cap[e]))
                cap[e] = c;
        }
        b->device_endpoint[i] = e;
    }
    for (size_t e = 0; e < b->nendpoints; e++) {
        size_t limit = wanted[e];
        if (cap[e] > 0 && (size_t)cap[e] < limit)
            limit = (size_t)cap[e];
        b->endpoints[e] = calloc(1, sizeof *b->endpoints[e]);
        if (b->endpoints[e])
            budget_init(b->endpoints[e], names[e], limit, true);
        free(names[e]);
    }
    free(wanted);
    free(cap);
    free(names);
    for (size_t e = 0; e < b->nendpoints; e++)
        if (!b->endpoints[e]) {
            tdot_budgets_free(b);
            return NULL;
        }
    return b;
}

void tdot_budgets_free(tdot_budgets_t *b) {
    if (!b)
        return;
    for (size_t e = 0; e < b->nendpoints; e++)
        if (b->endpoints[e]) {
            budget_fini(b->endpoints[e]);
            free(b->endpoints[e]);
        }
    if (b->total.name)
        budget_fini(&b->total);
    for (size_t i = 0; b->device_names && i < b->ndevices; i++)
        free(b->device_names[i]);
    free(b->device_names);
    free(b->endpoints);
    free(b->device_endpoint);
    free(b);
}

void tdot_budgets_acquire(tdot_budgets_t *b, size_t index, size_t wanted, tdot_inflight_t *out) {
    tdot_budget_t *ep = b->endpoints[b->device_endpoint[index]];
    budget_take(ep);
    budget_take(&b->total);
    size_t held = 1;
    while (held < (wanted ? wanted : 1)) {
        if (!budget_try_take(ep))
            break;
        if (!budget_try_take(&b->total)) {
            budget_give_back(ep, 1);
            break;
        }
        held++;
    }
    out->endpoint = ep;
    out->total = &b->total;
    out->held = held;
}

void tdot_inflight_release(tdot_inflight_t *f) {
    if (!f->endpoint)
        return;
    budget_give_back(f->endpoint, f->held);
    budget_give_back(f->total, f->held);
    f->endpoint = NULL;
    f->total = NULL;
    f->held = 0;
}

size_t tdot_inflight_allowance(const tdot_inflight_t *f) { return f && f->held ? f->held : 1; }

size_t tdot_inflight_current(const tdot_inflight_t *f) {
    if (!f || !f->endpoint)
        return 1;
    size_t now = budget_effective(f->endpoint);
    size_t cur = f->held < now ? f->held : now;
    return cur ? cur : 1;
}

void tdot_inflight_ok(const tdot_inflight_t *f) {
    if (f && f->endpoint)
        budget_success(f->endpoint);
}

void tdot_inflight_busy(const tdot_inflight_t *f) {
    if (f && f->endpoint)
        budget_back_off(f->endpoint, "endpoint busy");
}

void tdot_inflight_timed_out(const tdot_inflight_t *f, bool others_in_flight) {
    if (f && f->endpoint && others_in_flight)
        budget_back_off(f->endpoint, "request timed out behind others");
}

const char *tdot_budgets_endpoint(const tdot_budgets_t *b, size_t index) {
    return b->endpoints[b->device_endpoint[index]]->name;
}

size_t tdot_budgets_configured(const tdot_budgets_t *b, size_t index) {
    return b->endpoints[b->device_endpoint[index]]->configured;
}

size_t tdot_budgets_effective(const tdot_budgets_t *b, size_t index) {
    return budget_effective(b->endpoints[b->device_endpoint[index]]);
}

cJSON *tdot_budgets_describe(const tdot_budgets_t *b) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t e = 0; e < b->nendpoints; e++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "endpoint", b->endpoints[e]->name);
        cJSON *devices = cJSON_AddArrayToObject(entry, "devices");
        for (size_t i = 0; i < b->ndevices; i++)
            if (b->device_endpoint[i] == e)
                cJSON_AddItemToArray(devices, cJSON_CreateString(b->device_names[i]));
        cJSON_AddNumberToObject(entry, "max_inflight", (double)b->endpoints[e]->configured);
        cJSON_AddItemToArray(arr, entry);
    }
    return arr;
}
