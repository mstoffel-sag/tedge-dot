# Proposal

## Why

A point's `unit` (contract §5, echoed in every sample as `sample.unit`) never reaches Cumulocity.
`ot-measurement` publishes bare numbers on purpose, because the tedge c8y mapper drops a series
value that is an object such as `{value, unit}`. So the unit is lost on the device: charts and data
point labels show bare numbers, and a configured `unit` has no visible effect. thin-edge.io 2.x
has a supported channel for units: retained measurement metadata on `te/<entity>/m/<type>/meta`,
which its `units` flow keeps and the c8y mapper adds to every measurement of that type. On a
thin-edge.io 2.0.1 device, one retained `{"flow.flow":{"unit":"l/m"}}` on
`te/device/Pump01///m/flow/meta` made Cumulocity receive `{"flow":{"flow":{"value":…,"unit":"l/m"}}}`.
The OPC-UA solution blueprint currently works around the gap with a device-side script that
publishes this metadata by hand.

## What Changes

- `ot-measurement` publishes the units of the series it maps as retained measurement metadata, on
  `<measurement topic>/meta`. It uses the unit the sample already carries and the same group and
  series naming as the measurement.
- One message per measurement topic holds the units of all its series. It is published when a
  series' unit is first seen, changes, or goes away, not with every sample.
- The measurement body is unchanged: bare numbers, so nothing changes for mappers without the
  units flow. On those, the retained metadata is simply unused.
- No connector (Rust or C) change: `sample.unit` is already in the contract and in both
  implementations' samples.

## Capabilities

### New Capabilities
- `measurement-units`: how a point's engineering unit travels from the connector configuration to
  the cloud measurement through the device-side flows. It covers the metadata topic and payload,
  when the metadata is (re)published, and how combine mode and a custom `target_topic` are handled.

### Modified Capabilities
<!-- none: no existing spec covers measurement mapping -->

## Impact

- `flows/ot-measurement/main.js` (and its `flow.toml` comments), `flows/README.md`, and the
  `ot-measurement` cases in `flows/test-flows.sh`.
- Devices: one extra retained message per measurement type with a unit, under the topics the flow
  already owns. No new dependencies.
- Docs: the flows README stops saying units cannot reach the cloud. The opcua-solution-blueprint
  can drop its unit workaround once a release contains this.
