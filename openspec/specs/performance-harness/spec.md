# performance-harness Specification

## Purpose
A repeatable, offline performance harness that drives the connector and the thin-edge.io mapper with no cloud and reports per-stage cost as one JSON result line per run.
## Requirements
### Requirement: Offline bench stack
`tedge-dot-bench stack up` SHALL start a private mosquitto on a free port and the host's real
`tedge-mapper c8y` against it, from a private configuration directory, with no connection to
Cumulocity. It SHALL make the mapper convert measurements: it uses a placeholder `c8y.url`,
puts the mapper's proxy and HTTP servers on free ports, publishes a retained
`{"status":"up"}` on `te/device/main/service/mosquitto-c8y-bridge/status/health`, and
registers the scenario's child devices. Before reporting the stack ready, it SHALL check that
a test measurement reaches `c8y/measurement/measurements/create`. `stack down` SHALL stop only
the processes it started, identified by process id.

#### Scenario: Stack converts measurements offline
- **WHEN** `stack up` completes and a measurement is published on `te/device/plc0///m/bench`
- **THEN** a Cumulocity measurement for `plc0` appears on `c8y/measurement/measurements/create`, and no network connection to a Cumulocity host is attempted

#### Scenario: Conversion check fails clearly
- **WHEN** the installed `tedge-mapper c8y` does not emit the test measurement within 60 s (offline, the mapper starts converting about 30 s after it starts)
- **THEN** `stack up` exits non-zero and names the check that failed and the mapper log path

#### Scenario: Teardown is scoped
- **WHEN** another mosquitto and another `tedge-mapper` run on the host and `stack down` is called
- **THEN** only the processes started by `stack up` are stopped

### Requirement: Isolated runs
`tedge-dot-bench run` SHALL wait, before the measured window, until the mapper has used less
than 50 ms of CPU over 2 s. It SHALL fail the run if that does not happen within 60 s. After
the window, it SHALL wait until the counted output topics are quiet for 2 s and report that
wait as `drain_s`.

#### Scenario: Backlog is not measured
- **WHEN** a previous run left the mapper busy
- **THEN** the next run starts its window only after the mapper is idle

#### Scenario: Falling behind is visible
- **WHEN** the pipeline delivers its last output 8 s after the window ends
- **THEN** the result reports `drain_s` of about 8

### Requirement: Result line
Each run SHALL print one JSON object and append it to `bench/results/<host>-<date>.jsonl`. The
object SHALL contain:

- a result schema version, the scenario name and its parameters, `IMPL`
- the git SHA and a dirty flag
- the host's CPU model, core count and OS, and the `tedge`, `mosquitto` and `tedge-dot`
  versions
- the target and achieved readings/s, values out/s and messages/s per topic class
- CPU % and µs per reading for the connector, the broker and the mapper, and the observer's own
  CPU %
- the expected and observed readings, the rate shortfall and missing values
- Cumulocity messages/s and bytes/s, the `te/errors` count and `drain_s`

#### Scenario: Complete result
- **WHEN** a scenario completes
- **THEN** its result line contains every field listed above, and CPU % equals the process's CPU time during the window divided by the window's wall time

#### Scenario: Comparison across versions is flagged
- **WHEN** the summary tool compares two results with different `tedge` versions or hosts
- **THEN** it prints the comparison with a warning naming the differing fields

### Requirement: Rate shortfall and missing values
The expected readings in a window SHALL be computed from the scenario (devices × points ×
polls, or the push rate), excluding the first and last poll interval of the window. Observed
readings SHALL be counted by each sample's own timestamp, not by arrival. The rate shortfall
SHALL be the expected readings minus the observed ones, as a percentage of the expected: it
shows a connector that reads late or cannot keep up. Missing values SHALL be the samples lost
after the connector published them: for each point, the gaps in its `seq` between the first
and last sample received. The two SHALL be reported separately, and only missing values SHALL
count as loss.

#### Scenario: Nothing lost
- **WHEN** a polled scenario of 10 devices × 100 points at 1 s runs a 12 s window and the connector polls on time
- **THEN** the expected readings are 10,000, the rate shortfall is 0 and missing values are 0

#### Scenario: Late polls are a shortfall, not a loss
- **WHEN** a connector configured to poll every 1 s polls every 1.1 s and every sample arrives
- **THEN** the result reports a rate shortfall of about 9 % and 0 missing values

#### Scenario: Lost samples are missing
- **WHEN** the broker drops 50 of a point's samples between the first and the last one received
- **THEN** the result reports 50 missing values

### Requirement: Observer does not limit the measurement
The observer SHALL count at least 200,000 messages/s on the reference host without losing any
of them. It SHALL subscribe only to the topics the scenario counts. Its own CPU SHALL be
reported separately and SHALL NOT be included in the connector, broker or mapper figures.

#### Scenario: High-rate counting
- **WHEN** the `samples-noflows` scenario runs at 100,000 readings/s
- **THEN** the observer counts every sample published, and its CPU appears only in its own field

### Requirement: Named scenarios and baseline
`bench/scenarios.toml` SHALL define the scenarios `samples-flows`, `samples-noflows`,
`samples-push`, `samples-push-jitter`, `samples-latency`, `opcua-polled`, `native-polled`,
`native-flows` and `native-push-jitter`, each with its connector setup, whether the `ot-*` flows
are installed in the mapper's flows directory, rates and window. A scenario MAY set
`output = "measurements"`, a list of `group_by` policies (`"time"`, `"frame:<window>"`,
`"sample:<every>"`, one run each) and an `alarm_share` of points that declare an alarm. Every
baseline scenario SHALL run with `IMPL=rust` and `IMPL=c` from the same generated
configuration, unless the scenario restricts its implementations with `impls`, which the
baseline SHALL list with its reason. `just bench <scenario>` SHALL run one scenario, for both
implementations unless `IMPL` is set. `just bench-baseline` SHALL run every baseline scenario
three times per implementation and write the median of each metric to
`bench/results/baseline-<host>.md`, with Rust and C side by side, next to the raw result lines.
The repository SHALL contain a baseline of `main` for both implementations from at least one
host.

In measurement output a run SHALL count readings by the values inside the thin-edge
measurements, by each measurement's own `time`, SHALL report the values per measurement
message, and SHALL report as missing the values that did not reach Cumulocity.

#### Scenario: Baseline run
- **WHEN** `just bench-baseline` runs on a host with the stack's tools installed
- **THEN** each baseline scenario runs three times per implementation and the markdown summary lists the median of each metric per scenario, rate and implementation

#### Scenario: Both implementations measured
- **WHEN** `just bench samples-noflows` runs without `IMPL` set
- **THEN** it produces result lines with `IMPL=rust` and with `IMPL=c` for the same configuration and rates

#### Scenario: Missing module fails fast
- **WHEN** a scenario runs with `IMPL=c` and the C binary was built without `TDOT_BENCH`
- **THEN** the run fails before the window, naming the CMake option to enable, and does not fall back to the Rust binary

#### Scenario: Measurement output counted
- **WHEN** `native-polled` runs at 100,000 readings/s
- **THEN** the result line counts the readings from the measurement values, reports values per message, and reports as missing any value the mapper did not publish to Cumulocity

### Requirement: Standalone OPC UA simulator
`ot-conformance serve opcua --points N` SHALL run the embedded OPC UA simulator with N `Double`
nodes `ns=2;s=Bulk0` … `ns=2;s=Bulk<N-1>`, anonymous with security `None`, advertising the
endpoint it listens on. `--change-rate R` SHALL change R nodes per second, spread evenly.

#### Scenario: Bulk nodes readable
- **WHEN** `serve opcua --points 20000 --port 48400` runs and the connector reads `ns=2;s=Bulk19999`
- **THEN** the read returns `19999.0` with good quality

### Requirement: CI smoke checks correctness only
A CI job SHALL install thin-edge.io and mosquitto, build the Rust binary with the `bench`
feature and the C binary with `TDOT_BENCH=ON`, and run `samples-flows` and `samples-noflows` at
1,000 readings/s for 10 s with each implementation. It SHALL fail on
any `te/errors` message, on more than 0.1 % missing values, or on `drain_s` above 5 s. It SHALL
NOT fail on CPU, throughput or rate shortfall figures.

#### Scenario: Rejected message fails the smoke
- **WHEN** the mapper publishes one message on `te/errors` during the smoke run
- **THEN** the CI job fails and prints the error

#### Scenario: Slow runner does not fail the smoke
- **WHEN** the runner's CPU figures are twice the baseline's but no value is missing
- **THEN** the CI job passes

### Requirement: Performance changes report against the harness
`doc/performance.md` SHALL describe how to run the harness and how to read a result. It SHALL
list the known pitfalls: a mapper backlog spoils the next run, Cumulocity's 16,184-byte message
limit applies to the c8y size rather than the thin-edge size, and results from one host are
ratios, not another host's capacity. It SHALL state the rule that an openspec change claiming a
performance effect includes a **Performance** section in its `design.md`, with the scenarios,
the baseline lines compared against from the same host, the targets and the measured after
numbers.

#### Scenario: Follow-up change cites the harness
- **WHEN** a change such as `native-batched-output` claims lower mapper CPU
- **THEN** its `design.md` names the scenarios, quotes baseline and after result lines from the same host, and states whether each target was met
