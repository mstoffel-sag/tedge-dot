# Tasks

## 1. Shared reload trigger (Rust)

- [x] 1.1 In `impl/rust/src/main.rs`, move the body of the SIGHUP branch of `run()` (rediscover, warn about duplicates, `reconcile`, "reload failed" error) into one function that both triggers call. Verify the existing reload tests still pass (`cargo test -p tedge-dot`).
- [x] 1.2 Add `library::referenced_library_files` (Rust): a config file's library references resolved with the loader's own `search_path`/`locate`, plus the lookup paths of a reference that does not resolve (design D4). Verify with a unit test: a referenced library and a missing one are listed, an unreferenced one is not, and an unparsable config gives none.

## 2. Watcher (Rust)

- [x] 2.1 Parse `TEDGE_DOT_CONFIG_WATCH_INTERVAL` (duration grammar of the configs, default `2s`, `0` = off, invalid → warning + default, minimum 100 ms). Verify with unit tests for each case.
- [x] 2.2 Build the fingerprint of the watched set (config dirs' `*.toml` listing, named config files, resolved library files: existence, size, mtime, inode), and rebuild the set after every reload. Verify with unit tests: an added, modified, removed or renamed-over file changes the fingerprint; a `.txt` file or an unreferenced library does not.
- [x] 2.3 Add the poll loop to `run()`'s `select!` with the settle rule (changed, then unchanged for one interval → one reload, logged at info with the changed paths), and rebuild the library list after every reload. Verified by the watcher unit tests in `impl/rust/src/watch.rs` (a new file, several writes within an interval → one reload, a write during a reload → one more, `0` → off) and a live `tedge-dot run` against a temp config dir (add → starts, edit → applied, invalid TOML → kept, remove → stopped). The service-level case is covered end to end by 3.3.
- [x] 2.4 Verify that the service's own writes are no-ops: a `define-device` command persists the config, and the following detected reload logs "reload: … is unchanged" without reconnecting (extend the existing management-command test).
- [x] 2.5 Add `--no-watch` to `run`: watching off whatever `TEDGE_DOT_CONFIG_WATCH_INTERVAL` says, logged at start. Verify with a CLI parse test (`run_accepts_no_watch`) and a live run: with the variable at `500ms` and `--no-watch`, an added config starts nothing until SIGHUP.

## 3. Watcher (C)

- [x] 3.1 Add the same interval parsing and fingerprint in C (`sdk/src/watch.c`, `tedge_dot/watch.h`), with `tdot_config_referenced_libraries` in `config.c` for libraries. Verify with C unit tests in `impl/c/tests/watch.c` that mirror 2.1 and 2.2.
- [x] 3.2 Poll the watcher from the supervisor loop, bumping `g_reload_gen` like `on_hangup`, with the same settle rule, baseline (taken before the reload) and log line, and rebuild the watched set after each reload. Verify with a C test against a temp config dir, mirroring 2.3, including the write during a running reload.
- [x] 3.2c Add `--no-watch` to the C `run` (usage text, `tdot_run_opts_t.no_watch`), same behaviour and log line as 2.5. Verify with the same live run.
- [x] 3.2b Let the C service idle on an empty config directory, like Rust (design D7). Verify with a live run: start on an empty dir, add a config, and its connector starts; the exit code is 0.
- [x] 3.3 Verify parity: e2e cases in `connectors/modbus/tests/reload_e2e.robot` (run by both `just test-e2e modbus` and `just test-e2e-c modbus`) that never send SIGHUP. A config renamed into the directory brings its service `up` and polls its device within 15 s; a file saved with invalid TOML leaves the connector up and polling; a removed file reports the service `down`. The existing SIGHUP cases of that suite and the multi-device suite (whose management commands make the service write its own configs) must still pass with watching on.

## 4. Documentation and packaging

- [x] 4.1 Update README "Install" (reload paragraph: changes apply on their own; SIGHUP still works; the variable), the packaged default configs' "reload the service" hints (`packaging/config/*.toml`) and the `packaging/tedge-dot.service` comments. Verify the docs match the e2e behaviour from 3.3.
- [x] 4.2 Where the contract describes reloads (`doc/contract/ot-connector-contract.md`, §6.3 and the device switch-off note), add that a detected file change triggers the same reload as SIGHUP, and add a release-notes entry (`packaging/release-notes.md`) including the opt-out. Verify `openspec validate config-file-watch-reload --strict` passes.
