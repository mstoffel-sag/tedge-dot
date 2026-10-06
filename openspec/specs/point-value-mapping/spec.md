# point-value-mapping Specification

## Purpose
TBD - created by archiving change point-value-mapping. Update Purpose after archive.
## Requirements
### Requirement: Declared value map
A connector configuration SHALL accept an optional `map` table on a point, both inline and in a
point library. The table SHALL accept these keys:

- `cases`: an ordered list of cases. Each case SHALL have a `to` value (a number, string or
  bool) and SHALL match by either `eq` (a number, string or bool, or a non-empty list of them)
  or a numeric range (`min` and/or `max`, both inclusive). A range case MAY declare `write`, a
  number.
- `default`: a number, string or bool.
- `as`: one of `"number"`, `"string"` or `"bool"`.

Both the Rust and C SDK runtimes SHALL apply the map. Connector modules SHALL NOT apply it.
When a point has no `map`, its samples and writes SHALL be unchanged from today.

#### Scenario: State codes to labels
- **WHEN** a `uint16` point declares `map.cases = [{ eq = 0, to = "stopped" }, { eq = 1, to = "running" }]` and reads `1`
- **THEN** its sample has `value = "running"`, `value_repr = "string"`, `datatype = "uint16"` and `source_value = 1`

#### Scenario: No map
- **WHEN** a point declares no `map`
- **THEN** its samples carry no `source_value` and are identical to those published before this change

### Requirement: Case matching
The SDK SHALL evaluate `cases` in declaration order and SHALL use the `to` of the first
matching case.

- An `eq` case SHALL match a value equal to its scalar, or to any entry of its list.
- A range case SHALL match a numeric value `v` with `min <= v` (when `min` is set) and
  `v <= max` (when `max` is set).
- Numbers SHALL compare numerically. This includes 64-bit integers carried as strings (§4.1).
- Strings SHALL compare exactly and case-sensitively. A bool SHALL match only a bool.
- A value SHALL NOT be coerced to another type when matching.
- `NaN` SHALL match no case.

#### Scenario: Range with a catch-all
- **WHEN** a point's map is `cases = [{ eq = 0, to = "ok" }, { min = 1, max = 9, to = "warning" }, { min = 10, to = "fault" }]`, `default = "unknown"`, and the point reads `0`, `4`, `250` and `-3`
- **THEN** the samples carry `"ok"`, `"warning"`, `"fault"` and `"unknown"`

#### Scenario: First match wins on overlap
- **WHEN** the cases are `[{ eq = 5, to = "special" }, { min = 0, max = 10, to = "normal" }]` and the point reads `5`
- **THEN** the sample carries `"special"`

#### Scenario: List of values
- **WHEN** a case is `{ eq = [1, 5, 7], to = "running" }` and the point reads `7`
- **THEN** the sample carries `"running"`

#### Scenario: Strings match exactly
- **WHEN** a `string` point has `cases = [{ eq = "1", to = true }]` and `default = false`, and it reads `"01"`
- **THEN** the sample carries `false`: text is not compared as a number

#### Scenario: String to number
- **WHEN** a `string` point has `cases = [{ eq = "AUTO", to = 1 }, { eq = "MANUAL", to = 2 }]` and reads `"MANUAL"`
- **THEN** the sample carries `value = 2` with `value_repr = "number"`

#### Scenario: Float in a gap between inclusive ranges
- **WHEN** a `float32` point's cases are `[{ max = 9, to = "low" }, { min = 10, to = "high" }]`, its default is `"?"`, and it reads `9.5`
- **THEN** the sample carries `"?"`

### Requirement: Type conversion with `as`
When no case matches and `as` is set, the SDK SHALL convert the value to the named type:

- **Number** from a string: the string SHALL be parsed as a decimal number. Leading and
  trailing whitespace SHALL be ignored, and an empty or unparsable string SHALL fail. From a
  bool: `true` SHALL become `1` and `false` SHALL become `0`.
- **String** from a number: the shortest decimal representation that round-trips SHALL be
  used, identically in Rust and C. From a bool: `"true"` or `"false"`.
- **Bool** from a number: non-zero SHALL be `true`. From a string: `"true"`, `"1"`, `"on"`
  and `"yes"` SHALL be `true`, and `"false"`, `"0"`, `"off"` and `"no"` SHALL be `false`,
  compared case-insensitively. Any other string SHALL fail.
- A conversion to the value's own type SHALL be a no-op.

A failed conversion SHALL produce a `bad` sample, as an unmatched value does.

#### Scenario: Numeric text becomes a measurement value
- **WHEN** a `string` point has `map = { as = "number" }` and reads `" 21.5 "`
- **THEN** the sample carries `value = 21.5` and `value_repr = "number"`, and `ot-measurement` turns it into a measurement

#### Scenario: Cases override the conversion
- **WHEN** a `string` point has `map = { as = "number", cases = [{ eq = "N/A", to = -1 }] }` and reads `"N/A"`
- **THEN** the sample carries `-1`

#### Scenario: Unparsable text
- **WHEN** a `string` point has `map = { as = "number" }` and reads `"abc"`
- **THEN** the sample is `quality = "bad"` with an `error` naming the point and the value, and the device's link status is unaffected

#### Scenario: Number to text
- **WHEN** a `float64` point has `map = { as = "string" }` and reads `0.1`
- **THEN** the sample carries `"0.1"` in both implementations

### Requirement: Unmatched values
When no case matches, the SDK SHALL use `default` when it is set. Otherwise it SHALL use the
`as` conversion when `as` is set. Otherwise the sample SHALL be published with
`quality = "bad"`, no `value`, `error = "point <id>: no mapping for value <v>"`, the unmapped
value in `source_value`, and its `raw` bytes. A mapping failure SHALL NOT be treated as a
failed read: it SHALL NOT affect the link status, the reconnect logic or the liveness
watchdog.

#### Scenario: No catch-all
- **WHEN** a point's map has only `cases = [{ eq = 0, to = "off" }]` and reads `3`
- **THEN** it publishes a `bad` sample with `error = "point <id>: no mapping for value 3"` and `source_value = 3`, and the device's link stays `up`

### Requirement: Map validation
The SDK SHALL reject a configuration, at startup and on reload (keeping the running
configuration on reload), when a point's `map`:

- declares both `default` and `as`;
- has outputs (`to`, `default`, the type of `as`) that are not all of one JSON type;
- has a case without `to`, a case with both `eq` and a range, a case with neither, an empty
  `eq` list, or `min > max`;
- has a range case whose `write` lies outside its range, or a `write` on an `eq` case;
- has a range on a `string` or `bool` datatype, or an `eq` whose type can never match the
  point's value type;
- is declared on a `raw`-mode point or a `bytes` datatype;
- is neither empty nor has at least one of `cases`, `default` or `as`.

Each error SHALL name the device, the point and the offending key or case index. An empty
`map = {}` SHALL mean "no map".

#### Scenario: Mixed output types rejected
- **WHEN** a point's map is `cases = [{ eq = 0, to = "off" }, { eq = 1, to = 1 }]`
- **THEN** the configuration fails validation, naming the point and case 1

#### Scenario: Default with as rejected
- **WHEN** a point's map sets both `default = 0` and `as = "number"`
- **THEN** the configuration fails validation

#### Scenario: Eq of the wrong type rejected
- **WHEN** a `uint16` point's map has a case `{ eq = "a", to = 1 }`
- **THEN** the configuration fails validation with `map.cases[0].eq "a" can never match a uint16 value`, since a value is never coerced to match

#### Scenario: Write outside range rejected
- **WHEN** a case is `{ min = 10, max = 19, to = "warning", write = 25 }`
- **THEN** the configuration fails validation

### Requirement: Pipeline order
On reads, the SDK SHALL apply the map to the value after primitive decode and `transform`,
and before the reporting policy. On writes, the SDK SHALL reverse the map before the inverse
transform and the encoding. The reporting policy SHALL compare the mapped value. A numeric
deadband SHALL apply only when the map's output is numeric.

#### Scenario: Ranges in engineering units
- **WHEN** a `uint16` point with `transform = { multiplier = 0.1 }` has `map.cases = [{ max = 50.0, to = "normal" }, { min = 50.0, to = "hot" }]` and reads raw `612`
- **THEN** the sample carries `"hot"` with `source_value = 61.2`

#### Scenario: On-change sees the label
- **WHEN** a point with `report = { on_change = true }` maps `1..9` to `"warning"`, and reads `3` then `4`
- **THEN** only the first reading is published

#### Scenario: Bad samples untouched
- **WHEN** a mapped point's read fails
- **THEN** the `bad` sample has the read error and no `value` or `source_value`

### Requirement: Inverse mapping on writes
For a typed point with a map, the SDK SHALL map the value of every `write`, of every
`write-batch` entry and of the CLI `write` back to the device value before applying the
inverse transform. It SHALL find the first case whose `to` equals the written value (using the
matching rules) and SHALL write:

- the case's `eq` for a scalar `eq`;
- the first entry for an `eq` list;
- `write` for a range case.

When no case's `to` equals the value and `as` is set, it SHALL write the reverse `as`
conversion into the point's value type.

The write SHALL fail, with nothing sent to the device and a reason that lists the accepted
values, when:

- the only matching cases are range cases without `write`;
- the value matches only `default`, or nothing;
- the reverse conversion fails;
- the value's JSON type differs from the map's output type.

A `raw` (hex) write SHALL bypass the map. The command result SHALL echo the value as
requested.

#### Scenario: Write a label
- **WHEN** a `uint16` point maps `{ eq = 0, to = "stopped" }` and `{ eq = 1, to = "running" }`, and a `write` requests `"running"`
- **THEN** the connector is asked to write `1`, and the result is `successful` with value `"running"`

#### Scenario: List writes its first entry
- **WHEN** a case is `{ eq = [1, 5], to = "running" }` and `"running"` is written
- **THEN** `1` is written to the device

#### Scenario: Range with a write value
- **WHEN** a case is `{ min = 20, to = "fault", write = 20 }` and `"fault"` is written
- **THEN** `20` is written to the device

#### Scenario: Read-only label
- **WHEN** a case is `{ min = 10, max = 19, to = "warning" }` with no `write`, and `"warning"` is written
- **THEN** the write is `failed` with a reason listing the accepted values, and nothing is sent to the device

#### Scenario: Default not writable
- **WHEN** a map has `default = "unknown"` and `"unknown"` is written
- **THEN** the write is `failed`

#### Scenario: Reverse conversion
- **WHEN** a `string` point has `map = { as = "number" }` and `21.5` is written
- **THEN** the connector is asked to write `"21.5"`

#### Scenario: Composes with transform
- **WHEN** a `uint16` point with `transform = { multiplier = 0.1 }` maps `{ eq = 21.5, to = "comfort" }` and `"comfort"` is written
- **THEN** the connector is asked to write `215`

#### Scenario: Wrong type rejected
- **WHEN** a map with string outputs receives a write of `1`
- **THEN** the write is `failed` and nothing is sent to the device

#### Scenario: Cloud parameter round trip
- **WHEN** an operator sets a mapped parameter to `"stopped"` in Cumulocity, and `ot-command-forward` forwards it as a `write-batch` entry
- **THEN** the device receives the code for `"stopped"`, and the twin shows `"stopped"` once the next sample confirms it

### Requirement: Source value in the sample envelope
A sample of a point with a map SHALL carry `source_value` and `source_value_repr`: the value
after `transform` and before mapping, whenever the read produced a value. Samples of points
without a map SHALL NOT carry these fields. The sample JSON schema SHALL document them.

#### Scenario: Label and code together
- **WHEN** a mapped point reads code `2`, which maps to `"fault"`
- **THEN** the sample has `value = "fault"`, `value_repr = "string"`, `source_value = 2` and `source_value_repr = "number"`

### Requirement: Point library inheritance
The SDK SHALL accept `map` on a point declared in a point library. A site override that sets `map` on that point
SHALL replace the library's map as a whole. It SHALL NOT be merged key by key. `map = {}`
SHALL remove an inherited map. The §3.3 unknown-key check SHALL include `map` and the case
keys.

#### Scenario: Override replaces
- **WHEN** a library point declares `map = { cases = [...], default = "unknown" }` and the site sets `map = { as = "string" }` on it
- **THEN** the effective map is `{ as = "string" }` with no cases or default

#### Scenario: Misspelt key
- **WHEN** a case is written `{ eqs = 0, to = "off" }`
- **THEN** validation fails with `unknown key 'eqs' in map.cases[0] of point '<id>' (did you mean 'eq'?)`

### Requirement: Mapped parameters in describe
`tedge-dot describe` SHALL render a mapped parameter's property with the map's output type:
`string`, `boolean`, `integer` when every numeric output is a whole number, else `number`. It
SHALL NOT apply the datatype's range limits to it.

When the map's writable outputs form a closed set (no `as`), the property SHALL list them as an
`enum` in case order, without duplicates. Rust and C SHALL render identical output.

#### Scenario: Enumerated parameter
- **WHEN** a `read_write` `uint16` point maps `0 → "off"`, `1 → "auto"`, `2 → "manual"` and `10..19 → "warning"` (no `write`), with `default = "unknown"`
- **THEN** its DTM property is `{ "type": "string", "enum": ["off", "auto", "manual"] }`

#### Scenario: Open conversion
- **WHEN** a `read_write` `string` point has `map = { as = "number" }`
- **THEN** its DTM property is `{ "type": "number" }` with no `enum`

