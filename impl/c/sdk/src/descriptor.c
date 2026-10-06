#include "tedge_dot/descriptor.h"
#include "tedge_dot/map.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fold `src` into `dst` in place: every RUN of characters outside [A-Za-z0-9]
 * becomes a single '_', so a device type or group can be written the way it
 * reads ("acme-meter-v2") and still be a valid fragment key. A run rather than
 * a character because this folds bytes while the Rust implementation folds
 * chars: collapsing runs is what makes them agree on a name with a non-ASCII
 * character in it (one multi-byte character = one run either way). */
static void sanitize_in_place(char *s) {
    size_t o = 0;
    bool last_was_sep = false;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) {
            s[o++] = (char)c;
            last_was_sep = false;
        } else if (!last_was_sep) {
            s[o++] = '_';
            last_was_sep = true;
        }
    }
    s[o] = '\0';
}

char *tdot_param_set_name(const char *qualifier, const char *group) {
    if (!group || !*group)
        group = TDOT_PARAM_DEFAULT_GROUP;
    if (!qualifier)
        qualifier = "";
    /* Assembled first and sanitized as a whole, so a qualifier that already
     * ends in a separator does not produce a doubled '_'. */
    size_t n = strlen(qualifier) + 1 + strlen(group) + sizeof("_parameters");
    char *out = malloc(n);
    snprintf(out, n, "%s_%s_parameters", qualifier, group);
    sanitize_in_place(out);
    return out;
}

tdot_set_naming_t tdot_param_naming(const tdot_device_t *dev,
                                    const char *protocol, const char *forced) {
    tdot_set_naming_t naming = {forced, protocol};
    if (dev && dev->type && *dev->type)
        naming.qualifier = dev->type;
    return naming;
}

bool tdot_param_key_valid(const char *key) {
    if (!key || !*key)
        return false;
    for (const char *p = key; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return false;
    return true;
}

/* The point's `meta.parameter` normalized to an object, or NULL when the point
 * is not a parameter. Caller cJSON_Delete()s the result.
 * Mirrors descriptor.rs::parameter_of. */
static cJSON *parameter_options(const tdot_point_t *point) {
    cJSON *meta = point->meta_json ? cJSON_Parse(point->meta_json) : NULL;
    cJSON *param =
        meta ? cJSON_GetObjectItemCaseSensitive(meta, "parameter") : NULL;
    cJSON *options = NULL;

    if (param && cJSON_IsFalse(param)) { /* explicit opt-out */
        cJSON_Delete(meta);
        return NULL;
    }
    if (param) {
        if (cJSON_IsObject(param))
            options = cJSON_Duplicate(param, 1);
        else if (cJSON_IsString(param)) {
            options = cJSON_CreateObject(); /* a bare string names the set */
            cJSON_AddStringToObject(options, "set", param->valuestring);
        } else {
            options = cJSON_CreateObject(); /* `true`, or any other scalar */
        }
    } else if (point->access & TDOT_ACCESS_WRITE) {
        options = cJSON_CreateObject(); /* writable, no meta.parameter */
    }
    cJSON_Delete(meta);
    return options;
}

/* Append `name` to a growing string list unless it is empty or already there.
 * Takes ownership of `name` (frees it when it is a duplicate). */
static void push_name(char ***list, size_t *n, char *name) {
    if (!name || !*name) {
        free(name);
        return;
    }
    for (size_t i = 0; i < *n; i++)
        if (strcmp((*list)[i], name) == 0) {
            free(name);
            return;
        }
    *list = realloc(*list, (*n + 1) * sizeof **list);
    (*list)[(*n)++] = name;
}

/* The names a `set`/`group` option holds: one string, or an array of them.
 * Empty and non-string entries are ignored, so a mistyped entry degrades to the
 * default group rather than inventing a set name. Mirrors
 * descriptor.rs::names_of. */
static char **names_of(const cJSON *options, const char *key, size_t *n) {
    char **names = NULL;
    *n = 0;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(options, key);
    if (cJSON_IsString(value)) {
        push_name(&names, n, strdup(value->valuestring));
    } else if (cJSON_IsArray(value)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, value) {
            if (cJSON_IsString(item))
                push_name(&names, n, strdup(item->valuestring));
        }
    }
    return names;
}

/* The dot of a key that names its set too, `<set>.<key>`: its only dot, with
 * both halves non-empty. NULL for a plain key -- and for one with more dots or
 * an empty half, which stays a plain key and is refused by
 * tdot_param_invalid_keys. Mirrors descriptor.rs::dotted_key. */
static const char *dotted_key_dot(const cJSON *options) {
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(options, "key");
    if (!cJSON_IsString(key))
        return NULL;
    const char *dot = strchr(key->valuestring, '.');
    if (!dot || dot == key->valuestring || !dot[1] || strchr(dot + 1, '.'))
        return NULL;
    return dot;
}

/* The fragment a literal parameter is published as (`meta.parameter.fragment`),
 * or NULL when the point names none. A fragment that is not a string reads as
 * "", so tdot_param_invalid_keys refuses it rather than the point silently
 * landing in a set. Mirrors descriptor.rs::fragment_of. */
static const char *literal_fragment(const cJSON *options) {
    const cJSON *fragment = cJSON_GetObjectItemCaseSensitive(options, "fragment");
    if (!fragment)
        return NULL;
    return cJSON_IsString(fragment) ? fragment->valuestring : "";
}

/* Every set the options put the point in. A key naming its set is absolute and
 * wins; then `set` is absolute (used verbatim) and wins over `group`; each
 * accepts a string or a list. Mirrors descriptor.rs::SetNaming::sets_of. */
static char **sets_of(const cJSON *options, const tdot_set_naming_t *naming,
                      size_t *n) {
    /* A point naming a fragment IS that fragment: it is in no set, so `set`,
     * `group`, `key` and the forced set do not apply. Kept even when empty, so
     * the point stays a parameter for tdot_param_invalid_keys to refuse. */
    const char *fragment = literal_fragment(options);
    if (fragment) {
        char **only = malloc(sizeof *only);
        only[0] = strdup(fragment);
        *n = 1;
        return only;
    }
    const char *dot = dotted_key_dot(options);
    if (dot) {
        const char *key = cJSON_GetObjectItemCaseSensitive(options, "key")->valuestring;
        char **dotted = NULL;
        *n = 0;
        push_name(&dotted, n, strndup(key, (size_t)(dot - key)));
        return dotted;
    }
    char **sets = names_of(options, "set", n);
    if (*n)
        return sets; /* absolute */
    if (naming->forced) {
        push_name(&sets, n, strdup(naming->forced));
        return sets;
    }
    size_t ngroups = 0;
    char **groups = names_of(options, "group", &ngroups);
    if (!ngroups) {
        push_name(&sets, n,
                  tdot_param_set_name(naming->qualifier,
                                      TDOT_PARAM_DEFAULT_GROUP));
    }
    /* Deduped on the resulting names, not the group names: two groups can fold
     * to one set ("a b" and "a-b") and a point must not appear twice in one
     * definition. */
    for (size_t i = 0; i < ngroups; i++)
        push_name(&sets, n, tdot_param_set_name(naming->qualifier, groups[i]));
    tdot_param_sets_free(groups, ngroups);
    return sets;
}

char **tdot_param_sets(const tdot_point_t *point,
                       const tdot_set_naming_t *naming, size_t *n) {
    *n = 0;
    cJSON *options = parameter_options(point);
    if (!options)
        return NULL;
    char **sets = sets_of(options, naming, n);
    cJSON_Delete(options);
    return sets;
}

void tdot_param_sets_free(char **sets, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(sets[i]);
    free(sets);
}

bool tdot_param_is(const tdot_point_t *point, const tdot_set_naming_t *naming) {
    size_t n = 0;
    char **sets = tdot_param_sets(point, naming, &n);
    bool is = sets != NULL;
    tdot_param_sets_free(sets, n);
    return is;
}

/* True when the point is a parameter kept in a set rather than a literal
 * fragment. A device whose only parameters are literals names no set after its
 * type or protocol, so the type warnings have nothing to say about it. Mirrors
 * descriptor.rs::in_a_set. */
static bool in_a_set(const tdot_point_t *point) {
    cJSON *options = parameter_options(point);
    bool in = options && !literal_fragment(options);
    cJSON_Delete(options);
    return in;
}

/* Append "sep"-joined text to a growing heap string. */
static void append(char **buf, size_t *len, const char *sep, const char *text) {
    size_t add = strlen(text) + (*len ? strlen(sep) : 0);
    *buf = realloc(*buf, *len + add + 1);
    if (*len)
        memcpy(*buf + *len, sep, strlen(sep));
    strcpy(*buf + *len + (*len ? strlen(sep) : 0), text);
    *len += add;
}

/* Append a formatted item, built on the heap: the names in it are arbitrary
 * configured strings, and a fixed buffer would truncate where the Rust build
 * does not. */
static void append_fmt(char **buf, size_t *len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    char *item = malloc((size_t)n + 1);
    if (!item)
        return;
    va_start(ap, fmt);
    vsnprintf(item, (size_t)n + 1, fmt, ap);
    va_end(ap);
    append(buf, len, ", ", item);
    free(item);
}

char *tdot_param_invalid_keys(const tdot_config_t *cfg, const char *forced) {
    return tdot_param_invalid_keys_across(&cfg, 1, forced);
}

/* The key a parameter has inside its sets: `meta.parameter.key` when it is a
 * string -- its part after the set, for a key naming its set -- else the point
 * id. An unusable string is kept, so tdot_param_invalid_keys reports it.
 * Mirrors descriptor.rs::parameters_of. */
static const char *param_key(const tdot_point_t *point, const cJSON *options) {
    if (literal_fragment(options))
        return ""; /* a literal has no key: it is the fragment */
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(options, "key");
    if (!cJSON_IsString(key))
        return point->id;
    const char *dot = dotted_key_dot(options);
    return dot ? dot + 1 : key->valuestring;
}

char *tdot_param_invalid_keys_across(const tdot_config_t *const *cfgs,
                                     size_t ncfgs, const char *forced) {
    char *buf = NULL;
    size_t len = 0;
    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const tdot_device_t *dev = &cfg->devices[i];
            tdot_set_naming_t naming =
                tdot_param_naming(dev, cfg->protocol, forced);
            for (size_t j = 0; j < dev->npoints; j++) {
                const tdot_point_t *pt = &dev->points[j];
                cJSON *options = parameter_options(pt);
                if (!options)
                    continue;
                size_t nsets = 0;
                char **sets = sets_of(options, &naming, &nsets);
                const char *key = param_key(pt, options);
                const char *fragment = literal_fragment(options);
                /* A literal parameter is published as its fragment: only that
                 * name has to be usable -- not the id, and there is no key. */
                if (fragment) {
                    if (!tdot_param_key_valid(fragment))
                        append_fmt(&buf, &len,
                                   "parameter fragment '%s' of point '%s'",
                                   fragment, pt->id);
                    tdot_param_sets_free(sets, nsets);
                    cJSON_Delete(options);
                    continue;
                }
                /* A point that names its key is published under the key, so
                 * only the key has to be usable -- not the id. */
                if (strcmp(key, pt->id) != 0) {
                    if (!tdot_param_key_valid(key))
                        append_fmt(&buf, &len, "parameter key '%s' of point '%s'",
                                   key, pt->id);
                } else if (!tdot_param_key_valid(pt->id)) {
                    append_fmt(&buf, &len, "point id '%s'", pt->id);
                }
                for (size_t k = 0; k < nsets; k++)
                    if (!tdot_param_key_valid(sets[k]))
                        append_fmt(&buf, &len, "parameter set '%s'", sets[k]);
                tdot_param_sets_free(sets, nsets);
                cJSON_Delete(options);
            }
        }
    }
    return buf;
}

/* Names used both as a literal fragment and as a parameter set, across every
 * configuration: one item per name, naming the first point declaring the
 * fragment and the first device using the set, in the order the fragments are
 * first declared. Mirrors descriptor.rs::fragment_set_clashes. */
static void fragment_set_clashes(const tdot_config_t *const *cfgs, size_t ncfgs,
                                 const char *forced, char **buf, size_t *len) {
    /* fragment -> [point, device] declaring it first; set -> device using it first */
    cJSON *literals = cJSON_CreateObject();
    cJSON *sets_seen = cJSON_CreateObject();
    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const tdot_device_t *dev = &cfg->devices[i];
            tdot_set_naming_t naming =
                tdot_param_naming(dev, cfg->protocol, forced);
            for (size_t j = 0; j < dev->npoints; j++) {
                const tdot_point_t *pt = &dev->points[j];
                cJSON *options = parameter_options(pt);
                if (!options)
                    continue;
                bool literal = literal_fragment(options) != NULL;
                size_t nsets = 0;
                char **sets = sets_of(options, &naming, &nsets);
                for (size_t k = 0; k < nsets; k++) {
                    if (literal && !cJSON_GetObjectItemCaseSensitive(literals, sets[k])) {
                        cJSON *first = cJSON_AddArrayToObject(literals, sets[k]);
                        cJSON_AddItemToArray(first, cJSON_CreateString(pt->id));
                        cJSON_AddItemToArray(first, cJSON_CreateString(dev->name));
                    } else if (!literal &&
                               !cJSON_GetObjectItemCaseSensitive(sets_seen, sets[k])) {
                        cJSON_AddStringToObject(sets_seen, sets[k], dev->name);
                    }
                }
                tdot_param_sets_free(sets, nsets);
                cJSON_Delete(options);
            }
        }
    }
    const cJSON *first;
    cJSON_ArrayForEach(first, literals) {
        const cJSON *set_device =
            cJSON_GetObjectItemCaseSensitive(sets_seen, first->string);
        if (cJSON_IsString(set_device))
            append_fmt(buf, len,
                       "fragment '%s' of point '%s' on device '%s' is also a "
                       "parameter set on device '%s'",
                       first->string, cJSON_GetArrayItem(first, 0)->valuestring,
                       cJSON_GetArrayItem(first, 1)->valuestring,
                       set_device->valuestring);
    }
    cJSON_Delete(literals);
    cJSON_Delete(sets_seen);
}

char *tdot_param_key_conflicts(const tdot_config_t *cfg, const char *forced) {
    return tdot_param_key_conflicts_across(&cfg, 1, forced);
}

char *tdot_param_key_conflicts_across(const tdot_config_t *const *cfgs,
                                      size_t ncfgs, const char *forced) {
    char *buf = NULL;
    size_t len = 0;
    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const tdot_device_t *dev = &cfg->devices[i];
            tdot_set_naming_t naming =
                tdot_param_naming(dev, cfg->protocol, forced);
            /* set name -> { key -> the point of this device that has it first } */
            cJSON *seen = cJSON_CreateObject();
            for (size_t j = 0; j < dev->npoints; j++) {
                const tdot_point_t *pt = &dev->points[j];
                cJSON *options = parameter_options(pt);
                if (!options)
                    continue;
                size_t nsets = 0;
                char **sets = sets_of(options, &naming, &nsets);
                const char *key = param_key(pt, options);
                const char *fragment = literal_fragment(options);
                /* A literal is in no set: nothing may place it in one. */
                if (fragment) {
                    static const char *placing[] = {"set", "group", "key"};
                    for (size_t o = 0; o < sizeof placing / sizeof *placing; o++)
                        if (cJSON_GetObjectItemCaseSensitive(options, placing[o])) {
                            append_fmt(&buf, &len,
                                       "point '%s' on device '%s' combines "
                                       "\"fragment\" with \"%s\": a literal "
                                       "parameter is in no set",
                                       pt->id, dev->name, placing[o]);
                            break;
                        }
                }
                /* A key names its set itself (`<set>.<key>`): no room for `set`,
                 * and no room for `group` once it does. */
                else if (nsets && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(options, "key"))) {
                    if (cJSON_GetObjectItemCaseSensitive(options, "set"))
                        append_fmt(&buf, &len,
                                   "point '%s' on device '%s' combines \"set\" with "
                                   "\"key\": write the key as '<set>.<key>'",
                                   pt->id, dev->name);
                    else if (dotted_key_dot(options) &&
                             cJSON_GetObjectItemCaseSensitive(options, "group"))
                        append_fmt(&buf, &len,
                                   "point '%s' on device '%s' combines \"group\" with a "
                                   "key that names its set",
                                   pt->id, dev->name);
                }
                for (size_t k = 0; k < nsets; k++) {
                    cJSON *keys = cJSON_GetObjectItemCaseSensitive(seen, sets[k]);
                    if (!keys)
                        keys = cJSON_AddObjectToObject(seen, sets[k]);
                    const cJSON *first = cJSON_GetObjectItemCaseSensitive(keys, key);
                    if (cJSON_IsString(first) && fragment) {
                        append_fmt(&buf, &len,
                                   "fragment '%s' of points '%s' and '%s' on "
                                   "device '%s'",
                                   sets[k], first->valuestring, pt->id,
                                   dev->name);
                    } else if (cJSON_IsString(first)) {
                        append_fmt(&buf, &len,
                                   "key '%s' of points '%s' and '%s' in set '%s' "
                                   "on device '%s'",
                                   key, first->valuestring, pt->id, sets[k],
                                   dev->name);
                    } else {
                        cJSON_AddStringToObject(keys, key, pt->id);
                    }
                }
                tdot_param_sets_free(sets, nsets);
                cJSON_Delete(options);
            }
            cJSON_Delete(seen);
        }
    }
    fragment_set_clashes(cfgs, ncfgs, forced, &buf, &len);
    return buf;
}

char *tdot_param_type_warnings(const tdot_config_t *cfg) {
    return tdot_param_type_warnings_across(&cfg, 1);
}

char *tdot_param_type_warnings_across(const tdot_config_t *const *cfgs,
                                      size_t ncfgs) {
    /* Grouped by the set name the types actually derive -- the same string the
     * Rust build groups and displays by, so there is no second notion of
     * "qualifier" for the two to disagree about. Everything here is heap-built:
     * a device type is an arbitrary configured string, and a truncated warning
     * would name a type that is not in the configuration. */
    struct group {
        char *key;    /* representative set name */
        char **types; /* distinct raw types deriving it */
        size_t ntypes;
    } *groups = NULL;
    size_t ngroups = 0;

    char *buf = NULL;
    size_t len = 0;

    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const char *declared = cfg->devices[i].type;
            if (!declared || !*declared)
                continue;
            /* A type with nothing usable in it ("日本語", "---") folds away
             * entirely and the sets are named "_control_parameters" -- which
             * every such type shares, silently. */
            bool usable = false;
            for (const char *p = declared; *p; p++)
                if (isalnum((unsigned char)*p)) {
                    usable = true;
                    break;
                }
            if (!usable) {
                char *derived =
                    tdot_param_set_name(declared, TDOT_PARAM_DEFAULT_GROUP);
                static const char *UNUSABLE_FMT =
                    "warning: device type '%s' has no [A-Za-z0-9] character, so "
                    "its parameter sets are named '%s' with nothing to tell them "
                    "apart from another such type's; name the type in ASCII";
                size_t ulen = strlen(UNUSABLE_FMT) + strlen(declared) +
                              strlen(derived) + 1;
                char *msg = malloc(ulen);
                if (msg) {
                    snprintf(msg, ulen, UNUSABLE_FMT, declared, derived);
                    append(&buf, &len, "\n", msg);
                    free(msg);
                }
                free(derived);
            }
            /* A device with no parameters derives no set, so it cannot
             * collide. */
            bool has_parameters = false;
            for (size_t j = 0; j < cfg->devices[i].npoints; j++)
                if (in_a_set(&cfg->devices[i].points[j])) {
                    has_parameters = true;
                    break;
                }
            if (!has_parameters)
                continue;
            char *key = tdot_param_set_name(declared, TDOT_PARAM_DEFAULT_GROUP);
            struct group *g = NULL;
            for (size_t k = 0; k < ngroups; k++)
                if (strcmp(groups[k].key, key) == 0) {
                    g = &groups[k];
                    break;
                }
            if (!g) {
                groups = realloc(groups, (ngroups + 1) * sizeof *groups);
                g = &groups[ngroups++];
                g->key = key;
                g->types = NULL;
                g->ntypes = 0;
            } else {
                free(key);
            }
            bool seen = false;
            for (size_t t = 0; t < g->ntypes; t++)
                if (strcmp(g->types[t], declared) == 0) {
                    seen = true; /* one type on two devices is one device type */
                    break;
                }
            if (!seen) {
                g->types =
                    realloc(g->types, (g->ntypes + 1) * sizeof *g->types);
                g->types[g->ntypes++] = strdup(declared);
            }
        }
    }

    static const char *FMT =
        "warning: device types %s derive the same parameter set names (e.g. "
        "'%s'), so they share one tenant-wide definition and the first one "
        "rendered wins; give them names that differ by more than punctuation";
    for (size_t k = 0; k < ngroups; k++) {
        /* More than one DISTINCT type is the collision -- counted, never
         * inferred from the rendered text (a type may contain a comma). */
        if (groups[k].ntypes > 1) {
            char *names = NULL;
            size_t nlen = 0;
            for (size_t t = 0; t < groups[k].ntypes; t++) {
                size_t qlen = strlen(groups[k].types[t]) + 3;
                char *quoted = malloc(qlen);
                if (quoted) {
                    snprintf(quoted, qlen, "'%s'", groups[k].types[t]);
                    append(&names, &nlen, ", ", quoted);
                    free(quoted);
                }
            }
            size_t mlen = strlen(FMT) + nlen + strlen(groups[k].key) + 1;
            char *msg = malloc(mlen);
            if (msg) {
                snprintf(msg, mlen, FMT, names ? names : "", groups[k].key);
                append(&buf, &len, "\n", msg);
                free(msg);
            }
            free(names);
        }
        for (size_t t = 0; t < groups[k].ntypes; t++)
            free(groups[k].types[t]);
        free(groups[k].types);
        free(groups[k].key);
    }
    free(groups);
    return buf;
}

char *tdot_param_untyped_devices(const tdot_config_t *cfg) {
    return tdot_param_untyped_devices_across(&cfg, 1, cfg->protocol);
}

char *tdot_param_untyped_devices_across(const tdot_config_t *const *cfgs,
                                        size_t ncfgs, const char *protocol) {
    char *buf = NULL;
    size_t len = 0;
    const char **listed = NULL; /* borrowed names, each warned about once */
    size_t nlisted = 0;
    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        if (strcmp(cfg->protocol, protocol) != 0)
            continue;
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const tdot_device_t *dev = &cfg->devices[i];
            if (dev->type && *dev->type)
                continue;
            bool has_parameters = false;
            for (size_t j = 0; j < dev->npoints && !has_parameters; j++)
                has_parameters = in_a_set(&dev->points[j]);
            /* Named once, even when several files define a device of that name. */
            bool seen = false;
            for (size_t k = 0; k < nlisted && !seen; k++)
                seen = strcmp(listed[k], dev->name) == 0;
            if (!has_parameters || seen)
                continue;
            listed = realloc(listed, (nlisted + 1) * sizeof *listed);
            listed[nlisted++] = dev->name;
            append(&buf, &len, ", ", dev->name);
        }
    }
    free(listed);
    return buf;
}

/* JSON-schema type and datatype range for a point's datatype. 64-bit limits
 * exceed the JS safe range, so they are left unbounded (as in Rust). */
static const char *schema_type(tdot_datatype_t dt, bool *has_range, double *min,
                               double *max) {
    *has_range = true;
    switch (dt) {
    case TDOT_DT_BOOL:
        *has_range = false;
        return "boolean";
    case TDOT_DT_INT8:
        *min = -128.0, *max = 127.0;
        return "integer";
    case TDOT_DT_UINT8:
        *min = 0.0, *max = 255.0;
        return "integer";
    case TDOT_DT_INT16:
        *min = -32768.0, *max = 32767.0;
        return "integer";
    case TDOT_DT_UINT16:
        *min = 0.0, *max = 65535.0;
        return "integer";
    case TDOT_DT_INT32:
        *min = -2147483648.0, *max = 2147483647.0;
        return "integer";
    case TDOT_DT_UINT32:
        *min = 0.0, *max = 4294967295.0;
        return "integer";
    case TDOT_DT_INT64:
    case TDOT_DT_UINT64:
        *has_range = false;
        return "integer";
    case TDOT_DT_FLOAT32:
    case TDOT_DT_FLOAT64:
        *has_range = false;
        return "number";
    default: /* string / raw / unset */
        *has_range = false;
        return "string";
    }
}

static const char *opt_string(const cJSON *options, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(options, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static cJSON *map_val_json(const tdot_map_val_t *v) {
    switch (v->kind) {
    case TDOT_MAP_NUMBER: return cJSON_CreateNumber(v->num);
    case TDOT_MAP_STRING: return cJSON_CreateString(v->s);
    case TDOT_MAP_BOOL: return cJSON_CreateBool(v->b);
    }
    return cJSON_CreateNull();
}

/* The JSON-schema type of a mapped parameter, adding its `enum` to `schema`
 * when the map's writable values are a closed set; no datatype range. */
static const char *mapped_schema(const tdot_map_t *map, cJSON *schema, bool *has_range) {
    *has_range = false;
    if (!map->has_as) {
        const tdot_map_val_t *choices[256];
        size_t n = tdot_map_writable(map, choices, 256);
        if (n > 256)
            n = 256;
        if (n) {
            cJSON *list = cJSON_AddArrayToObject(schema, "enum");
            for (size_t i = 0; i < n; i++)
                cJSON_AddItemToArray(list, map_val_json(choices[i]));
        }
    }
    switch (tdot_map_output_kind(map)) {
    case TDOT_MAP_BOOL: return "boolean";
    case TDOT_MAP_STRING: return "string";
    case TDOT_MAP_NUMBER: break;
    }
    if (map->has_as)
        return "number";
    /* an integer only when every numeric output is whole */
    for (size_t i = 0; i < map->ncases; i++)
        if (map->cases[i].to.num != floor(map->cases[i].to.num))
            return "number";
    if (map->has_default && map->def.num != floor(map->def.num))
        return "number";
    return "integer";
}

/* JSON-schema property for one parameter: type and limits from the datatype,
 * everything else from `meta.parameter`. `key` is the property's key in its
 * set (param_key). */
static cJSON *property_schema(const tdot_point_t *point, const char *key,
                              const cJSON *options) {
    cJSON *schema = cJSON_CreateObject();
    bool has_range = false;
    double min = 0, max = 0;
    const char *type = schema_type(point->datatype, &has_range, &min, &max);
    /* A mapped parameter (§4.3) holds mapped values: its type is the map's
     * output type, the datatype's limits describe the device value and do not
     * apply, and a map whose writable values form a closed set (no `as`)
     * offers them as a choice. */
    if (point->map)
        type = mapped_schema(point->map, schema, &has_range);
    cJSON_AddStringToObject(schema, "type", type);

    /* meta.parameter.title wins, then the point's own `name`, then the key: a
     * point can carry a general-purpose label and still say something
     * different in the parameter UI (mirrors descriptor.rs property_schema). */
    const char *title = opt_string(options, "title");
    if (!title)
        title = point->name;
    cJSON_AddStringToObject(schema, "title", title ? title : key);

    /* Heap-built, because `description` and `unit` are arbitrary configured
     * strings: a fixed buffer would truncate where the Rust SDK does not, and
     * the two builds must render the same definition. */
    const char *d = opt_string(options, "description");
    if (!d)
        d = point->description;
    static const char *WRITE_ONLY = "(write-only: shows the last value written)";
    size_t len = (d ? strlen(d) : 0) +
                 (point->unit ? strlen(point->unit) + 4 : 0) +
                 (point->access == TDOT_ACCESS_WRITE ? strlen(WRITE_ONLY) + 1 : 0) + 1;
    char *description = calloc(1, len);
    if (description) {
        size_t n = 0;
        if (d)
            n += (size_t)snprintf(description + n, len - n, "%s", d);
        if (point->unit)
            n += (size_t)snprintf(description + n, len - n, "%s[%s]",
                                  n ? " " : "", point->unit);
        if (point->access == TDOT_ACCESS_WRITE)
            n += (size_t)snprintf(description + n, len - n, "%s%s", n ? " " : "",
                                  WRITE_ONLY);
        if (*description)
            cJSON_AddStringToObject(schema, "description", description);
        free(description);
    }

    const cJSON *opt_min = cJSON_GetObjectItemCaseSensitive(options, "min");
    const cJSON *opt_max = cJSON_GetObjectItemCaseSensitive(options, "max");
    if (cJSON_IsNumber(opt_min))
        cJSON_AddNumberToObject(schema, "minimum", opt_min->valuedouble);
    else if (has_range)
        cJSON_AddNumberToObject(schema, "minimum", min);
    if (cJSON_IsNumber(opt_max))
        cJSON_AddNumberToObject(schema, "maximum", opt_max->valuedouble);
    else if (has_range)
        cJSON_AddNumberToObject(schema, "maximum", max);

    static const char *passthrough[] = {"enum", "default", "order"};
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(options, passthrough[i]);
        /* replaces a mapped parameter's own `enum`: an explicit one wins */
        if (v && cJSON_GetObjectItemCaseSensitive(schema, passthrough[i]))
            cJSON_ReplaceItemInObjectCaseSensitive(schema, passthrough[i], cJSON_Duplicate(v, 1));
        else if (v)
            cJSON_AddItemToObject(schema, passthrough[i], cJSON_Duplicate(v, 1));
    }
    if (!(point->access & TDOT_ACCESS_WRITE))
        cJSON_AddTrueToObject(schema, "readOnly");
    return schema;
}

/* "modbus_control_parameters" -> "Modbus control parameters" (only the first word is
 * capitalized, as in Rust's title_from_key). Caller frees. */
static char *title_from_key(const char *key) {
    /* Heap, not a fixed buffer: the key contains the device type (§3.1), which
     * is an arbitrary configured string. The old fixed buffer both truncated
     * where Rust does not AND could write its terminator one byte past the end,
     * because the word-start branch emits two characters in one iteration.
     * Every input character yields at most one output character ('_' becomes a
     * single space or nothing), so strlen(key) + 1 always fits. */
    size_t o = 0;
    char *out = malloc(strlen(key) + 1);
    if (!out)
        return NULL;
    bool first_word = true, word_start = true;
    for (const char *p = key; *p; p++) {
        if (*p == '_') {
            if (!word_start) {
                word_start = true;
                first_word = false;
            }
            continue;
        }
        if (word_start) {
            if (!first_word)
                out[o++] = ' ';
            out[o++] = first_word ? (char)toupper((unsigned char)*p) : *p;
            word_start = false;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return out;
}

/* The `contexts` and `tags` every definition carries. */
static void add_contexts_and_tags(cJSON *doc, const char *protocol) {
    cJSON *contexts = cJSON_AddArrayToObject(doc, "contexts");
    cJSON_AddItemToArray(contexts, cJSON_CreateString("asset"));
    cJSON_AddItemToArray(contexts, cJSON_CreateString("event"));
    cJSON_AddItemToArray(contexts, cJSON_CreateString("operation"));

    cJSON *tags = cJSON_AddArrayToObject(doc, "tags");
    cJSON_AddItemToArray(tags, cJSON_CreateString("tedge-dot"));
    cJSON_AddItemToArray(tags, cJSON_CreateString(protocol));
}

cJSON *tdot_c8y_dtm_definitions(const tdot_config_t *cfg, const char *forced) {
    return tdot_c8y_dtm_definitions_across(&cfg, 1, forced);
}

cJSON *tdot_c8y_dtm_definitions_across(const tdot_config_t *const *cfgs,
                                       size_t ncfgs, const char *forced) {
    /* One entry per set, in configuration order; `properties` keeps the order
     * the points are configured in so the default `order` matches Rust. */
    cJSON *sets = cJSON_CreateObject(); /* set name -> properties object */
    /* set name -> protocol of the config that declared it first */
    cJSON *protocols = cJSON_CreateObject();
    /* set name -> true when its first declaration is a literal fragment */
    cJSON *literals = cJSON_CreateObject();
    for (size_t c = 0; c < ncfgs; c++) {
        const tdot_config_t *cfg = cfgs[c];
        for (size_t i = 0; i < cfg->ndevices; i++) {
            const tdot_device_t *dev = &cfg->devices[i];
            tdot_set_naming_t naming =
                tdot_param_naming(dev, cfg->protocol, forced);
            for (size_t j = 0; j < dev->npoints; j++) {
                const tdot_point_t *pt = &dev->points[j];
                cJSON *options = parameter_options(pt);
                if (!options)
                    continue;
                size_t nsets = 0;
                char **point_sets = sets_of(options, &naming, &nsets);
                bool literal = literal_fragment(options) != NULL;
                for (size_t k = 0; k < nsets; k++) {
                    cJSON *props =
                        cJSON_GetObjectItemCaseSensitive(sets, point_sets[k]);
                    if (!props) {
                        props = cJSON_AddObjectToObject(sets, point_sets[k]);
                        cJSON_AddStringToObject(protocols, point_sets[k],
                                                cfg->protocol);
                        cJSON_AddBoolToObject(literals, point_sets[k], literal);
                    }
                    /* A name declared as a literal and as a set is refused by
                     * tdot_param_key_conflicts; if it gets here anyway, its
                     * first declaration decides the shape. */
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                            literals, point_sets[k])) != literal)
                        continue;
                    const char *key = param_key(pt, options);
                    if (cJSON_GetObjectItemCaseSensitive(props, key))
                        continue; /* first definition wins */
                    /* A literal has no key: it is titled like a set, after its
                     * fragment. */
                    char *title = literal ? title_from_key(point_sets[k]) : NULL;
                    cJSON_AddItemToObject(props, key,
                                          property_schema(pt, title ? title : key,
                                                          options));
                    free(title);
                }
                tdot_param_sets_free(point_sets, nsets);
                cJSON_Delete(options);
            }
        }
    }

    cJSON *docs = cJSON_CreateArray();
    cJSON *props;
    cJSON_ArrayForEach(props, sets) {
        const char *protocol =
            cJSON_GetObjectItemCaseSensitive(protocols, props->string)->valuestring;
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "identifier", props->string);

        cJSON *schema = cJSON_AddObjectToObject(doc, "jsonSchema");
        cJSON_AddStringToObject(schema, "$schema",
                                "http://json-schema.org/draft-07/schema#");
        /* A literal parameter's property schema is the whole jsonSchema -- a
         * primitive, not an object with one property -- and `order`, which
         * places a property inside a set, has nothing to order. */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(literals, props->string))) {
            const cJSON *field;
            cJSON_ArrayForEach(field, props->child) {
                if (strcmp(field->string, "order") != 0)
                    cJSON_AddItemToObject(schema, field->string,
                                          cJSON_Duplicate(field, 1));
            }
            add_contexts_and_tags(doc, protocol);
            cJSON_AddItemToArray(docs, doc);
            continue;
        }
        char *title = title_from_key(props->string);
        cJSON_AddStringToObject(schema, "title", title ? title : props->string);
        free(title);
        static const char *DESC_FMT =
            "Writable %s points exposed by tedge-dot (generated from the "
            "connector configuration)";
        size_t dlen = strlen(DESC_FMT) + strlen(protocol) + 1;
        char *description = malloc(dlen);
        if (description) {
            snprintf(description, dlen, DESC_FMT, protocol);
            cJSON_AddStringToObject(schema, "description", description);
            free(description);
        }
        cJSON_AddStringToObject(schema, "type", "object");

        /* Properties without an explicit `order` get their 1-based position. */
        int position = 0;
        cJSON *prop;
        cJSON_ArrayForEach(prop, props) {
            position++;
            if (!cJSON_GetObjectItemCaseSensitive(prop, "order"))
                cJSON_AddNumberToObject(prop, "order", position);
        }
        cJSON_AddItemToObject(schema, "properties", cJSON_Duplicate(props, 1));

        add_contexts_and_tags(doc, protocol);
        cJSON_AddItemToArray(docs, doc);
    }
    cJSON_Delete(sets);
    cJSON_Delete(protocols);
    cJSON_Delete(literals);
    return docs;
}
