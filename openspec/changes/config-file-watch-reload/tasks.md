# Tasks

## 1. Shared reload trigger (Rust)

- [ ] 1.1 In `impl/rust/src/main.rs`, move the body of the SIGHUP branch of `run()` (rediscover, warn about duplicates, `reconcile`, "reload failed" error) into one function that both triggers call. Verify the existing reload tests still pass (`cargo test -p tedge-dot`).
- [ ] 1.2 Return the resolved point-library paths from the Rust config loader (`library.rs`) together with the config. Verify with a unit test: a config with `points_from = ["demo-sim"]` reports the resolved file under the search path.

## 2. Watcher (Rust)

- [ ] 2.1 Parse `TEDGE_DOT_CONFIG_WATCH_INTERVAL` (duration grammar of the configs, default `2s`, `0` = off, invalid → warning + default, minimum 100 ms). Verify with unit tests for each case.
- [ ] 2.2 Build the fingerprint of the watched set (config dirs' `*.toml` listing, named config files, resolved library files: existence, size, mtime, inode), and rebuild the set after every reload. Verify with unit tests: an added, modified, removed or renamed-over file changes the fingerprint; a `.txt` file or an unreferenced library does not.
- [ ] 2.3 Add the poll loop to `run()`'s `select!` with the settle rule (changed, then unchanged for one interval → one reload, logged at info with the changed paths). Verify with a test that runs `run` against a temp config dir: a new config file starts its connector, several writes within an interval cause exactly one reload, a write made while a reload is running causes one more reload that applies it (baseline taken before the reload), and `TEDGE_DOT_CONFIG_WATCH_INTERVAL=0` causes none.
- [ ] 2.4 Verify that the service's own writes are no-ops: a `define-device` command persists the config, and the following detected reload logs "reload: … is unchanged" without reconnecting (extend the existing management-command test).

## 3. Watcher (C)

- [ ] 3.1 Add the same interval parsing and fingerprint in `impl/c/sdk/src/runtime.c`, using the loader's `lib_paths` for libraries. Verify with C unit tests in `impl/c/tests/config.c` that mirror 2.1 and 2.2.
- [ ] 3.2 Run the poll loop on a thread that bumps `g_reload_gen` like `on_hangup`, with the same settle rule, baseline (taken before the reload) and log line, and rebuild the watched set after each reload. Verify with a C test against a temp config dir, mirroring 2.3, including the write during a running reload.
- [ ] 3.3 Verify parity: add an e2e case (shared by `just test-e2e` and `just test-e2e-c`) that copies a second config into the connector's config dir and expects its connector's health `up` within 10 s, without SIGHUP, and another that saves invalid TOML and expects the running connector unaffected.

## 4. Documentation and packaging

- [ ] 4.1 Update README "Install" (reload paragraph: changes apply on their own; SIGHUP still works; the variable), the packaged default configs' "reload the service" hints (`packaging/config/*.toml`) and the `packaging/tedge-dot.service` comments. Verify the docs match the e2e behaviour from 3.3.
- [ ] 4.2 Where the contract describes reloads (`doc/contract/ot-connector-contract.md`, §6.3 and the device switch-off note), add that a detected file change triggers the same reload as SIGHUP, and add a release-notes entry (`packaging/release-notes.md`) including the opt-out. Verify `openspec validate config-file-watch-reload --strict` passes.
