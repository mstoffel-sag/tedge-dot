## MODIFIED Requirements

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
