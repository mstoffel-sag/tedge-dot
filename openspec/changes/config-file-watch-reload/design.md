# Design

## Context

- Reload already exists in both builds and is diff-based. Rust: `run()` in `impl/rust/src/main.rs`
  waits for SIGHUP, then calls `discover_configs` and `Connectors::reconcile`. Each running
  connector calls `reload_from_file`, which returns `Unchanged` when the parsed config equals the
  running one. C: `on_hangup` bumps `g_reload_gen`; the supervisor rediscovers configs through
  `rediscover_configs`, and each connector's loop calls `reload_from_file`, which compares the
  serialized config and logs "reload: <path> is unchanged".
- The service's own writes (`persist_config` in Rust, its counterpart in C) write a temporary file
  next to the config and rename it into place. The temporary name does not end in `.toml`, so
  discovery ignores it.
- Point libraries are looked up on `TEDGE_DOT_POINT_LIBRARY_PATH`, else
  `/etc/tedge/plugins/ot/points.d` and `/usr/share/tedge-dot/points.d`. A reload re-reads the
  libraries a config references.
- The packaged unit runs `tedge-dot run /etc/tedge/plugins/ot` as `tedge`. thin-edge.io's
  configuration management writes files there through `tedge-write`, and it does not notify
  services.

## Goals / Non-Goals

**Goals:**
- A config written by any means is applied within seconds, through the existing reload path.
- Identical behaviour, settings and log lines in the Rust and the C build.
- No new dependency in either build.

**Non-Goals:**
- Reloading on changes to files the config only points at outside the library search, such as
  PKI files, `*_password_file` secrets or a CAN `.dbc`. These keep needing SIGHUP (or, for PKI,
  the existing `tedge-dot pki` flow). They can be added to the watched set later.
- Changing what a reload does: restart rules, `log_level` needing a service restart, and so on.
- Watching in the one-shot commands (`read`, `write`, `describe`).

## Decisions

### D1: Poll file metadata instead of using inotify/kqueue
Each interval, build a fingerprint of the watched set: for each path its existence, size,
modification time and inode, plus the sorted list of `*.toml` in each config directory. Compare it
with the previous fingerprint.
- *Why:* identical in Rust and C with no dependency (Rust has no file-watch crate today; C would
  need inotify, Linux-only, with different edge cases). It works through container bind mounts and
  network filesystems, where inotify events can be missing. The cost is a few `stat` calls every
  2 s.
- *Alternative:* `notify` (Rust) + inotify (C). Lower latency, but two implementations of event
  semantics to keep at parity, plus handling for rename-over, editor swap files and queue
  overflow. The settle rule (D2) adds a full interval anyway, so the latency difference is small.

### D2: Settle rule
A reload is triggered when the fingerprint changed at some poll and is then the same at the next
poll. A file still being written keeps changing and keeps deferring the reload, and several files
replaced together cause one reload. The worst-case delay is two intervals (4 s with the default).

### D3: One reload path
The watcher does not reload anything itself. In Rust it is one more branch in `run()`'s
`select!`, which calls the same code as SIGHUP (extract the branch body into a function). In C it
is a thread that does `atomic_fetch_add(&g_reload_gen, 1)`, exactly as `on_hangup` does. Ordering,
error handling ("reload failed; the running connectors are unchanged") and restart rules are
therefore shared with SIGHUP, and SIGHUP stays a valid trigger at any time.

### D4: Watched set follows the running configuration
After each reload (and at start), the watched set is rebuilt:
- the config directories and files named on the command line
- every discovered `*.toml`
- every library file that a successfully loaded config resolved

Referenced files come from the loaders, which resolve each `points_from` reference to a path. The
C loader keeps those paths (`lib_paths`, the cache key, in `config.h`). The Rust loader resolves
them into a cache local to the load (`library.rs`, keyed by `PathBuf`) and drops it, so it must
return the resolved paths with the config. Use these lists rather than resolving the search path a
second time, so the watcher and the loader cannot disagree. A library that fails to load is
still watched, by the path it resolved to, so fixing it triggers a reload.

### D5: The service's own writes
They are not filtered. A write by `persist_config` changes the fingerprint and causes a reload that
finds the file equal to the running config. In both builds that is a no-op that logs
"reload: <path> is unchanged". This keeps a single code path and needs no bookkeeping of "expected"
writes. The spec scenario pins the no-op.

### D6: Setting
`TEDGE_DOT_CONFIG_WATCH_INTERVAL`, a duration in the format the configs use (`500ms`, `2s`, `1m`),
default `2s`, with `0` to disable. It's an environment variable, like `TEDGE_DOT_RESTART_DELAY`,
because it concerns the process, not one connector. A connector config key would be ambiguous
with several files. The minimum is 100 ms; below that the minimum is used and a warning logged.

## Risks / Trade-offs

- [Editing a config by hand applies each saved step] An intermediate save that is valid but
  incomplete is applied. → The same as running `systemctl reload` after each save. Documented. An
  invalid intermediate save is logged and ignored (spec).
- [Each detected change logs "reload: <path> is unchanged" for every connector] → Noise
  proportional to the number of configs. Acceptable, and it shows the reload happened. The new
  info line names the changed file (spec).
- [mtime granularity] A rewrite with identical size within the same timestamp tick (1 s on some
  filesystems) could be missed. → The inode changes for rename-into-place writers
  (`tedge-write`, `persist_config`, most editors). An in-place rewrite of the same size in the
  same second is the only gap. SIGHUP remains available.
- [Polling cost on slow storage] A few `stat` calls every 2 s. → Negligible. Configurable, can be
  disabled.

## Migration Plan

On by default after the upgrade. Behaviour changes only for setups that rely on edited files *not*
being applied until a reload; set `TEDGE_DOT_CONFIG_WATCH_INTERVAL=0` in a systemd drop-in to keep
that. Rollback: the previous release ignores the variable and reloads only on SIGHUP. Workarounds
that send SIGHUP after a configuration update (the OPC-UA solution blueprint's `config_update`
workflow step) keep working and can be removed.
