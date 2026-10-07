# Proposal

## Why

The service applies configuration changes only on SIGHUP (`systemctl reload tedge-dot`). A file
written by anything else stays on disk while the old configuration keeps running, and nothing
reports it. That includes thin-edge.io's configuration management (`c8y_DownloadConfigFile`), a
package or provisioning tool, or a user editing over SSH. thin-edge.io does not signal services
after it writes a configuration file. So delivering a connector config from Cumulocity currently
needs a custom hook in the `config_update` workflow that sends the signal. The OPC-UA solution
blueprint ships exactly that workaround. The reload logic itself already exists and is safe:
unchanged connectors keep running, changed ones apply in place, and an unusable file is logged
while its connector keeps the configuration it has. What's missing is noticing the change.

## What Changes

- The `run` service detects changes to the files a reload would read, and runs the existing reload
  for them:
  - a config file added, changed or removed in a watched config directory
  - a config file given by path
  - a point library a running config references
- A change is acted on once it has stopped changing (debounced), so a file that is still being
  written is not read half-done.
- On by default, with a poll interval from `TEDGE_DOT_CONFIG_WATCH_INTERVAL` (default `2s`). `0`
  turns it off, and then only SIGHUP reloads, as today.
- SIGHUP and `systemctl reload` keep working unchanged.
- Both implementations (Rust and C) behave the same.

## Capabilities

### New Capabilities
- `config-reload`: when and how the running service applies changed connector configuration. It
  covers both triggers (SIGHUP and detected file changes), what counts as a change, debouncing,
  the interaction with the service's own writes (management commands persisting a config), and
  how to turn watching off.

### Modified Capabilities
<!-- none: reload behaviour is documented in the README and contract, not yet in a spec -->

## Impact

- Rust: `impl/rust/src/main.rs` (`run`: a watcher feeding the same reload path as SIGHUP).
- C: `impl/c/sdk/src/runtime.c` / `impl/c/src/main.c` (a watcher bumping the same reload generation
  as `on_hangup`).
- No new dependencies: the change is detected by polling file metadata.
- Docs: README "Install" section, `packaging/tedge-dot.service` comments, the packaged default
  configs' "reload the service" hints, and the contract's reload notes.
- Users: an edited config applies within a few seconds without `systemctl reload`. Someone who
  edits a config in several saves gets a reload per settled save, the same as running reload
  after each.
