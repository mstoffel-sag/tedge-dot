### Grouped output: measurement type and fragments

A measurement now has a **type** (the topic `te/device/<device>///m/<type>` and the Cumulocity
`type`) and a **fragment** (the key in the body) next to its **series**, instead of one `group`
that set both. Series of several fragments with the same type and time go into one measurement,
so a connector can build a Cumulocity measurement such as:

```json
{ "type": "c8y_CombinedSensorReading", "time": "…",
  "c8y_Environmental": { "temperature": { "value": 23.4, "unit": "°C" } },
  "c8y_Battery": { "level": { "value": 85, "unit": "%" } } }
```

```toml
[[device]]
name        = "sensor1"
measurement = { type = "c8y_CombinedSensorReading" }

  [[device.point]]
  id   = "t"
  meta = { measurement = { fragment = "c8y_Environmental", series = "temperature" } }
```

See [doc/measurement-output.md](../doc/measurement-output.md#several-fragments-in-one-measurement).

- `type`, `fragment` and `series` can be set on a point (`meta.measurement`), on a device (new
  `measurement` table) and on `[connector] measurement`. A fragment defaults to the type, and the
  type to the protocol id. The `ot-measurement` flow has the same `type` and `fragment`
  parameters and reads `meta.measurement.type` and `.fragment`.
- The output modes and the grouping table have names that say what they do:
  `output = "single"` (default) and `output = "group"`, and
  `grouping = { by, window, every, aggregate }`.
- With Streaming Lake Ingestion, each fragment is a table and a separate write. The fragment
  layout is a storage decision: grouping fragments into one measurement reduces the measurements
  sent, not the lake writes.
- The Cloud Fieldbus import writes `meta.measurement = { type, fragment, series }` from a device
  type's `measurementMapping`, with the same result as before.

**Existing configurations keep working** and publish the same topics and bodies. The old names
are deprecated and log a warning when the configuration loads:

| Deprecated | Use |
| --- | --- |
| `output = "samples"` | `output = "single"` |
| `output = "measurements"` | `output = "group"` |
| `batch = { group_by = … }` | `grouping = { by = … }` |
| `measurement.group`, `meta.measurement.group` | `type` (and `fragment`) |

A table level that sets both `batch` and `grouping` is refused.

**Action needed if you read the capability descriptor:** `outputs[]` now reports
`{ device, output, grouping }` with `output` as `"single"` or `"group"` and `grouping.by` in
place of `batch.group_by`. A tool that reads `outputs[].batch` or compares `outputs[].output`
with `"measurements"` must read `grouping` and `"group"`.

The OT Connector Contract is now version 0.5.0 (§5.5, `outputs`).
