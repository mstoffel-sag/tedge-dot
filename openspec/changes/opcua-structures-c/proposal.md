# Proposal

## Why

The change `opcua-custom-datatypes` gave the Rust OPC UA connector structured values, array
elements and more built-in types, and recorded the C build as missing the capability
`opcua-structures` (design D10). The C package is the default package, so a user who needs one
field of a structure must switch to the larger Rust build, which does not run on older
distributions. The language-neutral vectors in `doc/contract/test-vectors/opcua-struct/` already
pin the behaviour, so a C walker can be held to it exactly.

## What Changes

- The C OPC UA connector reads `address.field` points: it resolves each node's DataType and the
  `DataTypeDefinition`s it needs once per session, and decodes the selected field from the
  ExtensionObject body with its own Part 6 walker (same plan/skip design as `structure.rs`).
- `address.index` selects one array element on the server (`IndexRange`), on a scalar array
  variable and combined with `field`.
- `mode = "raw"` points on a structure publish the body as `raw`, with `addr.encoding_id` and
  `addr.data_type` in `nsu=` form. A body longer than the C sample's 256-byte `raw` buffer is a
  bad sample that names the limit; it is never truncated.
- DateTime, LocalizedText, StatusCode, Guid, NodeId, ExpandedNodeId and QualifiedName are read as
  top-level variables, structure fields and array elements, rendered as in Rust.
- Several points on the same node and index share one ReadValueId in a poll's Read.
- Configuration validation gains the Rust messages for `field` paths, `field` with `raw`, and
  `field`/`index` on a writable point.
- The C build no longer lists `opcua-structures` as missing: `C_MISSING_CAPABILITIES`, the
  parity table in `impl/c/README.md`, `TODO.md`, the site's package data, and the docs.
- A C unit test runs every vector; a fuzz target exercises the walker.

Out of scope, as in Rust: writes to fields or elements, whole and runtime-length arrays, matrices,
and 1.03 servers (DataTypeDictionary).

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `opcua-structured-values` (introduced by `opcua-custom-datatypes`, not yet archived): the
  requirement "Test vectors and implementation scope" changes from "Rust only, C declares the
  capability missing" to "both builds implement it and run the vectors", and the raw-mode
  requirement gains the C size limit.

## Impact

- `impl/c/connectors/opcua/`: new `structure.c`/`structure.h` (pure walker, no open62541),
  resolution and decoding in `connector_opcua.c`.
- `impl/c/tests/opcua_struct.c` (vector runner), `impl/c/tests/fuzz_opcua_struct.c` (fuzz
  target), `impl/c/CMakeLists.txt`.
- `justfile` (`C_MISSING_CAPABILITIES`), `impl/c/README.md`, `TODO.md`,
  `doc/connectors/opcua-connector-spec.md`, `doc/contract/test-vectors/opcua-struct/README.md`,
  `site/src/data/packages.yaml`, `connectors/opcua/connector-structures.toml` and
  `demo/points.d/opcua/demo-sim-structures.toml` comments.
- The OPC UA e2e cases tagged `requires:opcua-structures` now run against the C build.
- No change to the capability descriptor: Rust does not advertise structures there either.
