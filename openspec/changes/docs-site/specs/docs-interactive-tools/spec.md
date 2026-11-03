## ADDED Requirements

### Requirement: Config builder produces valid connector configs
The site SHALL offer a config builder, as step 2 of the config workbench, that runs entirely in the browser. It SHALL let a visitor
choose a protocol, fill in the `[connector]` settings, devices and points, and emit TOML that it
validates live against `doc/contract/schemas/config.schema.json`. Because that schema leaves the
protocol-specific tables (`connection`, `protocol_address`, a point's `address`) to the connector
specs, the builder SHALL also check the types of the keys it knows in those tables, from its own
protocol shapes (`site/src/data/protocols.json`), while keeping any other key. A schema error
SHALL be shown on the field it concerns. The output SHALL be copyable and downloadable. The builder SHALL say
that schema validation is structural, and point to `tedge-dot describe` for full validation.

#### Scenario: Valid config emitted
- **WHEN** a visitor picks `modbus`, adds one TCP device and two holding-register points
- **THEN** the builder shows TOML that validates against the config schema, with a copy and a download action

#### Scenario: Invalid field flagged
- **WHEN** a visitor enters a non-numeric value for a device's port
- **THEN** the field shows the schema error, and the copy action warns that the config is invalid

### Requirement: Config builder loads the shipped configs
The config builder SHALL be able to start from any file in `demo/config/`. CI SHALL check that
every file in `demo/config/` and `packaging/config/` validates against the schema and the
builder's protocol shapes, that every library in `demo/points.d/` validates against the
point-library schema, and that each config survives the builder's parse → emit round trip with
the same meaning.

#### Scenario: Start from the demo
- **WHEN** a visitor chooses "start from demo: opcua"
- **THEN** the form is filled from `demo/config/opcua.toml`

#### Scenario: Shipped config breaks the round trip
- **WHEN** a change to `packaging/config/snmp.toml` no longer round-trips through the builder
- **THEN** the docs check fails and names the file

### Requirement: Config workbench runs signals and configuration together
The config builder and the outcome views SHALL be one page, the **config workbench**, in three
steps that share one state: the **signals** (simulated), the **configuration** (the config
builder) and the **outcome** (the report-policy and batching views below). The former config
builder and report-policy playground URLs SHALL redirect to it.

The signals step SHALL offer up to four signals, each with a name, a behaviour and an optional
outage, and MAY start from examples, which set the configuration too. It SHALL NOT model the
device or its protocol: the protocol is a configuration choice. A point reads the signal with
the same `id`; each signal SHALL say how the configuration reads it (polled every
`poll_interval`, or subscribed for an OPC UA point the connector subscribes to), and a signal
that no point reads, and a point that reads no signal, SHALL be named. Adding, renaming or
removing a signal SHALL add, rename or remove its point. Switching the protocol SHALL keep the
points' ids and policies and give them the new protocol's addresses. Loading a configuration (a
demo, or pasted TOML) SHALL give it a signal per point of its first device (up to four),
resolving the point libraries it names in `points_from` as the connector does (§3.4), and SHALL
say what it left out.

The outcome SHALL run the configuration as the connector resolves it: a point's `poll_interval`,
or for a subscribed OPC UA point its `sampling_interval`, from the point, its device or
`[connector]` (§3.4), and its `report` merged key by key over the device's and `[connector]`'s
(§5.3). The batching view SHALL run the configuration's `batch` tables, and its `batch` form
SHALL edit `[connector] batch` of the same configuration. The outcome SHALL say which package
runs the configuration (both, or `tedge-dot-rs` when it uses a feature only that package has)
and link the package picker, which stays its own page.

To keep the page scannable, the builder's points, `[mqtt]`, `[connection]` and less used device
settings SHALL be collapsible, a collapsed point showing its datatype, unit, how it is read and
whether it sets its own policies; parts with a schema error SHALL open. The page's explanations
(what is checked, how to read the chart, why a signal is missing, what to try) SHALL be
collapsible too. A collapsed part SHALL stand out as one: a panel with an accent edge and a
chevron that turns when it opens.

Every input field of the workbench and the package picker SHALL have a short help text in a
popup next to its label, shown on hover, on keyboard focus and on tap, that never covers another
field. The texts SHALL live in one place, and a test SHALL fail when a field the config builder
offers, or a key of a protocol's `protocol_address`, `address` or `connection`, has none.

#### Scenario: A demo with a point library
- **WHEN** a visitor starts the configuration from `demo/config/opcua.toml`, whose points come from `points_from = ["demo-sim"]`
- **THEN** the signals step has a signal for each of the library's first four points, and the outcome shows them

#### Scenario: A connector default reaches the outcome
- **WHEN** a visitor sets `[connector] poll_interval = "5s"` in the configuration step
- **THEN** every point without its own `poll_interval` is read every 5 s in the outcome, and the signals step says where the interval comes from

#### Scenario: A signal without a point
- **WHEN** a visitor renames a point in the configuration so that no point has a signal's name
- **THEN** the signals step says the signal is not read, and the outcome leaves it out

#### Scenario: Batch form and TOML agree
- **WHEN** a visitor picks `frame` in the batching view
- **THEN** the configuration's TOML shows `batch = { group_by = "frame", window = ... }` on `[connector]`

### Requirement: Package picker
The site SHALL offer a package picker that recommends `tedge-dot-rs` or `tedge-dot-c` from the
visitor's answers (architecture, distribution or glibc age, protocols, features, footprint),
and explains the recommendation. It SHALL use the same package data that the install guide
renders.

#### Scenario: Old gateway
- **WHEN** a visitor answers armhf and Debian 9
- **THEN** the picker recommends `tedge-dot-c` and gives the glibc 2.17 floor as the reason

#### Scenario: PROFIBUS
- **WHEN** a visitor says they need PROFIBUS-DP
- **THEN** the picker recommends `tedge-dot-c`, or building `tedge-dot-rs` from source with the `profibus` feature

### Requirement: Report-policy playground matches the SDKs
The config workbench's outcome SHALL chart, for the point the visitor selects, the readings of
its signal (polled every `poll_interval`, or pushed by the server on change every
`sampling_interval` with a source time a few milliseconds off the sampling grid), the published
samples, the heartbeats and the outage, over a run length the visitor chooses. It SHALL show the
point's effective `report` policy and where it is merged from, and MAY offer presets that set the
point's own `report`. What these points publish SHALL be the input of the batching view on the
same page. Its policy engine SHALL pass every vector in
`doc/contract/test-vectors/report/vectors.json` in CI.

#### Scenario: Deadband on a noisy signal
- **WHEN** a visitor applies `deadband = 0.5` to a noisy signal
- **THEN** the chart marks only the readings that differ from the last published value by more than 0.5 as published

#### Scenario: Vector added without engine update
- **WHEN** a new vector is added to `vectors.json` that the playground engine does not satisfy
- **THEN** the docs check fails and names the vector

### Requirement: Batching playground matches the SDKs
The config workbench SHALL also show how measurement output's `batch` policy
(contract §5.5) turns the points' samples into measurements. Its input SHALL be every sample
the configured points publish, at the time their reporting policy publishes it, so that a reading
the policy held back, a signal read less often, a pushed signal that did not change, and an
outage all reach the batcher as they would on a gateway. The visitor SHALL choose a `batch`
policy (`time`; `frame` with a `window`; `sample` with an `every` and, optionally, an
`aggregate`) and the part of the run to show. When the configuration does not set
`output = "measurements"`, the view SHALL say so and offer to set it.

The playground SHALL first show the samples through every `group_by` side by side, each with its
number of measurements, of signals missing from a measurement, of samples not sent and how far a
time moved. It SHALL then chart, for the selected policy, every sample coloured by the
measurement it went into, the readings the reporting policy held back, the failed reads, the
samples that reach no measurement, and the frame windows or sample times. Every measurement SHALL
be drawn with one cell per signal, hollow for a signal it does not hold. For each measurement
the playground SHALL list its `time` and, per signal, its value or *missing*, and show it as
published on `te/device/<device>///m/<group>` and as the measurement Cumulocity stores, with
units. It SHALL show the `batch` table for the chosen policy. Its batching engine SHALL pass
every case in `doc/contract/test-vectors/measurements/vectors.json` in CI.

#### Scenario: A filtered signal under time and under sample
- **WHEN** a polled signal has `report = { deadband = 0.3 }` and the policy holds a reading back
- **THEN** under `time` the measurement of that poll shows the signal as missing, and under `sample` every measurement holds the signal's held value

#### Scenario: Pushed points in one frame
- **WHEN** a visitor picks pushed signals whose source times differ by a few milliseconds and `group_by = "frame"` with `window = "100ms"`
- **THEN** samples of one burst share a measurement stamped with its first sample's time, no sample is left out, and the largest time moved is under 100 ms

#### Scenario: A signal drops out
- **WHEN** one signal has an outage and the visitor picks `group_by = "sample"`
- **THEN** the measurements during the outage show that signal as missing, and it returns with its next good reading

#### Scenario: A spike between samples
- **WHEN** a pushed signal spikes between two samples, with `group_by = "sample"`, then with `aggregate = ["mean", "min", "max"]`
- **THEN** sample and hold marks the spike's samples as not sent, and the aggregate's `<series>_max` holds the spike's value

#### Scenario: Vector added without engine update
- **WHEN** a new case is added to the measurement-output vectors that the playground's engine does not satisfy
- **THEN** the docs check fails and names the case

### Requirement: Tools offer what the schema allows
The interactive tools SHALL keep up with the contract schema for the policies they show: the
report-policy playground SHALL have a field for every key of `report`, and the batching
playground SHALL offer every `batch.group_by` and every `batch.aggregate`. The config builder
SHALL offer the `[connector]` keys `output`, `max_inflight` and `max_inflight_total`, a device's
`max_inflight`, and a `batch` table. The docs check SHALL fail when the schema gains a `report`
key, a `group_by` or an aggregate that the playground does not offer.

#### Scenario: Schema gains a grouping policy
- **WHEN** a change adds `"window_sample"` to `batch.group_by` in `config.schema.json` without adding it to the batching playground
- **THEN** the docs check fails

