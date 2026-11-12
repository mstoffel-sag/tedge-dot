## MODIFIED Requirements

### Requirement: Output mode
The `[connector]` table SHALL accept `output`, either `"single"` (default) or `"group"`. With
`"single"`, the runtime SHALL publish one sample envelope per reading, as before this option
existed. With `"group"`, the runtime SHALL publish thin-edge measurements itself, as the following
requirements describe. `"samples"` SHALL be accepted as a deprecated alias of `"single"` and
`"measurements"` as a deprecated alias of `"group"`, each with a warning at load. Any other value
SHALL be refused, with a message naming `output` and the allowed values. Both builds SHALL accept
the same values and refuse the same invalid ones with the same message.

#### Scenario: Default unchanged
- **WHEN** a config has no `output` key
- **THEN** the connector publishes one sample envelope per published reading and no measurement

#### Scenario: Deprecated alias
- **WHEN** a config sets `output = "measurements"`
- **THEN** the connector behaves as with `output = "group"`, logs a deprecation warning, and its descriptor reports `"group"`

#### Scenario: Invalid value
- **WHEN** a config sets `output = "both"`
- **THEN** both builds refuse it with the same message, naming `output` and the allowed values

### Requirement: Measurement shape and naming
In grouped output, a published reading SHALL become a series in a measurement on
`te/device/<device>///m/<type>`, inside the fragment `<fragment>` of the body
`{"time": <ts>, "<fragment>": {"<series>": <value>}, …}`. The type, fragment and series SHALL be
resolved key by key from the point's `meta.measurement`, the device's `measurement` table and the
`[connector]` `measurement` table, in that order, where `group` at a level sets both the type and
the fragment at that level unless the level sets them itself. When no level names one, the type
SHALL be the prefix of the point id split at `measurement.point_separator` (when set and
contained), else the protocol id; the fragment SHALL be that prefix, else the type; the series
SHALL be the suffix of that split, else the point id.

Only good-quality readings with a number or a boolean value SHALL become series. A boolean
SHALL become 1 or 0 unless `measurement.include_boolean = false`. A point with
`meta.measurement = false`, a string, a 64-bit integer carried as a string, and a raw-mode
point SHALL NOT become a series.

When a point's `aggregate` is a list, each aggregate SHALL be published as the series
`<series>_<aggregate>`. Each of these series SHALL carry the point's unit in the units metadata.
When `aggregate` is a single string, the series SHALL keep its resolved name. A configuration
SHALL be refused when a series of an aggregated point has the same name as another series of
the device in the same type and fragment, with a message naming both points, the series, the
fragment and the type.

#### Scenario: Default naming
- **WHEN** an OPC UA point `flow` on device `plc1` reads 118.6 with good quality
- **THEN** a measurement on `te/device/plc1///m/opcua` contains `"opcua": {"flow": 118.6}`

#### Scenario: Point meta wins
- **WHEN** the point sets `meta = { measurement = { group = "Environment", series = "Temperature" } }`
- **THEN** the series appears as `"Environment": {"Temperature": <value>}` on `m/Environment`

#### Scenario: One type, several fragments
- **WHEN** `[connector] measurement = { type = "c8y_CombinedSensorReading" }`, point `t` sets `meta.measurement = { fragment = "c8y_Environmental", series = "temperature" }` and point `b` sets `meta.measurement = { fragment = "c8y_Battery", series = "level" }`, and both are read with the same time
- **THEN** one measurement on `m/c8y_CombinedSensorReading` holds `"c8y_Environmental": {"temperature": …}` and `"c8y_Battery": {"level": …}`

#### Scenario: Fragment defaults to the type
- **WHEN** a device sets `measurement = { type = "c8y_Sensor" }` and its point names no fragment
- **THEN** the series appears in the fragment `c8y_Sensor` on `m/c8y_Sensor`

#### Scenario: Separator names the fragment under a type
- **WHEN** `[connector] measurement = { type = "Combined", point_separator = "." }` and a point is named `c8y_Battery.level`
- **THEN** the series appears as `"c8y_Battery": {"level": …}` on `m/Combined`

#### Scenario: Bad reading not measured
- **WHEN** a reading has quality `bad`
- **THEN** no series is published for it, and the link status and any sample are published as today

#### Scenario: Several aggregates
- **WHEN** a point `temp` with unit `°C` sets `grouping = { by = "sample", every = "1s", aggregate = ["mean", "max"] }`
- **THEN** each sample holds `temp_mean` and `temp_max`, and the units metadata gives both the unit `°C`

#### Scenario: Aggregate name clash
- **WHEN** a point `temp` publishes `temp_max` and another point of the device in the same type and fragment is named `temp_max`
- **THEN** the configuration is refused, naming both points, `temp_max`, the fragment and the type

### Requirement: Batched per device and group
The runtime SHALL put every series that shares a device, a type and a message time (see
`measurement-time-policy`) into one measurement message, with one entry per fragment. A point
SHALL appear at most once in a message. A second reading of the same point SHALL go into a new
message.

#### Scenario: One message per polled pass
- **WHEN** a device's 1,000 polled points of one type are read in one call, all with good quality
- **THEN** the series are published in as few messages as the size budget allows, all with the same `time`, whatever their fragments

#### Scenario: Repeated point
- **WHEN** a pushed point delivers two readings with the same timestamp
- **THEN** they go out in two messages, and neither value is dropped

### Requirement: Size budget
The runtime SHALL split a message before its estimated Cumulocity size exceeds 15,000 bytes. The
estimate SHALL be the thin-edge message size, all fragments included, plus 12 bytes per series. A
measurement published by the runtime SHALL never be rejected by the c8y mapper's payload limit.

#### Scenario: Large device stays under the limit
- **WHEN** a device publishes 5,000 series with long names of one type at one time
- **THEN** the series are split over several messages, and the c8y mapper publishes no `te/errors` message for them

### Requirement: Units metadata
In grouped output, the runtime SHALL publish the units of a measurement topic as a retained
message on `<measurement topic>/meta`, holding `{"<fragment>.<series>": {"unit": "<unit>"}}` for
every series of the topic, in every fragment, that has a unit. It SHALL publish it only when a
unit appears, changes or goes away, and before the first measurement that uses it. An empty map
SHALL clear the topic with an empty retained message.

#### Scenario: Unit published once
- **WHEN** a point with `unit = "°C"` is read 100 times
- **THEN** its topic's metadata message is published once, before its first measurement

#### Scenario: Units of several fragments
- **WHEN** two points of one type in the fragments `c8y_Battery` and `c8y_Environmental` have units
- **THEN** the topic's metadata message holds both, keyed `c8y_Battery.<series>` and `c8y_Environmental.<series>`
