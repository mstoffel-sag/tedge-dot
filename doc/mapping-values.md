# Mapping values: codes to labels, text to numbers

Devices often report a value in a form that suits the device, not the people who read it:

- An operating state is a code. `0` is "stopped" and `1` is "running", and a whole range of
  codes (`10`–`19`) may mean "warning".
- A number is stored as text, such as an OPC UA `String` node holding `"21.5"` or an SNMP
  `DisplayString`. Text cannot become a measurement.
- A setting is a choice for an operator ("auto", "manual", "off"), but a code for the device.

A point's **value map**, the `map` table, converts between the two forms in both directions.
Samples carry the readable value. Writes, including Cumulocity device parameters, carry the
readable value too, and the connector converts it back before it reaches the device.

The normative rules are in [contract §4.3](contract/ot-connector-contract.md#43-value-mapping).
This guide explains how to use them.

## What it does

`map` is applied by the **SDK runtime**, the same way for every connector and in both the Rust
and the C build. On a read it runs after the point's `transform`, so its numbers are in the
point's engineering units. On a write it runs the other way, before the transform is inverted.

| Key | Meaning |
| --- | --- |
| `cases` | A list of cases, checked **in order**. The first case that matches gives the value. |
| `cases[].eq` | Match this value, or any value in a list: `eq = 0`, `eq = [1, 5]`, `eq = "AUTO"`, `eq = true`. |
| `cases[].min`, `cases[].max` | Match a number in this range. Both bounds are **inclusive**. Leave one out for an open range. |
| `cases[].to` | The value the sample carries. |
| `cases[].write` | Range cases only: the code that a write of `to` sends. Without it, the range's label can be read but not written. |
| `default` | The value for anything that no case matches. It is never written. |
| `as` | Convert anything that no case matches to `"number"`, `"string"` or `"bool"`. It works in both directions, and it cannot be used together with `default`. |

All the outputs of one map must have the same type, so a point always publishes the same kind of
value. The sample's `datatype` still names the device type (`uint16`). Its `value_repr` gives
the type that was published (`string`), and `source_value` carries the original code.

```json
{ "point": "op_state", "datatype": "uint16",
  "value": "running", "value_repr": "string",
  "source_value": 1, "source_value_repr": "number", "quality": "good" }
```

## Recipes

### State codes to labels, with ranges and a catch-all

```toml
[[device.point]]
id       = "op_state"
datatype = "uint16"
address  = { table = "holding", address = 100, count = 1 }
map.cases = [
  { eq = 0,             to = "stopped" },
  { eq = [1, 5],        to = "running" },   # two codes, one label
  { min = 10, max = 19, to = "warning" },
  { min = 20,           to = "fault" },     # 20 and above
]
map.default = "unknown"                      # everything else (2, 3, 4, 6–9)
```

Write the specific values before the ranges that contain them, because the first match wins.

For a float point, make the ranges meet. With `{ max = 9 }` followed by `{ min = 10 }`, the value
`9.5` falls between the two and goes to `default`. Use `{ max = 10 }` followed by `{ min = 10 }`
instead: `10` matches the first case because it comes first.

### Numeric text to a measurement

```toml
[[device.point]]
id       = "tank_level"
datatype = "string"
address  = { node_id = "ns=2;s=TankLevelText" }
map      = { as = "number", cases = [{ eq = "N/A", to = -1 }] }
```

`" 21.5 "` becomes `21.5` (surrounding whitespace is ignored), and `ot-measurement` publishes it
as a measurement. Text that is not a decimal number, such as `"abc"`, `"0x1A"` or `"inf"`,
produces a `bad` sample that names the value. A case can give a special value its own number,
as `"N/A"` gets `-1` here.

The opposite direction also works. `as = "string"` turns a number into its shortest exact text
(`0.1`, `12`, never an exponent).

### An operator's choice: an enumerated device parameter

```toml
[[device.point]]
id       = "mode"
datatype = "uint16"
access   = "read_write"
address  = { table = "holding", address = 101, count = 1 }
map      = { cases = [{ eq = 0, to = "off" }, { eq = 1, to = "auto" }, { eq = 2, to = "manual" }] }
```

The parameter twin shows `"auto"`. `tedge-dot describe` renders the property as
`{ "type": "string", "enum": ["off", "auto", "manual"] }`, so the Cumulocity parameter UI offers a
choice. When an operator picks `"manual"`, `2` is written to the register. The `write` and
`write-batch` commands and `tedge-dot write --value manual` work the same way.

A write fails, and nothing is sent to the device, when:

- the value is not one of the map's labels: the reason lists the accepted values;
- the label belongs only to the `default` or to a range without `write`;
- the value has the wrong type, such as `1` for a map of labels.

To make a range writable, give it a code: `{ min = 20, to = "fault", write = 20 }`.

### Keeping a code as a measurement

`ot-measurement` publishes numbers only, so a point mapped to labels no longer produces a
measurement. You have two options:

- Map to numbers instead, for example to renumber vendor codes into your own scheme:
  `map.cases = [{ eq = 0, to = 100 }, { min = 1, max = 9, to = 200 }]`.
- Keep a second, unmapped point on the same address for the measurement, and give the mapped
  point `meta.measurement = false`.

## How it interacts with the rest

- **`transform`** runs first. Ranges and `eq` numbers are in engineering units: with
  `multiplier = 0.1`, `{ eq = 21.5, to = "comfort" }` matches raw `215`, and a write of
  `"comfort"` sends `215`.
- **`report`** (see [Reducing data volume](reducing-data-volume.md)) compares the mapped value. With
  `{ min = 10, max = 19, to = "warning" }` and `on_change = true`, a change from 12 to 15 is not
  published. A `deadband` applies only when the map produces numbers.
- **Alarms and events** see the label: `meta.alarm = { type = "pump_fault", when = { equals = ["fault"] } }`.
  A numeric threshold (`above`/`below`) needs numeric outputs.
- **Unmatched values**: without `default` or `as`, a value that no case matches produces a `bad`
  sample whose error names the value, and the code is kept in `source_value`. This counts as a
  mapping problem, not a failed read, so the device's link status is not affected.
- **Point libraries**: a library can declare `map`. A site's own definition of the point
  **replaces** the whole map, because cases are an ordered list. `map = {}` removes the map.

## What it does not do

The map converts one value at a time, by lookup or by type. It does not evaluate expressions,
format text (`"%.1f °C"`), match patterns or decode a bit mask into several flags. Use a flow
for those. For single bits of a status word, use the connector's bit-field addressing, such as
Modbus `start_bit`/`bit_count`.
