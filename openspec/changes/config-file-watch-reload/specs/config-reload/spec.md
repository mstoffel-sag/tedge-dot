# Spec Delta

## Purpose

Defines when the long-running `tedge-dot run` service applies changed connector configuration:
on SIGHUP, and on its own when the files it reads change, so a configuration delivered by any
means takes effect without a manual reload.

## ADDED Requirements

### Requirement: SIGHUP reloads
On SIGHUP, the service SHALL rediscover its configuration files and reload them. A new file starts a connector, a removed file stops its connector, and every other connector re-reads its file and applies what changed. This is the existing behaviour, kept unchanged.

#### Scenario: systemctl reload
- **WHEN** `systemctl reload tedge-dot` is run after a config file was edited
- **THEN** the edited connector applies the new configuration and the others keep running untouched

### Requirement: File changes trigger the same reload
While watching is enabled, the service SHALL perform the SIGHUP reload on its own when a watched file changes. Watched files: every `*.toml` in a config directory named on the command line (added, changed or removed), every config file named on the command line, and every point library file a running configuration references.

#### Scenario: Config delivered by configuration management
- **WHEN** thin-edge.io's configuration management writes `/etc/tedge/plugins/ot/opcua-pump.toml`, a new file
- **THEN** within the watch interval plus the settle time, a connector for it starts, without SIGHUP

#### Scenario: Config edited
- **WHEN** a running connector's config file is changed and saved
- **THEN** that connector applies the change as on SIGHUP, and the other connectors keep running untouched

#### Scenario: Config removed
- **WHEN** a config file is deleted from the config directory
- **THEN** its connector stops

#### Scenario: Point library edited
- **WHEN** a point library referenced by a running configuration is changed
- **THEN** the connectors using it reload it

#### Scenario: Unrelated file
- **WHEN** a file not ending in `.toml` is written to the config directory, or a library no running configuration references is changed
- **THEN** no reload happens

### Requirement: A change is applied once it has settled
The service SHALL NOT reload while a watched file is still changing. A change triggers a reload only once the watched files have been unchanged for one full watch interval. Changes in the same settle window cause one reload.

#### Scenario: File written in several steps
- **WHEN** a file is written in several writes that follow each other within the watch interval
- **THEN** exactly one reload happens, after the last write

#### Scenario: Several files changed together
- **WHEN** three config files are replaced within one watch interval
- **THEN** one reload applies all three

### Requirement: A change made during a reload is not lost
A file that changes while a reload is in progress SHALL be reloaded again afterwards. Whether a later change counts as new SHALL be judged against the state of the watched files when the running reload was triggered, not when it finished. Files watched for the first time after a reload SHALL count as changed, which may cause one extra reload that applies nothing.

#### Scenario: Edit while connectors are reloading
- **WHEN** a config file is saved again while the reload caused by its previous save is still running
- **THEN** once the second save has settled, another reload runs and the connector ends up with the second version

#### Scenario: Newly referenced library
- **WHEN** a reload applies a config that now references a point library that was not watched before
- **THEN** that library is watched from then on, and at most one further reload follows, which finds every config unchanged

### Requirement: An unusable file does not disturb running connectors
A reload caused by a file change SHALL handle an unusable file exactly as a SIGHUP reload does: the error is logged and that connector keeps the configuration it has. The file SHALL be read again on its next change.

#### Scenario: Syntax error saved
- **WHEN** a config file is saved with invalid TOML
- **THEN** an error naming the file is logged, its connector keeps running with the previous configuration, and nothing else is affected
- **AND** when the file is saved again in a valid form, the connector applies it

### Requirement: The service's own writes do not restart connectors
When the service itself writes a config file, for example a management command (`set-config`, `define-device`, `remove-device`) persisting the configuration it has applied, the detected change SHALL NOT reconnect devices or restart connectors. The configuration on disk equals the one running.

#### Scenario: Management command persists a config
- **WHEN** a `define-device` command is applied and the service writes the updated config file
- **THEN** the resulting reload changes nothing: no device reconnects and the connector's service health stays up

### Requirement: Watching can be turned off
The watch interval SHALL come from `TEDGE_DOT_CONFIG_WATCH_INTERVAL`, a duration such as `2s` (the default) or `500ms`. `0` SHALL turn watching off, leaving SIGHUP as the only trigger. An invalid value SHALL be logged and the default used. The `run` flag `--no-watch` SHALL turn watching off regardless of the variable, and the service SHALL log that it is off.

#### Scenario: Disabled
- **WHEN** the service runs with `TEDGE_DOT_CONFIG_WATCH_INTERVAL=0` and a config file is changed
- **THEN** nothing is reloaded until SIGHUP

#### Scenario: Turned off with --no-watch
- **WHEN** the service runs as `tedge-dot run /etc/tedge/plugins/ot --no-watch`, with `TEDGE_DOT_CONFIG_WATCH_INTERVAL=500ms` set, and a config file is added
- **THEN** no connector starts for it until SIGHUP, and the log says watching is off

#### Scenario: Invalid value
- **WHEN** `TEDGE_DOT_CONFIG_WATCH_INTERVAL=often` is set
- **THEN** a warning is logged and changes are detected at the default interval

### Requirement: A detected reload is logged
Every reload caused by a file change SHALL be logged at info level, naming the changed files, so a reload in the log can be traced to its cause.

#### Scenario: Log line
- **WHEN** `opcua-pump.toml` changes and is reloaded
- **THEN** the log has an info line naming `opcua-pump.toml` as changed, followed by the usual reload messages

### Requirement: Both implementations behave the same
The Rust and the C build SHALL detect the same changes, with the same settle rule, environment variable and default. A configuration directory changed in the same way SHALL lead to the same connectors running in both.

#### Scenario: Same edit, both builds
- **WHEN** the same config file is added to the config directory of a Rust and of a C service
- **THEN** both start a connector for it within the watch interval plus the settle time
