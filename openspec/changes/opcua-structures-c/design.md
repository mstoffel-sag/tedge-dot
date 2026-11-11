## Context

`opcua-custom-datatypes` (D3, D10) built structured values in Rust only: a Part 6 body walker
driven by the server's `DataTypeDefinition` (`structure.rs`), session-scoped resolution
(`resolve.rs`) and per-sample decoding in `lib.rs`. The vectors in
`doc/contract/test-vectors/opcua-struct/` pin values, `raw` bytes and messages. The C connector
(`connector_opcua.c`, open62541 v1.5.5) ignores `field` and `index`.

Constraints specific to C:

- the sample model is fixed-size: a 255-byte string value and a 256-byte `raw`;
- open62541 maps namespace indexes: every NodeId it decodes (a DataType attribute, a definition's
  field types and encoding id, an ExtensionObject's type id) is in the **client's** index space,
  while NodeIds inside an undecoded ExtensionObject body are in the **server's**;
- reads of one device run on one thread at a time, possibly a worker thread, and different
  devices concurrently; the runtime once lost commands to process-global state (`strtok`).

## Goals / Non-Goals

**Goals:** the behaviour of the `opcua-structured-values` requirements in the C build, every
vector green in a C unit test, a fuzz target for the walker, and the `requires:opcua-structures`
e2e cases green on the C build.

**Non-Goals:** writes to fields or elements, whole or runtime-length arrays, matrices, 1.03
servers, `UA_Client_getRemoteDataTypes` (D3 of the original change), a changed sample model.

## Decisions

### C1: A pure walker in its own file, a port of `structure.rs`

`connectors/opcua/structure.{c,h}` holds the type set, path parser, plan compiler, body walker and
renderer. It has no open62541 dependency, allocates nothing while decoding, and keeps no static
state, so it is safe on concurrent connector threads and can be fuzzed alone. Names, the order
of checks and every message follow `structure.rs` one to one, so that the shared vectors pass
without C-specific expectations. The C unit test reads `vectors.json` with cJSON.

Alternative: open62541's own decoder with types built from the definitions
(`UA_Client_getRemoteDataTypes`). Rejected for the reasons of the original D3: padded C structs,
and the plan/skip design needs no type memory at all.

### C2: Resolution through open62541 reads, cached per device and session

For each field point (and the DataType of each raw point) the connector reads the variable's
`DataType`, then for each structure type one Read of `BrowseName` and `DataTypeDefinition`.
open62541 decodes the attribute into a `UA_StructureDefinition` or `UA_EnumDefinition` (both ns0
types), so one Read covers structures and Enumerations. `UA_Client_readDatatypeDefinitionAttribute`
only accepts a structure, so the generic Read is used. Simple subtypes follow `HasSubtype`
inverse browses (at most 8 hops), abstract `Structure`/`BaseDataType` variables resolve through
the value's encoding id and its inverse `HasEncoding` reference, as in Rust.

The resolved types live in the device state, keyed by DataType NodeId, with failures cached by
key. They are freed on disconnect, so a reconnect re-resolves. Resolution runs lazily: before a
polled point's first read in a session, and in `subscribe_device`/`retry_pending` for a pushed
point (never from inside an open62541 callback, which cannot issue synchronous services).
Failures are point-level bad samples starting `data type definition unavailable`, and the link
is not degraded.

### C3: Two namespace tables

The walker renders NodeIds with a namespace table. A structure body uses the **server's**
`NamespaceArray` (read once per session). A NodeId open62541 decoded (the `addr.encoding_id`,
`addr.data_type`, a top-level NodeId value re-encoded to its binary form) uses the **client's**
table (`UA_Client_getNamespaceUri`), where those indexes live. Both render as `nsu=<uri>;`.

### C4: Top-level built-in types through the walker

A top-level value of a type without an SDK primitive (DateTime, LocalizedText, StatusCode, Guid,
NodeId, ExpandedNodeId, QualifiedName) is encoded with `UA_encodeBinary` and decoded by
`uas_decode_builtin`, as Rust re-encodes and calls `decode_builtin`. ByteString keeps its existing
path. An ExtensionObject that open62541 decoded (a known ns0 type) is re-encoded to get its body.

### C5: Fixed-size values

- A String or LocalizedText value longer than 255 bytes is cut at a UTF-8 boundary, as top-level
  strings already are in C (parity table); its `raw` holds at most 256 bytes.
- A `bytes` value holds at most 127 bytes (the existing top-level rule); a longer one is a bad
  sample, never truncated.
- A raw point's body longer than 256 bytes is a bad sample:
  `structure body of N bytes is longer than a raw sample holds in the C build (256 bytes)`.
- The `error` field holds 159 bytes; long resolution messages are cut there.

### C6: Shared reads by node and index

`tdot_ua_read_blocks` (and the in-flight variant) give every distinct (node, index) one
ReadValueId per Read and hand its result to each point that asked for it. A point with an index
sends `IndexRange "n"`; the result is unwrapped as in Rust (one element, or the element at `n`
when a server ignores the range). Monitored items stay one per point: the C ring hands each
notification to one point, and the spec's one-item-per-node rule is an efficiency goal that the
polling path meets; this is recorded as a remaining difference in the parity table.

### C7: Per-sample `addr` for raw points

A raw structure sample's `addr` adds `encoding_id` and `data_type`. It is rendered into a buffer in
the point's state and passed as `sample.addr_json`, which only needs to outlive the call. Field
and index points carry `addr.field`/`addr.index` in their static `addr_json`.

### C8: Fuzzing

`tests/fuzz_opcua_struct.c` builds a libFuzzer target when the compiler supports
`-fsanitize=fuzzer` (`-DTDOT_FUZZ=ON`, clang). It compiles every vector plan and decodes the input
with each, plus every built-in type. Without libFuzzer, the same file builds a deterministic driver
that feeds random and mutated vector bodies and runs as a ctest under the default build.

## Risks / Trade-offs

- [Two implementations drift] → both run the same vectors; messages are compared exactly.
- [Client vs server namespace indexes confused] → C3, and the e2e raw case checks `nsu=` forms.
- [Large bodies on a 256-byte raw buffer] → bad sample naming the limit, never truncation.
- [Resolution latency on the first poll after a connect] → a handful of Reads per type, once.

## Migration Plan

Additive. A C configuration with `field`/`index` points switches from bad samples to values.
`opcua-custom-datatypes` should be archived before this change, which modifies its capability.
