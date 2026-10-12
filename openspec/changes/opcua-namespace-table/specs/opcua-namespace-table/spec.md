# Spec Delta

## Purpose

Defines the optional local namespace table of an OPC UA device or point library, which records
the namespace URI behind each index used in the configured node ids, so that the connector reads
the intended node even when the server numbers its namespaces differently.

## ADDED Requirements

### Requirement: Optional namespace table
A device SHALL accept `protocol_address.namespaces` and a point library SHALL accept `[library] namespaces`: an array of namespace URI strings in index order, as the server listed them when the node ids were written. Entry 0 SHALL be `http://opcfoundation.org/UA/`; otherwise loading SHALL fail with an error naming the device or library.

#### Scenario: Device with a table
- **WHEN** a device has `protocol_address = { endpoint = "opc.tcp://plc:4840/", namespaces = ["http://opcfoundation.org/UA/", "urn:plc:server", "urn:acme:plc"] }`
- **THEN** the configuration loads and the device's points are translated through the table

#### Scenario: Library with a table
- **WHEN** a point library has `[library]` with `protocol = "opcua"` and `namespaces = [...]`
- **THEN** the library loads and its points are translated through its table

#### Scenario: Wrong entry 0
- **WHEN** `namespaces = ["urn:acme:plc"]`
- **THEN** loading fails with an error saying entry 0 must be `http://opcfoundation.org/UA/`

#### Scenario: Not an array of strings
- **WHEN** `namespaces = "urn:acme:plc"` or contains a number
- **THEN** loading fails with an error naming the key

### Requirement: No table, no change
A point without an applicable table SHALL be read, written and subscribed with its configured namespace index exactly as before this change, and the connector SHALL NOT read the server's `NamespaceArray` for it.

#### Scenario: Existing configuration
- **WHEN** a configuration written before this change uses `ns=3;i=1234` and has no `namespaces`
- **THEN** the point reads namespace index 3 on the server and no `NamespaceArray` read is issued for the device

### Requirement: Table applies to a point
A point defined on the device SHALL use the device's table. A point expanded from a point library SHALL use the library's table, or the device's table when the library has none. Each point SHALL use exactly one table or none.

#### Scenario: Library and device tables differ
- **WHEN** the library's table lists `urn:acme:plc` at index 3 and the device's table lists it at index 4, and the library point has `ns=3;i=1234`
- **THEN** the library point resolves through the library's table to `urn:acme:plc`

#### Scenario: Library without a table
- **WHEN** a library has no `namespaces` and the device has one
- **THEN** the library's points are translated through the device's table

### Requirement: Index must be in the table
For a point that uses a table, a namespace index greater than or equal to the table's length SHALL be a configuration error naming the point and the index. Namespace index 0 SHALL always mean index 0 and SHALL never be translated.

#### Scenario: Index outside the table
- **WHEN** a point has `ns=5;i=1` and its table has three entries
- **THEN** loading fails with `point '<id>': namespace index 5 is not in the namespace table (3 entries)`

#### Scenario: Standard node
- **WHEN** a point with a table has `i=2258` or `ns=0;i=2258`
- **THEN** it reads `i=2258` on the server, also when the server's `NamespaceArray` cannot be read

### Requirement: Translation at every session activation
After every successful session activation (first connect and every reconnect), before the device's first Read, Write or CreateMonitoredItems, the connector SHALL read the server's `NamespaceArray` once when the device has at least one point using a table, and translate each such point: local index → URI from its table → index of that URI on the server (exact, case-sensitive match; first occurrence). The point SHALL then be read, written and subscribed at the translated index. The configured identifier SHALL be used unchanged.

#### Scenario: Server renumbered across a restart
- **WHEN** the table lists `urn:acme:plc` at index 3, the server restarts with `urn:acme:plc` at index 4, and the connector reconnects
- **THEN** the point configured as `ns=3;i=1234` reads `ns=4;i=1234` and keeps publishing the same value as before the restart

#### Scenario: Same order as the table
- **WHEN** the server lists the namespaces in the table's order
- **THEN** every point reads at its configured index

#### Scenario: One library on two instances
- **WHEN** two devices use the same library with a table, and their servers list `urn:acme:plc` at indexes 3 and 5
- **THEN** both devices read the correct node

#### Scenario: Write and subscribe
- **WHEN** a mapped point is written by `tedge-dot write` or a parameter write, or is subscribed
- **THEN** the write and the monitored item use the translated node id, and two points that translate to the same node still share one monitored item

### Requirement: Translation failures are per point
A translation failure SHALL affect only the points it concerns: they publish bad samples, are not written to and get no monitored item. Other points SHALL keep publishing. A failure SHALL NOT fail the connect or the subscription; for the link status a polled untranslated point counts like any other bad point, and a subscribed one's bad samples do not count, as for any pushed sample.

#### Scenario: Namespace not on the server
- **WHEN** a point's URI from the table is not in the server's `NamespaceArray`
- **THEN** the point publishes bad samples `namespace not found: <uri>`, other points publish normally, and the device stays `connected`

#### Scenario: NamespaceArray cannot be read
- **WHEN** reading the server's `NamespaceArray` fails with a status
- **THEN** every point that uses a table publishes bad samples `namespace array unavailable: <status>`, and points without a table and namespace-0 points are unaffected

### Requirement: Unresolved points are retried
While at least one point of a session is untranslated, the connector SHALL re-read the server's `NamespaceArray` before each poll cycle of the device (every 30 s for a device whose points are all subscribed) and translate the untranslated points again. Points already translated in the session SHALL keep their translation until the next session activation.

#### Scenario: Namespace added at runtime
- **WHEN** a point's URI is missing at connect and the server adds it to its `NamespaceArray` later in the same session
- **THEN** the point is translated and publishes good samples: a polled point within a few poll cycles, a subscribed one within the retry interval

#### Scenario: Array becomes readable
- **WHEN** the `NamespaceArray` read failed at connect and succeeds later
- **THEN** the affected points recover without a reconnect

### Requirement: Addresses shown as configured
Published samples, status messages, `describe` and the output of `tedge-dot read` (including `--json`) SHALL show a point's address as configured, never the translated index.

#### Scenario: Sample after renumbering
- **WHEN** a mapped point publishes before and after its namespace moved on the server
- **THEN** the sample's `addr` is identical in both

#### Scenario: read --json unchanged
- **WHEN** `tedge-dot read -c opcua.toml -d plc-7 --json` reads a point configured as `ns=3;i=1234` whose namespace is at index 4 on the server
- **THEN** it prints the same sample envelope the service publishes, with `addr` as configured and no additional keys

### Requirement: Translation is logged when it moves a namespace
When a translation maps a table index to a different server index, the connector SHALL log one info line per such namespace and session: `<device>: namespace <uri> local <n> -> server <m>`. Namespaces at the same index as in the table SHALL NOT be logged. A namespace translated later by a retry SHALL be logged when it is translated.

#### Scenario: Namespace moved
- **WHEN** device `plc-7` connects and `urn:acme:plc` is at index 3 in the table and at index 4 on the server
- **THEN** one info line `plc-7: namespace urn:acme:plc local 3 -> server 4` is logged for that session, however many points use the namespace

#### Scenario: Nothing moved
- **WHEN** the server lists every table namespace at its table index
- **THEN** no translation line is logged

#### Scenario: Same line in both builds
- **WHEN** the renumbering e2e case runs against the Rust and the C build
- **THEN** both log the identical line

### Requirement: Both builds behave identically
The Rust and the C build SHALL accept the same tables with the same errors, translate at the same point in the session lifecycle, and publish the same bad-sample reasons.

#### Scenario: Parity
- **WHEN** `just c-describe-parity` runs over configurations with device and library tables and with each table error, and the namespace e2e suite runs against both builds
- **THEN** outputs, errors and sample reasons are identical
