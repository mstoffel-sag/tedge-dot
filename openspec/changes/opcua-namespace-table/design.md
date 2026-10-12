# Design

## Context

- See proposal.md (Why). Supersedes the URI-addressing design of draft RFC 000X.
- **What OPC UA allows** (OPC 10000-5 §6.3.1, ServerType):
  - "The Server shall not modify or delete entries of the namespace table while any client has
    an open session to the Server." Adding entries is allowed while clients are connected.
  - Servers are recommended not to change indexes, "nevertheless, it might not always be
    possible". So indexes can change **between** sessions, typically across a server restart.
  - "Clients that cache NamespaceIndexes of NodeIds should always check when starting a
    session to verify that the cached NamespaceIndexes have not changed."
  - There is **no dedicated event** for a namespace table change. The optional `UrisVersion`
    property (VersionTime) is increased on every change of `NamespaceArray` or `ServerArray`.
    Model change events (`GeneralModelChangeEventType`) report changed nodes, not namespace
    changes; an OPC Foundation proposal for a namespace event was closed without change. A client
    that wants to know can monitor `NamespaceArray` (`i=2255`) as an ordinary variable.
- Rust: `NodeAddress` → `NodeId` is built once at configure time (`node_id_from`, `lib.rs`). The
  connect path already reads `i=2255` in `resolve_types` (`resolve.rs`) for structure/raw points.
- C: `ua_point_t.node_id[160]` holds the text id; `read_point`, `write_point`,
  `subscribe_device` call `UA_NodeId_parse` on it. Nothing is read after activation.
- Libraries are expanded by the SDK loader (`library.rs`, `config.c`) into the device's point list;
  `[library]` accepts only `protocol`, `type`, `description`, `version`.

## Goals / Non-Goals

**Goals:**
- Configured ids stay numeric and copyable from UaExpert; the table is the only addition.
- No behaviour change and no extra request for configurations without a table.
- Correct after any renumbering that OPC UA permits.

**Non-Goals:**
- `nsu=` / `namespace_uri` address forms and a `migrate` command (dropped with RFC 000X's design).
- Monitoring `NamespaceArray` during a session. Per §6.3.1 existing entries cannot change while
  the session is open, so a translation made at activation stays valid for the session; the
  only runtime change, an added namespace, is covered by the retry (D5).
- Using `UrisVersion`: optional on servers, and unnecessary given D3/D5.
- Reading or browsing a server to produce the table or node ids: that belongs to the separate
  address and namespace discovery tool, not the `tedge-dot` executable. The table format (D1)
  is what that tool is expected to emit.

## Decisions

### D1: The table is a full copy of the server's array
`namespaces` lists every entry in index order, including 0 and 1, exactly as UaExpert's namespace
view or the planned discovery tool shows it.
- *Why:* the ids in the file are positions in that array; a full copy can be pasted without
  re-numbering, and checking entry 0 catches tables that were trimmed or shifted by one.
- *Alternative:* a sparse map `{ 3 = "urn:acme:plc" }`. Shorter, but a second shape to document
  and parse in two builds, and easier to get the key wrong.

### D2: Where the table lives and how it reaches the point
- Device: `protocol_address.namespaces`. `protocol_address` is OPC UA-specific and free-form for
  the SDK, so no SDK key list changes; the table describes what this device's ids were written
  against.
- Library: `[library] namespaces`. The SDK accepts the key in `LIBRARY_KEYS` (Rust and C) for
  any protocol but does not interpret it. *Found in implementation:* instead of copying one
  protocol's key around, the loader records on every library point that has an `address` the
  `[library]` table that address came from — Rust `PointConfig::library` (written into the
  expanded document as the reserved point key `__library`, which a user's point is refused for
  as an unknown key), C `tdot_point_t.library` (borrowed from the library document). A
  definition that replaces the address drops it, since an inline address is the device's own.
  Protocol-neutral, so a future protocol can describe its library addresses the same way.
  Samples never show it: both modules build `addr` from the configured node id.
- The OPC UA module picks per point: the address's library table → device table → none (spec requirement
  "Table applies to a point").
- *Alternative:* `[connection] namespaces` as a default for all devices. Rejected: different
  devices of one config are different servers.

### D3: Translate once per session, in the connect path
Right after activation, before `resolve_types`, the first poll and `subscribe_device`: if any
point uses a table, read `i=2255` once and build `URI → server index` (first occurrence wins,
byte comparison). Fill a per-session table `point → Result<NodeId, reason>`; points without a
table and `ns=0` points get their configured id. Read, write, subscribe, structure resolution
and shared-node grouping all take node ids from this table. The array is handed to
`resolve_types` so it is not read twice. The per-session table is dropped with the session
(Rust: next to `SessionTypes`; C: cleared in `disconnect_device`), so a reconnect always
re-translates — which is exactly what §6.3.1 asks of clients.
- *Alternative:* translate at configure time. Fails on the restart renumbering this change
  exists for.

### D4: Failures stay per point
`namespace not found: <uri>` and `namespace array unavailable: <status>` are stored in the point's
slot; the point produces bad samples, is refused for writes and gets no monitored item. Link
status is computed as today, ignoring translation.

### D5: Retry only what is untranslated
*Found in implementation:* both runtimes treat a failed `subscribe` as a reason to reconnect, so
an untranslated point must not fail it. A subscribed untranslated point is therefore accepted
for push without a monitored item: it reports through the push path (whose samples do not judge
the link) and a retry adds its item later — Rust: a task per subscription, C: from
`drain_subscriptions`.

While any slot holds a failure, the array is re-read before the device's next poll cycle (or on
a 30 s timer if all its points are subscribed), and only the failed slots are translated again;
newly translated subscribed points get their monitored items then. Translated slots are never
re-translated within a session (entries cannot change during it), so the retry cannot move a
working point.
- *Found in implementation (C):* open62541 1.5 rewrites the namespace index of every node id it
  encodes, from the client's own index to the server's, with a table it builds when a session is
  created (`client.h`, "Namespace Mapping"). The C build therefore translates to the client's
  index for the URI (`UA_Client_getNamespaceIndex`, equal to the server's for every namespace
  listed at session creation), and a namespace the server added later cannot be addressed in
  the running session: the module then reports the link down once, so the runtime renews the
  session, logged as `device <d>: the server added namespace <uri>; renewing the session to
  address it`. async-opcua sends indexes as given, so the Rust build needs no new session.
- *Why:* covers the one change OPC UA allows during a session (an added namespace, e.g. a module
  loaded at runtime) without monitoring `i=2255` on every device.

### D6: Show translations in the log, not in output
Samples, `describe` and `read`/`read --json` stay exactly as configured: `read --json` is meant
to print the envelope the service publishes, so it gets no extra keys. Instead, the translation
step logs one info line per namespace whose server index differs from the table
(`<device>: namespace <uri> local <n> -> server <m>`), at activation and when a retry translates
it. That is visible in the running service's journal as well as in a one-shot `read`.
- *Alternative:* `resolved_node_id` in `read --json`. Rejected: makes `read --json` diverge from
  the MQTT sample and only helps in manual runs.

## Risks / Trade-offs

- [A table copied from a different server than the ids] → translation then maps to the wrong
  URI; the same risk exists today with raw indexes. Docs: take the table from the server the ids came from
  (UaExpert today, the planned discovery tool later).
- [A server breaks §6.3.1 and renumbers during a session] → undetected until reconnect. Accepted:
  non-conformant, and monitoring `i=2255` can be added later without changing the config format.
- [Server lists a URI twice] → first index wins, as `UA_Client_NamespaceGetIndex` does.
- [Extra request per connect] → only for devices that use a table; reused by `resolve_types`.

## Migration Plan

Additive. Rollback to an older package: the Rust OPC UA module does not deny unknown
`protocol_address` keys (`OpcuaEndpoint` has no `deny_unknown_fields`), so a device table is
silently ignored and points fall back to their raw index — today's behaviour. A library with
`[library] namespaces` is refused by an older loader (unknown `[library]` key). Task 1.1 confirms
the C build behaves the same, and the release notes state both.
