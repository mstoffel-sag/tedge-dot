# RFC 0006: OPC UA namespace tables — numeric node ids that survive a renumbering

Status: proposed — implemented alongside this RFC (openspec change `opcua-namespace-table`; both
builds, e2e)

## Problem

A NodeId has two parts: a namespace index and an identifier. The identifier (`i=1234`,
`s=Line1.Temp`) is usually defined by the server's project and stays stable. The index is not:
it is a position in the server's `NamespaceArray` (`i=2255`), and OPC UA guarantees only index 0
(`http://opcfoundation.org/UA/`). A server may number the others differently after a firmware or
project update, and two instances of one device type may number them differently from the
start. `ns=3;i=1234` then names another node, or none, and a point that reads another node
publishes plausible but wrong values.

Point libraries ([RFC 0004](0004-point-libraries.md)) make this worse: a library describes a
device **type** and is shared by every instance of it, so an index-based library is only correct
for the instances it happened to be written against.

Engineers copy node ids, index included, from UaExpert or from older configurations, and want to
keep doing that.

## What OPC UA says

OPC 10000-5 §6.3.1 (ServerType):

- "The Server shall not modify or delete entries of the namespace table while any client has an
  open session to the Server." A server may add entries while clients are connected.
- Servers are recommended not to change indexes, but "it might not always be possible". Indexes
  can therefore change between sessions, typically across a server restart.
- "Clients that cache NamespaceIndexes of NodeIds should always check when starting a session."
- There is no event for a changed namespace table. The optional `UrisVersion` property is
  increased on every change. Model change events report changed nodes, not namespaces. A client
  that wants to know can monitor `NamespaceArray` like any other variable.

## Decision

The ids stay numeric. The configuration records which namespace each index meant when the ids
were written, a **namespace table**, and the connector translates through it to the server's
current index.

```toml
[[device]]
name             = "plc-7"
protocol_address = { endpoint = "opc.tcp://plc:4840/", namespaces = [
  "http://opcfoundation.org/UA/", "urn:plc:server", "urn:acme:plc" ] }

  [[device.point]]
  id      = "boiler_temp"
  address = { node_id = "ns=2;i=1234" }
```

1. **The table is optional, and a full copy of the array.** It lists the `NamespaceArray` in
   index order, as UaExpert shows it, so it can be pasted without renumbering. Entry 0 must be the
   OPC UA namespace, which catches a table that was trimmed or shifted by one. Without a table
   nothing changes.
2. **A device and a library can each carry one.** `protocol_address.namespaces` describes the
   device's own addresses. `[library] namespaces` describes the library's, so a library carries
   what its ids mean to every instance it is used on. The loader does not interpret the key. Each
   resolved point remembers the `[library]` table its address came from (contract §3.4,
   "Library address settings"), and an inline address always counts as the device's own.
3. **Translate once per session.** After every session activation the connector reads
   `NamespaceArray` once and translates each point that has a table: index → URI → the server's
   index. That is exactly the check §6.3.1 asks of clients. Since entries cannot change during a
   session, no monitoring is needed.
4. **Failures are per point.** An unknown URI or an unreadable array gives bad samples for the
   points concerned only. They are tried again, which also picks up a namespace the server adds
   later.
5. **Output shows the address as configured.** A namespace that moved is logged once per session:
   `plc-7: namespace urn:acme:plc local 2 -> server 3`.

The normative rules are in the [OPC UA connector spec §3.9](../connectors/opcua-connector-spec.md).

## Alternatives considered

- **Address by namespace URI** (`nsu=urn:acme:plc;i=1234`, or `namespace_uri = "..."`), the
  first draft of this RFC. It is equally correct, but every existing configuration has to be
  rewritten, which in turn needs a migration tool that connects to the reference server. The
  table keeps the ids engineers already have, and the rewrite becomes one pasted array.
- **Monitor `NamespaceArray` during the session.** It is unnecessary under §6.3.1. The one
  change allowed during a session, an added namespace, is covered by retrying untranslated
  points.
- **Browse paths** (`TranslateBrowsePathsToNodeIds`). These also resist reassigned identifiers,
  but they are slower, more verbose and ambiguous with duplicate names. They could be added later
  as an address form, and this RFC does not preclude it.
- **Producing tables and node ids from a server** is the job of the planned address and
  namespace discovery tool, outside the `tedge-dot` executable. The table format here is what
  that tool is expected to emit.

## Implementation notes

- **Rust** (`async-opcua`): `connector-opcua/src/namespaces.rs` translates. The session keeps the
  translated node ids, and the array is handed on to structure resolution so that it is not read
  twice. Untranslated subscribed points have no monitored item; a retry task adds it.
- **C** (`open62541`): the same rules apply. open62541 1.5 rewrites every node id it encodes from
  the client's own namespace index to the server's, using a table built when the session is
  created. The C build therefore translates to the client's index for the URI, and renews the
  session when the server adds a namespace after it.
- **Tests**: unit tests and an in-process server in Rust, configuration errors in C, and
  `connectors/opcua/tests/opcua_namespace_e2e.robot` against both builds: two simulators with
  different namespace orders, a restart that renumbers, a namespace added while connected, and
  a library shared by both. The interop suite against UA-.NETStandard addresses everything
  through a table.

## Compatibility

The change is additive. An older package ignores a device's `namespaces` and reads the
configured index. It rejects a library carrying `[library] namespaces` as an unknown key.
