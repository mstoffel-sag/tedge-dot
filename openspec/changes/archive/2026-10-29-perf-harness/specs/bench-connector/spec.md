## ADDED Requirements

### Requirement: Synthetic protocol module in both builds
The Rust and the C build SHALL each provide a connector module with protocol id `bench`. Rust
SHALL compile it only with the `bench` Cargo feature, and C only with the `TDOT_BENCH` CMake
option. The module SHALL work against no device and generate readings from the configuration
alone. Both builds SHALL accept the same configuration keys and refuse the same invalid values
with the same messages. Writes and methods SHALL be unsupported.

#### Scenario: Runs without a device
- **WHEN** either binary, built with the module, runs a config with `protocol = "bench"`, one device and 100 points
- **THEN** the device's link reports `connected` and one sample per point is published at each poll

#### Scenario: Same config in both builds
- **WHEN** a config sets an invalid `change = 1.5` on a point
- **THEN** both builds refuse it with the same message, naming the point and `change`

#### Scenario: Write refused
- **WHEN** a `write` command targets a `bench` point in either build
- **THEN** the command fails as unsupported

### Requirement: Deterministic values, identical across builds
Each point SHALL produce values from a stream seeded with FNV-1a 64 over the bytes
`<device>\0<point id>` (a zero seed becomes 1) and advanced by xorshift64*. A draw SHALL be
`u = (x >> 11) · 2⁻⁵³`. For each reading, a first draw SHALL decide whether the reading
changes the value (`u < change`, where `change` is the point's fraction from 0 to 1, default
1.0). A change SHALL apply the point's `model`:

- `"walk"` (default): adds `(2u − 1) · step`, using a second draw, with `step` defaulting
  to 1.0
- `"constant"`: never changes
- `"counter"`: adds 1

Datatypes:

- `float64` SHALL carry the value.
- `int32` SHALL carry it rounded half away from zero.
- `bool` SHALL flip on each change.
- `string` SHALL carry the decimal text of the change count.

Shared vectors in `doc/contract/test-vectors/bench/` SHALL list the first 1,000 values of a set
of cases. Both builds' unit tests SHALL match them bit for bit.

#### Scenario: Repeatable sequence
- **WHEN** the same configuration runs twice in one build
- **THEN** the n-th sample of each point has the same value in both runs

#### Scenario: Builds agree
- **WHEN** the Rust and the C build run the same configuration
- **THEN** the n-th sample of each point has bit-identical values in both builds

#### Scenario: Change ratio
- **WHEN** a point sets `change = 0.2` and is read 10,000 times
- **THEN** between 1,800 and 2,200 of the readings differ from the previous one

### Requirement: Polled reads with modelled latency
`read_points` SHALL return one good-quality sample per requested point. The device
`protocol_address` SHALL accept `latency`, a duration (default 0), and `batch`, a point count
(default unlimited). A call SHALL take `latency` for each block of up to `batch` points before
returning. In the C build the wait SHALL block the caller, as a blocking protocol call would.

#### Scenario: Latency per block
- **WHEN** a device sets `latency = "2ms"` and `batch = 500` and 2,000 points are read in one call
- **THEN** the call takes at least 8 ms in either build

#### Scenario: No latency by default
- **WHEN** a device sets neither key
- **THEN** reading 10,000 points returns without waiting

### Requirement: Pushed points
The device `protocol_address` SHALL accept `push_rate`, the samples per second across the
device's pushed points (default 0). On a device with `push_rate` above 0, every point SHALL be
delivered by push unless it sets `subscribe = false`; on a device without it, every point SHALL
be polled. Each pushed
sample SHALL be for a pushed point chosen from the device's stream.

- The Rust module SHALL deliver the samples through the subscription sink, spread evenly over
  time.
- The C module SHALL deliver, at each `drain_subscriptions`, the samples owed since the
  previous drain (`push_rate × elapsed`), carrying the fractional remainder to the next drain.

Over any window of at least one second, both builds SHALL deliver `push_rate` samples per
second within ±5 %. The subscription health check SHALL report healthy while the device is
subscribed.

#### Scenario: Push rate
- **WHEN** a device has 1,000 pushed points and `push_rate = 10000`
- **THEN** each build delivers 9,500–10,500 samples per second on the device's sample topics, with no polling of those points

#### Scenario: C delivery follows the tick
- **WHEN** the C runtime's tick is 200 ms and `push_rate = 10000`
- **THEN** each drain delivers about 2,000 samples and no sample is lost between drains

### Requirement: Source timestamps on pushed samples
In the Rust build, the device `protocol_address` SHALL accept `ts_jitter` (a duration, default
0), `ts_resolution` (a duration of at least 1 ms, default 1 ms) and `publish_interval` (a
duration, default 0). For a pushed sample, `ts` SHALL be its generation time minus a uniform
draw from [0, `ts_jitter`], floored to `ts_resolution`. The draws SHALL come from a per-device
stream seeded with FNV-1a 64 over `<device>\0#ts`, so setting these keys does not change any
point's values. With `publish_interval` above 0, samples SHALL be delivered together once per
interval, each keeping its own `ts`. Polled samples SHALL keep the read time. The C build SHALL
refuse the three keys with a message saying that the C build does not support source
timestamps yet.

#### Scenario: Timestamps precede delivery
- **WHEN** a device sets `ts_jitter = "50ms"` and `publish_interval = "100ms"`
- **THEN** every pushed sample's `ts` is at most 50 ms before its generation time and at most 150 ms before it is published, and samples are published in groups about 100 ms apart

#### Scenario: Resolution makes timestamps coincide
- **WHEN** `ts_resolution = "10ms"` is set
- **THEN** every pushed sample's `ts` is a multiple of 10 ms

#### Scenario: Values unaffected
- **WHEN** the same configuration runs with and without `ts_jitter`
- **THEN** the n-th value of each point is the same in both runs

#### Scenario: C build refuses the keys
- **WHEN** a C build config sets `ts_jitter` on a `bench` device
- **THEN** the config is refused with a message naming `ts_jitter` and the missing support

### Requirement: Never shipped
The `bench` feature SHALL NOT be in the `tedge-dot` default features, and `TDOT_BENCH` SHALL
default to `OFF`. No release build, package or container image SHALL include the module in
either build. The C release configure step SHALL pass `-DTDOT_BENCH=OFF` explicitly. A check
run by the `lint` recipe SHALL fail when a release feature list names `bench`, or when the C
release configure step turns `TDOT_BENCH` on or does not pass the explicit `OFF`. The Rust
crates that implement the module and the harness tool SHALL set `publish = false`.

#### Scenario: Rust release list guarded
- **WHEN** `bench` is added to the `--features` list in `dist/config.yaml`
- **THEN** `just lint` fails and names the file

#### Scenario: C release build guarded
- **WHEN** `-DTDOT_BENCH=OFF` is removed from `impl/c/cross/build.sh`
- **THEN** `just lint` fails and names the file

#### Scenario: Default builds exclude it
- **WHEN** either binary is built with its defaults and a config uses `protocol = "bench"`
- **THEN** the config is refused with that build's existing unknown-protocol error
