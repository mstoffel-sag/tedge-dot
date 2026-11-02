## MODIFIED Requirements

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

When a point's `aggregate` is a list, each aggregate SHALL be published as the series
`<series>_<aggregate>`. Each of these series SHALL carry the point's unit in the units metadata.
When `aggregate` is a single string, the series SHALL keep its resolved name. A configuration
SHALL be refused when a series of an aggregated point has the same name as another series of
the device in the same group, with a message naming both points and the series.

#### Scenario: Default naming
- **WHEN** an OPC UA point `flow` on device `plc1` reads 118.6 with good quality
- **THEN** a measurement on `te/device/plc1///m/opcua` contains `"opcua": {"flow": 118.6}`

#### Scenario: Point meta wins
- **WHEN** the point sets `meta = { measurement = { group = "Environment", series = "Temperature" } }`
- **THEN** the series appears as `"Environment": {"Temperature": <value>}` on `m/Environment`

#### Scenario: Bad reading not measured
- **WHEN** a reading has quality `bad`
- **THEN** no series is published for it, and the link status and any sample are published as today

#### Scenario: Several aggregates
- **WHEN** a point `temp` with unit `°C` sets `batch = { group_by = "sample", every = "1s", aggregate = ["mean", "max"] }`
- **THEN** each sample holds `temp_mean` and `temp_max`, and the units metadata gives both the unit `°C`

#### Scenario: Aggregate name clash
- **WHEN** a point `temp` publishes `temp_max` and another point of the device in the same group is named `temp_max`
- **THEN** the configuration is refused, naming both points and `temp_max`
