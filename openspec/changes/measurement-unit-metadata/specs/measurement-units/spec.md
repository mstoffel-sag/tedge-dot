# Spec Delta

## Purpose

Carries a point's engineering unit from the connector configuration to the cloud measurement, as
retained thin-edge.io measurement metadata published by the ot-measurement flow, so a configured
`unit` is visible in Cumulocity without changing the measurement body.

## ADDED Requirements

### Requirement: Units are published as retained measurement metadata
For every measurement it publishes on `te/<entity>/m/<type>`, ot-measurement SHALL publish a retained message on `te/<entity>/m/<type>/meta`. The message is a JSON object with one key per series of that measurement that has a unit: `"<group>.<series>": {"unit": "<unit>"}`. Group and series are the names the measurement uses, and the unit is the sample's `unit`.

#### Scenario: A point with a unit
- **WHEN** ot-measurement maps a good sample of point `flow` on device `Pump01` with `unit = "l/m"` and `meta.measurement = {group = "flow", series = "flow"}`
- **THEN** it publishes the measurement `{"flow":{"flow":<value>},"time":…}` on `te/device/Pump01///m/flow`
- **AND** it publishes `{"flow.flow":{"unit":"l/m"}}` retained on `te/device/Pump01///m/flow/meta`

#### Scenario: Default naming
- **WHEN** a sample of OPC UA point `temperature` with `unit = "°C"` and no `meta.measurement` is mapped
- **THEN** the metadata is `{"opcua.temperature":{"unit":"°C"}}` on `te/device/<device>///m/opcua/meta`

### Requirement: The measurement body is unchanged
Series values in the measurement SHALL stay bare numbers. The unit SHALL travel only in the metadata message, so a mapper without measurement metadata support still receives every measurement as before.

#### Scenario: Body without unit
- **WHEN** a sample with `unit = "W"` is mapped
- **THEN** the series value in the measurement is a number, not an object

### Requirement: One metadata message holds every unit of a measurement topic
When several series share a measurement topic, the metadata message for that topic SHALL contain the units of all of them that the flow has seen. Publishing it for one series SHALL NOT drop another series' unit.

#### Scenario: Two series in one group
- **WHEN** points `inflow` (`unit = "°C"`) and `bearing` (`unit = "°C"`) are both mapped to group `temperatures`
- **THEN** the retained message on `te/<entity>/m/temperatures/meta` contains both `temperatures.inflow` and `temperatures.bearing`

### Requirement: Metadata is published only when it changes
The flow SHALL publish a metadata message when a series' unit is seen for the first time, when it changes, or when a series that had a unit is seen without one. Samples whose unit is unchanged SHALL NOT produce a metadata message.

#### Scenario: Steady stream
- **WHEN** 100 samples of the same point arrive with the same unit
- **THEN** exactly one metadata message is published for that measurement topic

#### Scenario: Unit edited in the connector config
- **WHEN** a point's `unit` changes from `"l/m"` to `"l/s"` and its next sample is mapped
- **THEN** the metadata message is published again with `"l/s"` for that series

#### Scenario: Unit removed
- **WHEN** a series that had a unit is mapped from a sample without `unit` (or with an empty one)
- **THEN** the metadata message is republished without that series
- **AND** when no series of the topic has a unit any more, an empty retained message clears the topic

### Requirement: Only mapped series carry units
Units SHALL be taken only from samples that ot-measurement maps to a measurement series: good quality, a numeric (or, with `include_boolean`, boolean) value, and not opted out with `meta.measurement = false`. A sample that is suppressed only by the deprecated on_change, deadband, min_interval or debounce settings SHALL still update the units.

#### Scenario: Opted-out point
- **WHEN** a point with `unit = "%"` has `meta.measurement = false`
- **THEN** no metadata is published for it

#### Scenario: Bad quality
- **WHEN** the only samples of a point are bad quality
- **THEN** no metadata is published for it

#### Scenario: Suppressed by on_change
- **WHEN** a sample is not published as a measurement because its value did not change, but its unit did
- **THEN** the metadata message is still republished with the new unit

### Requirement: Combine mode and custom target topics
In combine mode, the metadata message SHALL cover every series merged into the combined measurement of a topic. With a custom `target_topic`, metadata SHALL be published on `<target_topic>/meta` only when `target_topic` is a thin-edge.io measurement topic (`te/<4 segments>/m/<type>`). Otherwise no metadata is published.

#### Scenario: Combine
- **WHEN** combine is on and points `a` (`unit = "V"`) and `b` (`unit = "A"`) of one device are merged into `te/device/d1///m/modbus`
- **THEN** `te/device/d1///m/modbus/meta` holds both `modbus.a` and `modbus.b`

#### Scenario: Non-measurement target topic
- **WHEN** `target_topic` is `plant/line1/measurements`
- **THEN** measurements are published there as before and no metadata message is published
