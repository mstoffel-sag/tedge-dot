## 1. Walker

- [x] 1.1 Add `impl/c/connectors/opcua/structure.{h,c}`: the type set, `uas_parse_path`, `uas_compile` (with the datatype check), the Part 6 walker (skip by encoded length, optional fields, unions, depth limit, the stop on zero-length elements) and the renderer (DateTime, StatusCode names, Guid, NodeId/ExpandedNodeId with `nsu=`, QualifiedName, base64, int64 outside the safe range as text). Pure, no static mutable state, messages identical to `structure.rs`.
- [x] 1.2 Add `impl/c/tests/opcua_struct.c`: run every `read` and `invalid_paths` vector from `doc/contract/test-vectors/opcua-struct/vectors.json`, plus the empty-structure-array and deep-nesting cases of the Rust tests, and the C size limits (C5).
- [x] 1.3 Add `impl/c/tests/fuzz_opcua_struct.c`: a libFuzzer target (`-DTDOT_FUZZ=ON`) and a deterministic random/mutation driver run by ctest; a `just fuzz-c-opcua-struct` recipe for the libFuzzer build (kept out of `just fuzz-all`, which runs on macOS, where Apple clang has no libFuzzer).

## 2. Connector

- [x] 2.1 Configuration: parse `address.field` (path errors as `point '<id>' address: …`) and `address.index`; refuse `field` with `mode = "raw"` and `field`/`index` on a writable point with the Rust messages; echo `field` and `index` in `addr`.
- [x] 2.2 Per-session resolution in the device state (C2): DataType attributes, definitions through one Read of BrowseName and DataTypeDefinition, Enumerations, simple subtypes through HasSubtype, abstract DataTypes through HasEncoding, the server's NamespaceArray, freed on disconnect; resolved lazily before a polled read and in `subscribe_device`/`retry_pending`.
- [x] 2.3 Decoding (C3, C4): a shared `decode_value` for poll and push: index unwrapping and `BadIndexRangeNoData`, field points (with the encoding id check), raw structure points with `addr.encoding_id`/`addr.data_type` and the 256-byte limit, top-level built-ins through the walker, and the existing primitives.
- [x] 2.4 Shared reads (C6): one ReadValueId per distinct node and index in `tdot_ua_read_blocks` and `read_points_inflight`, `IndexRange` for index points in Reads and monitored items; extend `tests/opcua_blocks.c`.

## 3. Parity bookkeeping and docs

- [x] 3.1 Remove `opcua-structures` from `C_MISSING_CAPABILITIES` in the justfile.
- [x] 3.2 Update the parity table in `impl/c/README.md` (structures at parity, C limits, monitored items per point), the TODO.md entry, the C references in `doc/connectors/opcua-connector-spec.md`, `doc/contract/test-vectors/opcua-struct/README.md`, `connectors/opcua/connector-structures.toml` and `demo/points.d/opcua/demo-sim-structures.toml`.
- [x] 3.3 Site: add `opcua-structures` to the C package's features in `site/src/data/packages.yaml`, drop it from the Rust package's "pick" text, and fix the picker tests that expect structures to need Rust.
- [x] 3.4 Add the release highlight `changes/opcua-structures-c.md`.

## 4. Verification

- [x] 4.1 C unit tests (ctest) green, including the vectors and the fuzz driver (the fuzz driver also clean for 300k inputs under ASan/UBSan). The two PKI tests need Python `cryptography`, as before.
- [x] 4.2 `just c-describe-parity` green for the OPC UA configurations.
- [x] 4.3 `just test-e2e-c opcua` (or the OPC UA e2e suite with the C build): the `requires:opcua-structures` cases pass (92 of 92, none skipped).
