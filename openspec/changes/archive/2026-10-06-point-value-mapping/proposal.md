## Why

Many OT signals carry a value whose *type* is wrong for the people and systems that use it:

- A device reports an operating state as a code (`0`, `1`, `2`…) that only means something
  after a lookup ("stopped", "running", "fault"). Ranges of codes often share one meaning
  (`10`–`19` = "warning"), and every value nobody listed still needs a sensible label.
- A device stores a number as text (an OPC UA `String` node holding `"21.5"`, an SNMP
  `DisplayString`, a Modbus ASCII register block). It cannot become a measurement, because
  `ot-measurement` only accepts numeric values.
- A writable setting is shown to an operator as a choice ("auto", "manual", "off"), but the
  device expects a code.

Today this can only be done in a flow, and only for reads. A flow cannot do the reverse
conversion on the write path: a Cumulocity device parameter edit for a mapped point would
reach the device with the label instead of the code. `transform` already solves the same
problem for linear scaling. It is a declared point field whose maths the SDK owns in both
directions. Value mapping is the non-linear counterpart and belongs at the same level. It also
covers the "status mapping" part of the Cloud Fieldbus migration gap (G4 in
`doc/migration/modbus-plugin-gap-analysis.md`).

## What Changes

- Add a new optional point field, **`map`**. It converts a point's value between the device's
  representation and the one published in samples. The **SDK runtime** applies it in both the
  Rust and C implementations, after `transform` on reads and before the inverse transform on
  writes. Connector modules do not change.
  - `cases`: an ordered list where the first match wins. A case matches an exact value
    (`eq`, a number, string or bool, or a list of them) or a numeric range (`min` and/or `max`,
    both inclusive, either side open). It maps the match to `to`, which is a number, string or
    bool.
  - `default`: the catch-all output for a value that matches no case.
  - `as`: a generic type conversion (`"number"`, `"string"` or `"bool"`) for values no case
    matched, for example to parse `"21.5"` into `21.5` or to format a number as text. It cannot
    be combined with `default`.
  - All outputs of one map share one value type, which is the sample's `value_repr`. A value
    that matches nothing, when the map has neither `default` nor `as`, gives a `bad` sample
    whose `error` names the value. This counts as a mapping failure, not as a failed read, so
    the link status is unaffected.
- **Writes**, from the `write` verb, every `write-batch` entry (and therefore device parameters
  from the cloud) and the CLI `write`, carry the *mapped* value. The SDK maps it back first,
  then inverts the transform and encodes it:
  - For an `eq` case, the inverse is its value (the first entry when `eq` is a list).
  - For a range case, the inverse is its optional `write` value. Without one, the case is
    read-only.
  - For `as`, the inverse is the reverse conversion.
  - `default` is never invertible.
  - A value with no inverse fails the write with a reason that lists the accepted values.
    Nothing is sent to the device.
- The sample envelope gains an optional **`source_value`**: the value before mapping (after
  `transform`). Flows and operators can then see both the code and its label.
- The reporting policy (`report`, §5.3) compares the mapped value, which is the value the
  sample carries.
- `tedge-dot describe` renders a mapped point's parameter with the mapped type. Its accepted
  write values become a JSON-schema `enum`, so Cumulocity shows a choice list.
- A point library MAY declare `map`. A site override replaces the whole `map` and does not
  merge with it, because cases are an ordered list.
- Add `map` to the contract (§3.1, a new §4.3, §5, §5.2 and §6.2), to the config,
  point-library and sample JSON schemas, to shared test vectors that both SDKs run, and to the
  conformance and e2e suites.
- Documentation: a user-facing section with the common recipes (state codes to labels, ranges
  with a catch-all, a string to a measurement, an enumerated writable parameter). Add a mapped
  point to the demo configs, and update the migration guide's G4 entry.

## Capabilities

### New Capabilities
- `point-value-mapping`: the SDK-runtime `map` point field. It covers exact, list and range
  cases, the `default` catch-all, `as` type conversion, the inverse mapping on every write
  path, how it orders with `transform` and `report`, its validation, the `source_value`
  envelope field, inheritance from point libraries, and rendering of mapped parameters in
  `describe`.

### Modified Capabilities
<!-- None: the existing specs (opcua-pki-management, opcua-secure-connections) do not cover
     sample decoding or writes. -->

## Impact

- **Contract and schemas**: `doc/contract/ot-connector-contract.md` (§3.1, §3.3, §3.4, new
  §4.3, §5, §5.2, §5.3 and §6.2), and `doc/contract/schemas/{config,point-library,sample}.schema.json`.
- **Rust SDK**: new `impl/rust/crates/sdk/src/map.rs`; `config.rs` (parse and validate),
  `library.rs` (`map` is replaced, not deep-merged), `connector.rs` (`to_raw_units` maps back
  before inverting the transform), `runtime.rs` (one place that applies the map ahead of
  `report` in the publish, stdout and CLI `read` paths), and `descriptor.rs` (the DTM schema
  type and `enum`).
- **C SDK**: new `impl/c/sdk/src/map.c` and `include/tedge_dot/map.h`; `config.c` and
  `config.h` (parse, validate, library replacement), `factory.c` (the write path),
  `runtime.c` (`emit_sample` and CLI read), `envelope.c` (`source_value`), and the C
  `describe`.
- **Connectors**: no changes. Mapping sits outside the module API.
- **Flows**: no code change is required. `ot-measurement` already accepts numbers produced by
  a map and skips strings. `ot-parameter-state` and `ot-command-forward` pass values through
  unchanged, and the SDK maps them back. Docs and flow tests should still cover a mapped
  parameter round trip.
- **Conformance and e2e**: shared vectors in `doc/contract/test-vectors/map/`, a layer-3
  conformance case, and a Modbus or OPC UA e2e case for a mapped state read and an enumerated
  parameter write. All of these run through the Rust/C parity harness.
- **Behaviour**: no change for existing configs, because `map` is opt-in. A mapped point
  changes its `value_repr`, so consumers that keyed on the old type, such as a measurement of
  a state code, need either a numeric `to` or `source_value`.
