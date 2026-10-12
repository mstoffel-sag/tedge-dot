# Proposal

## Why

OPC UA points are configured with numeric node ids such as `ns=3;i=1234`. The `3` is only a
position in the server's `NamespaceArray`, and OPC UA guarantees nothing but index 0. A firmware
update, a project change, or a different load order on another instance of the same device type
can renumber the array, and the id then silently names a different node: the connector publishes
plausible but wrong values. Point libraries make this worse, because one library describes every
instance of a device type, and instances may order their namespaces differently.

Engineers copy node ids from UaExpert or older configurations, and want to keep doing that. So
the ids stay as they are, and the file records which namespace each index meant when the ids were
taken: a **local namespace table**. The connector translates through it to the server's current
index.

This replaces the URI-addressing proposal of draft RFC 000X (`nsu=` addresses, `migrate`,
in-session discovery). Those changes are dropped.

## What Changes

- A device (in `protocol_address`) and a point library (in `[library]`) may carry an optional
  `namespaces` array, a copy of the server's `NamespaceArray` from when the ids were written:
  ```toml
  protocol_address = { endpoint = "opc.tcp://plc:4840/",
                       namespaces = ["http://opcfoundation.org/UA/", "urn:plc:server", "urn:acme:plc"] }
  ```
- **Without a table, nothing changes**: `ns=3;i=1234` is read as namespace index 3, exactly as
  today, with no extra server request.
- **With a table**, the connector reads the server's `NamespaceArray` after every session
  activation, maps each point's local index to its URI through the table and the URI to the
  server's current index, and reads, writes and subscribes the node there. `ns=0` is never
  mapped. Point ids in the file stay numeric and unchanged.
- Points from a library use the library's table; points defined on the device use the device's.
  A library without a table falls back to the device's.
- Validation: entry 0 must be `http://opcfoundation.org/UA/`; a point whose index is outside its
  table is a configuration error.
- Failures are per point: a URI missing on the server gives bad samples
  `namespace not found: <uri>`; an unreadable `NamespaceArray` gives bad samples
  `namespace array unavailable: <status>` for the mapped points only. Other points keep
  publishing, the link status is unchanged, and an unresolved point is retried on each poll
  cycle, which also picks up a namespace the server adds later.
- Samples and `describe` show the address as configured. When a namespace sits at a different
  index on the server than in the table, the connector logs it once per session at info level,
  e.g. `plc-7: namespace urn:acme:plc local 3 -> server 4`. No CLI output changes.
- Both builds (Rust and C) behave identically.

Not in this change: reading or browsing a server to produce node ids or the namespace table.
That belongs to the planned address and namespace discovery tool, outside the `tedge-dot`
executable; the table format here is what that tool is expected to emit.

## Capabilities

### New Capabilities
- `opcua-namespace-table`: the optional local namespace table of an OPC UA device or point
  library: its syntax and validation, which table applies to which point, translation to the
  server's current index per session, per-point failures, and how addresses appear in output.

### Modified Capabilities
<!-- none: OPC UA addressing is documented in the connector spec, not yet in an openspec spec -->

## Impact

- Rust: `connector-opcua` (`config.rs` reads the table, the connect path reads `NamespaceArray`
  and builds a per-session node table next to `resolve_types`), SDK `library.rs` (`namespaces`
  allowed in `[library]` and carried to the expanded points).
- C: `connector_opcua.c` (same), `impl/c/sdk/src/config.c` (library key).
- Docs: OPC UA connector spec §3.2/§3.4, RFC 0004 and the library template, README, release
  notes. RFC 000X is rewritten to this design.
- Tests: a simulator that renumbers its namespaces across a restart, one library used by two
  instances with different namespace order, missing namespace, interop against the
  UA-.NETStandard reference server (which replaces `connectors/opcua/interop/resolve-ns.py`).
- Users: purely additive. Existing configurations without a table behave exactly as before.
