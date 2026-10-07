## 1. Contract, schema and vectors

- [x] 1.1 Amend contract §4: list structure field selection as an allowed decoding refinement next to bit-field extraction (the driver still performs no renaming or shaping, and `value` stays primitive). Amend §5: for a structured value, `raw` is the encoded ExtensionObject body.
- [x] 1.2 ~~Add `field` to the OPC UA address in `config.schema.json`~~. Not applicable: the schema leaves `address` to each connector spec ("Shape defined by the connector spec"), so `field` and `index` are specified in `doc/connectors/opcua-connector-spec.md` §3.4 (task 5.1).
- [x] 1.3 Write the shared vectors in `doc/contract/test-vectors/opcua-struct/*.json`, each holding definitions, body hex, path and the expected value or error category. Cover every built-in type the walker must skip (String, ByteString, NodeId in all encodings, ExpandedNodeId, LocalizedText, QualifiedName, Variant with arrays and dimensions, DataValue, DiagnosticInfo, nested ExtensionObject, arrays of each), plus nesting, optional fields present and absent, union active and inactive, an Enumeration field, array elements (a primitive, a string, a structure element followed by a field, out of range, an array without an index), each type and rendering from "Additional built-in types" (including DateTime 0, a DateTime beyond 9999, an unnamed StatusCode, null strings and ByteStrings, and a NodeId in each id type and namespace form), a type mismatch, an unknown field, a truncated body, an oversized length prefix, and an encoding id mismatch.

## 2. Rust

- [x] 2.1 Add `field` (with optional `[n]` per segment) and `index` to the address in `connector-opcua/src/config.rs`, with validation: non-empty segments, a non-negative integer index, typed mode only for `field`, and read-only access for `field` and `index`.
- [x] 2.2 Add a pure `structure.rs` module: the definition model, plan compilation (path → skip and descend steps plus the expected built-in type, including the datatype check from D2), and the Part 6 walker with bounds-checked reads.
- [x] 2.3 Resolve definitions per session in `lib.rs`: read the `DataType` attribute and `DataTypeDefinition`, resolve nested types, and read the binary encoding id through `HasEncoding`. Cache them per session and clear them on reconnect. Turn failures into the point-level errors described in D6.
- [x] 2.4 Handle `ExtensionObject` in `build_sample`: raw mode gives the body from `ByteStringBody` and the `addr` ids in `nsu=` form, and a field point runs its plan. Keep the error string for unsupported types for everything else.
- [x] 2.5 Send `IndexRange` for `index` points in the Read and in the monitored item. Group reads by node and index in `read_points`, and monitored items by node in the subscription path, fanning out one sample per point.
- [x] 2.5a Extend `variant_to_value` / `build_sample` with the renderings from D8, selected by the declared `datatype`. The walker uses the same rendering functions for fields. Read `NamespaceArray` once per session for the `nsu=` forms. Add `bytes` to the capability datatypes.
- [x] 2.6 Unit tests that run the shared vectors, a proptest that the walker never panics or over-reads on arbitrary bodies and definitions, and a `structure_walk` fuzz target that is added to `just fuzz-all`.

## 3. C (not implemented in this change, design D10)

- [x] 3.1 Declare the capability `opcua-structures` known and missing in C (`KNOWN_CAPABILITIES`, `C_MISSING_CAPABILITIES`), and tag the new e2e cases `requires:opcua-structures`.
- [x] 3.2 Record the gap in the parity table of `impl/c/README.md` (structured values, array elements by index, the additional built-in types, and the C SDK's missing `bytes` datatype), and in TODO.md as a follow-up.
- [x] 3.3 Keep the shared e2e configuration loadable in C (its `bytes` points load there since 3.4), and the structure device in its own connector instance (`connector-structures.toml`, passed by `entrypoint.sh`; the C Dockerfile copies `connector-*.toml`), so it cannot slow the C build's other devices.
- [x] 3.4 Add `bytes` to the C SDK and read a top-level ByteString as `bytes` in the C module (hex value, bytes as `raw`, "accepted: bytes" for another declared datatype, refused on write, refused over 127 bytes); advertise it in the C capabilities and manifests; cover it in the e2e suite for both builds and in `impl/c/tests/config.c`. SNMP refuses `bytes` as the Rust module does.

## 4. Simulator, e2e, interop and parity

- [x] 4.1 Extend `connectors/opcua/sim/server.py` with asyncua `new_struct` / `new_enum` types whose `DataTypeDefinition` is populated: a flat struct, a nested struct, a struct with an array field before a scalar, a struct with an array of structures, a `Double[]` variable and an array-of-structures variable, top-level DateTime, LocalizedText, StatusCode, Guid, NodeId, QualifiedName and ByteString variables, and a struct carrying each of them, a struct with optional fields, a union, and an enum field. At least one of them should change value periodically, so the subscription path can be tested.
- [x] 4.2 Add OPC UA e2e cases: field points (poll and subscribe), indexed elements (in a structure, on an array variable, out of range), each additional built-in type in both of its renderings, one Read for several fields (checked via the simulator's read counter, or with a debug log assertion), a raw point on a struct, a type mismatch error, an absent optional field, and a reconnect after the simulator restarts. Tagged `requires:opcua-structures`, which the C build lists as missing until task group 3 is done.
- [x] 4.3 Add interop cases against UA-.NETStandard `TestServer/ExtensionObjects/PointValue`: a raw point gives a good sample with the 24-byte body, and a field point gives "data type definition unavailable" while the link stays up. Resolve the namespace with the existing `resolve-ns.py`.
- [x] 4.4 Run the OPC UA e2e suite against the C build: the configuration loads, the `requires:opcua-structures` cases are skipped, and everything else still passes.

## 5. Documentation and release

- [x] 5.1 Update `doc/connectors/opcua-connector-spec.md`: the `field` address in §3.4, and a new "Structured values" section (field points, raw passthrough, resolution, errors, limits, non-goals).
- [x] 5.2 Add a point-library example in `demo/points.d/opcua/` describing one structure as field points.
- [x] 5.3 Add an example to `flows/README.md` of decoding a raw structure body in a flow (hex → DataView), for servers without definitions.
- [x] 5.4 Add the `opcua-structures` gap to the parity table in `impl/c/README.md` (task 3.2) and TODO.md entries for the deferred items: the C port, writes, whole and runtime-length arrays, matrices, 1.03 servers, optional `datatype` and `inspect`, the automatic flow (D9), and the upstream draft.
- [x] 5.5 Add a release-note entry to `packaging/release-notes.md`.
- [x] 5.6 Reply on thin-edge/tedge-dot#57 with the design summary, including the answer to the flow question (outward-facing: only with the user's go-ahead).
