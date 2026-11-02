# native-measurements Specification

## Purpose
A connector option that publishes batched thin-edge.io measurements directly instead of one sample per reading, with the same naming as the ot-measurement flow, a size budget, units metadata, and samples only for points that flows read.
## Requirements
### Requirement: Output mode
The `[connector]` table SHALL accept `output`, either `"samples"` (default) or
`"measurements"`. With `"samples"`, the runtime SHALL publish exactly what it publishes today.
With `"measurements"`, the runtime SHALL publish thin-edge measurements itself, as the
following requirements describe. Any other value SHALL be refused, with a message naming
`output` and the allowed values. Both builds SHALL accept the same values and refuse the same
invalid ones with the same message.

#### Scenario: Default unchanged
- **WHEN** a config has no `output` key
- **THEN** the connector publishes one sample envelope per published reading and no measurement

#### Scenario: Invalid value
- **WHEN** a config sets `output = "both"`
- **THEN** both builds refuse it with the same message, naming `output` and the allowed values

### Requirement: Measurement shape and naming
In measurement mode, a published reading SHALL become a series in a measurement on
`te/device/<device>///m/<group>` with the body `{"time": <ts>, "<group>": {"<series>": <value>}}`.
The group and series SHALL be resolved in this order:

1. the point's `meta.measurement.group` and `meta.measurement.series`
2. the `[connector]` `measurement` table's `group` and `series`
3. a split of the point id at the `measurement.point_separator`, when one is set and the id
   contains it
4. the protocol id for the group and the point id for the series

Only good-quality readings with a number or a boolean value SHALL become series. A boolean
SHALL become 1 or 0 unless `measurement.include_boolean = false`. A point with
`meta.measurement = false`, a string, a 64-bit integer carried as a string, and a raw-mode
point SHALL NOT become a series.

#### Scenario: Default naming
- **WHEN** an OPC UA point `flow` on device `plc1` reads 118.6 with good quality
- **THEN** a measurement on `te/device/plc1///m/opcua` contains `"opcua": {"flow": 118.6}`

#### Scenario: Point meta wins
- **WHEN** the point sets `meta = { measurement = { group = "Environment", series = "Temperature" } }`
- **THEN** the series appears as `"Environment": {"Temperature": <value>}` on `m/Environment`

#### Scenario: Bad reading not measured
- **WHEN** a reading has quality `bad`
- **THEN** no series is published for it, and the link status and any sample are published as today

### Requirement: Batched per device and group
The runtime SHALL put every series that shares a device, a group and a message time (see
`measurement-time-policy`) into one measurement message. A point SHALL appear at most once in a
message. A second reading of the same point SHALL go into a new message.

#### Scenario: One message per polled pass
- **WHEN** a device's 1,000 polled points in one group are read in one call, all with good quality
- **THEN** the series are published in as few messages as the size budget allows, all with the same `time`

#### Scenario: Repeated point
- **WHEN** a pushed point delivers two readings with the same timestamp
- **THEN** they go out in two messages, and neither value is dropped

### Requirement: Size budget
The runtime SHALL split a batch before its estimated Cumulocity size exceeds 15,000 bytes. The
estimate SHALL be the thin-edge message size plus 12 bytes per series. A measurement published
by the runtime SHALL never be rejected by the c8y mapper's payload limit.

#### Scenario: Large device stays under the limit
- **WHEN** a device publishes 5,000 series with long names in one group at one time
- **THEN** the series are split over several messages, and the c8y mapper publishes no `te/errors` message for them

### Requirement: Units metadata
In measurement mode, the runtime SHALL publish the units of a measurement topic as a retained
message on `<measurement topic>/meta`, holding `{"<group>.<series>": {"unit": "<unit>"}}` for
every series of the topic that has a unit. It SHALL publish it only when a unit appears,
changes or goes away, and before the first measurement that uses it. An empty map SHALL clear
the topic with an empty retained message.

#### Scenario: Unit published once
- **WHEN** a point with `unit = "°C"` is read 100 times
- **THEN** its topic's metadata message is published once, before its first measurement

### Requirement: Samples only where flows need them
In measurement mode, the runtime SHALL publish a sample envelope only for a point that has
`meta.alarm`, `meta.event` or `meta.parameter`, has `write` or `read_write` access, or sets
`sample = true`. A point with `sample = false` SHALL publish no sample, even when it would
otherwise. A sample published for a point that also became a series SHALL carry
`"measured": true`. Link status, capability descriptors and commands SHALL be unchanged.

#### Scenario: Plain point publishes no sample
- **WHEN** a point has none of the listed properties
- **THEN** its readings appear only in measurements

#### Scenario: Alarm point keeps its sample
- **WHEN** a point has `meta.alarm` and a numeric value
- **THEN** each published reading appears both as a series and as a sample with `"measured": true`

### Requirement: Flows don't duplicate or wake on measurements
`ot-measurement` SHALL ignore samples with `"measured": true`. `ot-alarm` and `ot-event` SHALL
NOT subscribe to measurement topics. Their measurement-series sources SHALL move to new flows,
`ot-alarm-series` and `ot-event-series`, with the same parameters and behavior.

#### Scenario: No double measurement
- **WHEN** a connector in measurement mode publishes a measured sample for an alarm point
- **THEN** `ot-measurement` publishes nothing for it, and the cloud receives the series once

#### Scenario: Series alarm still works
- **WHEN** `ot-alarm-series` is configured with a group, series and threshold, and a measurement crosses the threshold
- **THEN** it raises the same alarm `ot-alarm` raised before this change

### Requirement: Same output from both builds
For the same configuration and readings, the Rust and C builds SHALL publish the same
measurement topics, the same series in the same messages, and the same units metadata. A
shared set of test vectors SHALL define the measurement messages for given readings, and both
builds' unit tests SHALL check them.

#### Scenario: Builds agree
- **WHEN** both builds run the same `bench` config in measurement mode
- **THEN** the measurement messages are equal apart from timestamps, and the time groups are equal
