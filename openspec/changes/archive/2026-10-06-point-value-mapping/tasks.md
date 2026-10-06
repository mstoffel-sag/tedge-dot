## 1. Contract, schemas and shared vectors

- [x] 1.1 Add `map` to contract §3.1. Add its validation rules to §3.3 (including the known-key check for `map` and its case keys) and replacement semantics to §3.4.
- [x] 1.2 Write a new §4.3 "Value mapping" covering cases, matching, `default`, `as`, unmatched values, pipeline order and the inverse on writes. Cross-reference it from §4.2, §5.2 (mapped parameters), §5.3 (the policy compares the mapped value; deadband only for numeric outputs) and §6.2.
- [x] 1.3 Add `source_value` and `source_value_repr` to the §5 envelope table and to `schemas/sample.schema.json`.
- [x] 1.4 Add a `map` definition (cases with `eq`/`min`/`max`/`to`/`write`, `default`, `as`) to `schemas/config.schema.json` and `schemas/point-library.schema.json`.
- [x] 1.5 Write shared vectors in `doc/contract/test-vectors/map/`. Read cases: eq, list, range, open ranges, overlap, float gap, default, `as` in every direction including whitespace and failure, cases that override `as`, no coercion, NaN, int64 as a string, a transform followed by a map, unmatched → bad. Write cases: scalar, list, range with and without `write`, default, `as` reverse, wrong type, with a transform, raw bypass. Number formatting cases: `0.1`, `1e21`, `-0`, integers.

## 2. Rust SDK

- [x] 2.1 Add `impl/rust/crates/sdk/src/map.rs`: a `ValueMap` type with `apply(&Value) -> Result<Value, MapError>` and `invert(&serde_json::Value) -> Result<serde_json::Value, String>` (the error lists the accepted values), plus the shortest round-trip number formatting.
- [x] 2.2 Parse and validate `map` in `config.rs`, covering every rule in the "Map validation" requirement, with errors that name the device, the point and the key or case index. Add `map` and its case keys to the known-key lists in `library.rs`. Keep `map` out of `DEEP_MERGED_KEYS`, so an override replaces it and `{}` clears it.
- [x] 2.3 In `runtime::raw_unit_request`, the one write path every write verb and the CLI share, unmap before `CommandRequest::to_raw_units` inverts the transform, and let a `raw` write bypass it. The map is read from the point's config, so `PointRef` and the connectors are unchanged.
- [x] 2.4 In `runtime.rs`, apply the map in one sample-finalising helper used by `publish_sample` (poll and push), the stdout path and the CLI `read`, before `report` offers the sample. Set `source_value`/`source_value_repr`. Make sure a mapping failure does not reach `note_poll` as a failed read. Rate-limit a debug log line when a point hits `default`.
- [x] 2.5 Add `source_value` and `source_value_repr` to `Sample` serialisation in `model.rs`, and leave them out when unset.
- [x] 2.6 In `descriptor.rs`, render the DTM type of a mapped parameter from the map's output type and add the `enum` of writable outputs. Drop the datatype limits for mapped points.
- [x] 2.7 Tests: run the shared vectors, add config validation unit tests, and add `to_raw_units` tests with a map and a transform. Add proptests to `tests/properties.rs` for `apply(invert(o)) == o` on every invertible output, and for `apply` being total (it never panics on any f64, string or bool).
- [x] 2.8 Add a fuzz target for `map` config parsing and application under `crates/sdk/fuzz/`.

## 3. C SDK

- [x] 3.1 Add `include/tedge_dot/map.h` and `sdk/src/map.c`: the map struct stored on `tdot_point_t`, `tdot_map_apply`, `tdot_map_invert` and number formatting that matches Rust (`%.17g`, shortened to the shortest form that round-trips).
- [x] 3.2 Parse, validate and library-replace `map` in `config.c` and `include/tedge_dot/config.h`, with the same rules and error wording as 2.2.
- [x] 3.3 In the write path in `factory.c`, unmap before `tdot_transform_invert`, and let `raw` bypass it.
- [x] 3.4 In `runtime.c`, apply the map in `emit_sample` (poll, `push_sink`, stdout) and in the CLI read, before the report offer. Add `source_value` to `envelope.c`. Make sure a mapping failure does not mark the transport down.
- [x] 3.5 Make the C `describe` emit the same DTM type and `enum` as 2.6, and extend `describe-parity.sh` with a mapped point.
- [x] 3.6 Unit tests in `impl/c/tests`: run the shared vectors, and add config validation and write-path tests.

## 4. Conformance, parity and e2e

- [x] 4.1 Add a layer-3 `ot-conformance` case with the Modbus simulator: a mapped holding register reads as a label with `source_value`, a label write sets the code, a read-only label write fails, and an unmatched value gives `bad` while the link stays up.
- [x] 4.2 Run the conformance and the new cases through the parity harness (`IMPL=rust` and `IMPL=c`), and record any gaps.
- [x] 4.3 Add an e2e case (Modbus or OPC UA suite) for a mapped state point that appears as a string in the parameter twin. Writing a label through the `write-batch` verb as `ot-command-forward` sends it changes the device register.
- [x] 4.4 Add a cloud e2e case to `cloud/modbus/tests/parameters_c8y.robot`: set an enumerated parameter from Cumulocity, and check the device code and the twin label.

## 5. Flows

- [x] 5.1 Add `flows/test-flows.sh` cases: `ot-measurement` accepts a sample whose number came from a map and skips a string-mapped sample. `ot-parameter-state` publishes the label, and `ot-command-forward` forwards a label unchanged.
- [x] 5.2 Document in `flows/README.md` that alarms and events on a mapped point test the label (`equals`), how to keep a numeric threshold, and that `source_value` carries the code for custom flows.

## 6. Documentation, demo and release

- [x] 6.1 Add a user-facing "Mapping values" doc with recipes: state codes to labels with ranges and a catch-all, numeric text to a measurement, an enumerated writable parameter, and keeping a code measurement with a numeric `to`.
- [x] 6.2 Reference `map` from each connector spec in `doc/connectors/*-spec.md` where state or text values are common (Modbus, OPC UA, SNMP, CANopen).
- [x] 6.3 Add a mapped point to the demo configs (for example an operating state on the Modbus or OPC UA demo), and check that `tedge-dot describe` renders it as an enum.
- [x] 6.4 Update `doc/migration/modbus-plugin-gap-analysis.md` G4: the Cloud Fieldbus `statusMapping` maps to `map`.
- [x] 6.5 Add a release-notes entry in `packaging/release-notes.md`.
