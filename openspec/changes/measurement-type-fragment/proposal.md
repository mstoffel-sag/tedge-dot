## Why

A connector cannot produce a Cumulocity measurement in which one `type` holds several fragments
(issue thin-edge/tedge-dot#126):

```json
{
  "time": "2026-11-12T12:23:00.000Z",
  "type": "c8y_CombinedSensorReading",
  "c8y_Environmental": { "temperature": { "value": 23.4, "unit": "°C" } },
  "c8y_Battery": { "level": { "value": 85, "unit": "%" } }
}
```

One name, `group`, sets both the measurement type (the thin-edge topic
`te/device/<d>///m/<group>`) and the fragment (the body key). Each fragment therefore has its own
topic and its own message, and a fragment per series means one measurement per series.

The names also describe the transport instead of what the setting does: `output =
"measurements"` means "put many series into one measurement", and the `batch` table holds the
policy for that grouping, while "batch" is also what Cumulocity calls sending several whole
measurements in one API request.

## What Changes

- **Type and fragment.** The `measurement` naming gets `type` (the topic, the Cumulocity `type`)
  and `fragment` (the body key), next to `series`. A fragment defaults to the type. `group` stays
  as a deprecated alias that sets both, so existing configurations publish the same topics and
  bodies.
- **Device level.** A device may carry its own `measurement = { type, fragment, series }`.
- **Several fragments per measurement.** In grouped output the runtime groups series by device,
  type and time instead of device, group and time. Series of different fragments with the same
  type and time go into one message.
- **Output modes renamed.** `output = "single"` (default) and `output = "group"`. `"samples"` and
  `"measurements"` stay accepted as deprecated aliases.
- **Grouping table renamed.** `batch = { group_by, window, every, aggregate }` becomes
  `grouping = { by, window, every, aggregate }`. `batch` stays accepted as a deprecated alias.
- **Flow.** `ot-measurement` gets the `type` and `fragment` parameters and reads
  `meta.measurement.type` and `.fragment`, with the same resolution.
- **Capability descriptor.** `outputs` reports `output` under the new names and the merged
  `grouping` table (`by` instead of `group_by`).

Not in this change: whether the connector should build every measurement (issue #126, question
3).

## Capabilities

### Modified Capabilities

- `native-measurements`: output mode names, type/fragment naming, several fragments per message.
- `measurement-time-policy`: the `grouping` table replaces `batch`.

## Impact

- Contract §3.3, §5.5 and §7, config and status schemas, measurement test vectors.
- Rust SDK (`measure.rs`, `config.rs`, `library.rs`, `descriptor.rs`, `runtime.rs`) and C SDK
  (`config.c`, `measure.c`, `runtime.c`) with their tests.
- `ot-measurement` flow, the Cloud Fieldbus import, connector example configs, bench scenarios.
- Docs: `doc/measurement-output.md`, contract, connector specs, migration guides, the docs site
  (guides, config builder, grouping playground).
- Compatibility: no configuration changes its output. Status consumers that read
  `outputs[].batch` or `outputs[].output == "measurements"` must read `grouping` and `"group"`.
