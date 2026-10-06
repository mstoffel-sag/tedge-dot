## Context

A sample's value is produced in two steps today. The first is primitive decode (§4), done by
the connector module. The second is the declared linear `transform` (§4.2). The connector
calls the SDK helper for it on reads, and the SDK inverts it at a single place on writes
(`CommandRequest::to_raw_units` in Rust, `factory.c` in C), which every write path goes
through. The reporting policy (`report`, §5.3) then runs in the SDK runtime in front of the
sample topic.

There is nothing for non-linear, type-changing conversions. Users do them in flows, which
works only for reads. Cumulocity device parameters (RFC 0003) write the twin's value back
through `ot-command-forward` → `write-batch`. A label shown in the twin would therefore be
written to the device as a string, and the encoder would reject it for an integer datatype.

Constraints:
- Rust and C must behave identically. They are checked by shared test vectors and by the
  parity harness.
- Connector modules stay "dumb". They decode and encode primitives only.
- Configuration lives in TOML and point libraries. The `meta` table stays free-form and is
  never interpreted by the SDK.

## Goals / Non-Goals

**Goals:**
- Declarative value→value mapping per point. It supports exact values, lists, numeric ranges
  and a catch-all, and works for number, string and bool on both sides.
- Generic type conversion: string↔number and bool↔number/string.
- A symmetric write path, so a mapped point works as a device parameter with no flow logic.
- One implementation per SDK, applied by the runtime, so connectors need no change.

**Non-Goals:**
- Expressions, scripting, string formatting templates (`"%.1f °C"`), regex matching. Those
  belong in flows.
- Scaling a value that was parsed from a string. `transform` runs before `map`, so a parsed
  `"215"` is not divided by 10. Combine the map with a flow for that. This is listed under
  Open Questions.
- Mapping raw-mode points or `bytes` values.
- Bit-field or bit-mask decoding of status words. Modbus bit fields already cover single bits.
- Mapping alarms or events. `meta.alarm` and `meta.event` in flows remain the way to do that.
  They can match on the mapped value or on `source_value`.

## Decisions

### D1. A first-class point field, applied by the SDK runtime, not a flow or `meta`

`map` sits next to `transform` and `report` in §3.1. The runtime applies it centrally when it
finalises a sample, for MQTT publish, stdout mode and the CLI `read`. Connectors do not apply
it, unlike `transform`, so no connector code changes and a new connector cannot forget it.

*Alternatives.* A flow (`ot-value-map`) cannot reverse the mapping on writes without
duplicating the point configuration into flow params, and C and Rust would not share it.
`meta.map` would make the SDK interpret `meta`, which the contract forbids.

### D2. Pipeline order: decode → transform → map → report

Reads apply `map` to the transformed value, so range bounds are in engineering units, the
same units an operator sees. The reporting policy then compares the **mapped** value, which
is what the sample carries. With `2..9 → "warning"`, a change from 3 to 4 is therefore not
a change, and that is the intent.

Writes run the reverse: unmap → inverse transform → encode.

*Alternative considered:* map before transform. Ranges would then be in raw device units,
and transform would have nothing to do once the value becomes a string. It was rejected.

### D3. Configuration shape

```toml
[[device.point]]
id       = "op_state"
datatype = "uint16"
access   = "read_write"
map.cases = [
  { eq = 0,              to = "stopped" },
  { eq = [1, 5],         to = "running" },        # several codes, one label; writes 1
  { min = 10, max = 19,  to = "warning" },        # range: read-only unless `write` given
  { min = 20,            to = "fault", write = 20 },
]
map.default = "unknown"                           # catch-all (read only)

[[device.point]]
id       = "level_text"
datatype = "string"
map      = { as = "number" }                      # "21.5" → 21.5, and back on write
```

- `cases` is evaluated in order and the first match wins. That makes overlapping ranges and
  "specific value before range" readable without precedence rules.
- `eq` accepts a scalar or a list, so the common "several codes → one label" needs no
  repetition.
- `min` and `max` are inclusive and either may be absent. Inclusive bounds match how
  integer state tables are written in device manuals. A float value between `max = 9` and
  `min = 10` falls through to `default`. The docs say to use contiguous bounds, for example
  `max = 10` followed by `min = 10`, where first-match-wins settles the shared edge.
- `default` and `as` are mutually exclusive, because both define the result for an unmatched
  value.

*Alternatives:* a TOML table keyed by value (`map = { "0" = "stopped" }`). TOML keys are
always strings, so the table cannot express ranges, bools or a value order. It was rejected.
A separate `cast` field was also considered and rejected: `as` inside `map` keeps one place
to look and lets cases override the conversion for special values (`{ eq = "N/A", to = -1 }`
together with `as = "number"`).

### D4. Matching semantics

- Numbers compare numerically. An `int64`/`uint64` carried as a string (§4.1) is compared as
  an integer, so `eq = 9007199254740993` works.
- A numeric `eq` never matches a string value and the reverse is also true. A bool matches
  only a bool. There is no implicit coercion when matching. Use `as` for coercion.
- String comparison is exact and case-sensitive.
- Ranges apply to numeric values only. A `NaN` value matches no case.
- `stale` samples are mapped like `good` ones. `bad` samples carry no value and are left
  untouched.

### D5. One output type per map

Every `to`, the `default`, and the type named by `as` must produce the same JSON type. A
config that mixes them is rejected with an error that names the point and the offending case.
This guarantees a stable `value_repr`, which the measurement flow, the parameter twin and the
DTM schema all depend on. `value_repr` reflects the mapped type. `datatype` keeps naming the
device primitive, so consumers can still tell that a `uint16` is behind the label.

### D6. Unmatched values with no catch-all give a `bad` sample

If no case matches and the map has neither `default` nor `as`, the sample is `quality = "bad"`
with `error = "point <id>: no mapping for value <v>"`. It keeps `raw` and carries the
unmapped value as `source_value`.

This does **not** count as a read failure: the link status, reconnect logic and heartbeat
bookkeeping treat it as a successful read.

*Alternative:* pass the unmapped value through. It was rejected because it breaks D5, since
the point would publish a string one moment and a number the next.

### D7. `source_value` in the envelope

Mapped samples carry `source_value`, which is the value after `transform` and before `map`,
with its own `source_value_repr`. Unmapped points do not carry it, so existing envelopes stay
byte-identical. A flow can then raise an alarm on the code while the twin shows the label,
and an operator can debug a `default` hit.

### D8. Inverse mapping on writes

The inverse is applied on the existing single write path, before the inverse transform: in
Rust `runtime::raw_unit_request`, which reads the map from the point's config so `PointRef` and
the connectors stay unchanged, and in C `tdot_connector_write`. `write`, every `write-batch`
entry and the CLI `write` all go through it.
The SDK finds the first case whose `to` equals the written value, compared with the same
rules as D4, and writes the case's raw value:

| Case kind | Raw value written |
| --- | --- |
| `eq` scalar | that value |
| `eq` list | its first entry |
| range with `write` | `write` (validated at load to lie within the range) |
| range without `write` | none: the case is read-only |
| no case matched, `as` set | the reverse conversion (`"21.5"` ← `21.5`, `1` ← `true`) |
| no case matched, `default` or nothing | none |

When there is no inverse, the write is `failed` with
`point <id>: cannot write "<v>"; accepted values: "stopped", "running", "fault"` and nothing
is sent. A write whose JSON type differs from the map's output type fails the same way. A
`raw` hex write bypasses the map, as it bypasses `transform` today. The result echoes the value
as requested, consistent with §4.2.

The written value then goes through the inverse transform and integer rounding unchanged, so
`map` and `transform` compose.

### D9. Point libraries: `map` is replaced, not merged

`meta`, `transform` and `report` are deep-merged (`DEEP_MERGED_KEYS`). A merged `cases` list
would have no meaningful order, so a site override of `map` replaces the library's map as a
whole. `map = {}` removes an inherited map. `map` is added to the §3.3 known-key check so
misspellings such as `maps` and `mapping` are reported.

### D10. `describe` and the parameter UI

A mapped writable point's DTM property takes the map's output type (`string`, `number`,
`integer` when all numeric outputs are whole numbers, or `boolean`). When the inverse is a
closed set, meaning only `eq` cases and range cases with `write`, and no `as`, the property
gets `"enum": [...]` listing the writable outputs in case order. Datatype `minimum` and
`maximum` limits are dropped for mapped points, because they describe the raw value.
`meta.parameter.min` and `meta.parameter.max` still apply and describe the mapped value.

### D11. Shared vectors and properties

`doc/contract/test-vectors/map/*.json` holds `{ point config, input value, expected output |
error }` for reads and `{ written value, expected raw | error }` for writes. Both SDKs run
them. A Rust proptest checks two properties:

- for every invertible output `o`, `map(unmap(o))` yields `o`;
- `map` never panics and is total for finite and non-finite inputs.

## Risks / Trade-offs

- [A changed `value_repr` breaks existing consumers of a point] → `map` is opt-in, and the
  docs recipe shows `source_value` or a numeric `to` for keeping a measurement.
  `ot-measurement` skips strings silently, so turning a numeric point into labels drops its
  measurement. The docs call this out.
- [The deadband does not apply to a string map] → this is intended: labels compare exactly.
  It is documented next to the `report` rules.
- [A typo in `eq` becomes a silent `default` hit] → `source_value` makes it visible, and an
  `eq` of a type the point can never produce is refused at load.
- [Inclusive float ranges leave gaps] → first-match-wins and documented contiguous bounds.
  A vector covers a value in the gap.
- [C and Rust drift] → shared vectors plus parity harness runs. Number formatting for
  `as = "string"` uses the shortest round-trip digits in positional notation in both: Rust's
  `{}`, and in C the fewest `%.*e` digits that read back (trying the last digit's neighbours
  next to a power of two), rendered without an exponent. Vectors pin it, for example
  `0.1 → "0.1"` and `1e21`. A C value holds at most 255 characters, so a number whose positional
  text is longer (above about 1e255) fails the conversion in C.
- [Write races between a label and the twin] → none new. The twin shows the requested label
  until the next sample confirms it, as for any parameter.

## Migration Plan

This is additive. No existing config changes behaviour. The demo configs gain one mapped
point, and the migration guide maps Cloud Fieldbus `statusMapping` to `map`. To roll back,
remove `map` from the point.

## Open Questions

- Should `as = "number"` on a string point apply `transform` after parsing, so a decimal
  string can be scaled? This design keeps the fixed order (D2). Revisit it if a real device
  needs it.
- Should there be an option for case-insensitive string matching (`map.ignore_case = true`)?
  It is left out until it is asked for.
- Should the capability descriptor publish each point's map outputs (`point_maps`) so flows
  and UIs can list labels without the config? `describe` already covers the DTM case.
