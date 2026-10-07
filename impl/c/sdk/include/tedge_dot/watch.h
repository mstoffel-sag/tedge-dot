#ifndef TEDGE_DOT_WATCH_H
#define TEDGE_DOT_WATCH_H

/* Config file watching for the run service (openspec change
 * config-file-watch-reload), the C side of impl/rust/src/watch.rs.
 *
 * Notices, by polling file metadata, when the files a reload would read have
 * changed: every config file the `run` argument names (a directory's *.toml,
 * so an added or removed file counts) and the point-library files those
 * configs reference. It only decides when to reload; the reload is the SIGHUP
 * path. A change is acted on once it has settled (the next poll shows the same
 * state again), and the state that triggers a reload becomes the baseline
 * before the reload runs, so a write landing during the reload gets its own. */

#include <stdbool.h>
#include <stddef.h>

#define TDOT_WATCH_INTERVAL_ENV "TEDGE_DOT_CONFIG_WATCH_INTERVAL"
#define TDOT_WATCH_DEFAULT_S 2.0
#define TDOT_WATCH_MIN_S 0.2

/* The poll interval for the environment value `value` (NULL = unset), in
 * seconds; 0 when watching is off. An invalid value gives the default, a value
 * below the minimum the minimum; both fill `warning` (empty otherwise). */
double tdot_watch_interval(const char *value, char *warning, size_t warnlen);

/* Lists the config files to watch, like the run service's discovery: 0 and a
 * malloc'd array of malloc'd paths, or -1 (nothing listed). */
typedef int (*tdot_watch_list_fn)(void *ctx, char ***paths, size_t *npaths);

typedef struct tdot_watch tdot_watch_t;

tdot_watch_t *tdot_watch_new(tdot_watch_list_fn list, void *ctx);
void tdot_watch_free(tdot_watch_t *w);

/* Re-read which point libraries the configs reference; call after each
 * reload. A newly watched file counts as added at the next poll. */
void tdot_watch_rebuild(tdot_watch_t *w);

/* One poll. True when a settled change calls for a reload; then `changed`
 * holds the changed paths, comma separated (truncated to `changedlen`). */
bool tdot_watch_poll(tdot_watch_t *w, char *changed, size_t changedlen);

#endif
