# Design

## Context

The exploration behind this change measured the pipeline with scripts in a session scratchpad:
a Python load generator, a Python observer, shell scripts that start mosquitto and a
`tedge-mapper`, a Python OPC UA simulator in Docker, and a Rust spike that stood in for the
connector. The numbers are in the proposal. Several problems showed up along the way, and the
harness has to handle each of them:

- **A standalone `tedge-mapper local` cannot run the built-in Cumulocity steps.**
  `into-c8y-measurements` reads entity metadata from the flow context, and only the c8y mapper
  writes it there. The real `tedge-mapper c8y` runs offline with a placeholder `c8y.url`, the
  proxy and HTTP servers on spare ports, and a retained `{"status":"up"}` on
  `te/device/main/service/mosquitto-c8y-bridge/status/health`. It then converts measurements
  to `c8y/measurement/measurements/create` and registers child devices on `c8y/s/us`.
- **A backlog spoils the next run.** A run that floods the mapper leaves a backlog, and the
  next run measures it. One run reported 240 % mapper CPU for a design that needs 17 %.
- **The c8y mapper drops oversized messages.** It drops messages above 16,184 bytes
  (`limit-payload-size`) and reports each one on `te/errors`. A batch's c8y size is larger than
  its thin-edge size (about 10 bytes more per series).
- **Python on the measured path skews the results.** A Python subscriber cannot count 100,000
  messages/s, and the Python OPC UA simulator capped a run at 68 % of its own CPU.
- **Killing processes by pattern is unsafe.** `pgrep -f`/`pkill -f` patterns matched the
  invoking shell's own command line and killed it.
- **Results are ratios between designs, not gateway capacity.** All numbers come from one M1
  Max.

The repository already has the parts to build on. `ot-conformance` embeds an async-opcua
simulator (`src/sim/opcua.rs`). The `tedge-dot` binary selects modules with Cargo features.
Release builds pass an explicit `--features` list (`dist/config.yaml`). Robot/e2e suites use
an `IMPL=rust|c` switch.

## Goals / Non-Goals

**Goals:**
- One command measures a named scenario against the real runtime and the real c8y mapper, with
  no cloud, and prints a comparable result line.
- It runs on a developer machine (macOS, Linux) and on a gateway (Linux, arm64/amd64) with the
  same code.
- Load generation and observation never bottleneck the measurement below 100,000 readings/s.
- The Rust and C runtimes are measured with the same synthetic load and the same values, so a
  follow-up change can be compared across both builds.
- There is a baseline of `main` to compare follow-up changes against.
- The `bench` module can never reach a package.

**Non-Goals:**
- Changing the runtime, the contract, the flows or the output format. Those are follow-up
  changes, and this one only measures them.
- Gating CI on CPU or throughput.
- Measuring the Cumulocity cloud, the mosquitto bridge's TLS uplink, or tenant ingestion.
- A throughput target for the C build. It is measured and baselined like the Rust build, but
  the 100,000/s target applies to Rust.

## Decisions

### D1. Measure the real `tedge-mapper c8y`, with only the bridge faked
The mapper is where today's cost is (about 490 µs per reading with the flows, about 92 µs per
unbatched message without them), so it must be the real binary with the real flows directory.
Faking the bridge health is the smallest change that makes it run offline. The `ot-*` flows,
when a scenario includes them, go into the c8y mapper's `flows/` directory, where packages put
them.
- *Alternative: a stub converter.* It would measure the stub, not thin-edge.
- *Alternative: a Docker stack.* On macOS the VM adds noise, and a gateway may not run Docker.
  The harness uses the host's `tedge`, `tedge-mapper` and `mosquitto`, and records their
  versions.

### D2. One Rust tool, `tedge-dot-bench`, instead of shell plus Python
A new crate `impl/rust/crates/bench`, never published or packaged, builds a `tedge-dot-bench`
binary with these subcommands:

- `stack up|down`: writes a private config dir, starts mosquitto on a free port and
  `tedge-mapper c8y` against it, publishes the bridge health and registers devices. It tracks
  the children by PID, never by pattern.
- `gen-config`: writes connector configs for a scenario.
- `observe`: subscribes to the output topics, counts messages and the values inside them, and
  counts `te/errors`.
- `run <scenario>`: does everything above and prints the result line.

Rust keeps the observer fast enough at 100,000 messages/s, uses one process API on macOS and
Linux, and avoids the shell pitfalls found during the exploration.
- *Alternative: keep the scratchpad scripts.* They worked for 2,000–10,000 msg/s, but the
  observer and the generator were the bottleneck in the high-rate runs.

### D3. CPU from the OS per process, sampled at the window edges
`run` reads each PID's cumulative user and system time at the start and end of the measured
window: `/proc/<pid>/stat` on Linux, `proc_pidinfo` on macOS. CPU % = Δcpu / Δwall. µs per
reading = Δcpu / readings in the window. Three processes are reported (connector, broker,
mapper), and the observer's own CPU is reported separately so it can be told apart.
- *Alternative: `perf`/`sample` profiles.* They answer "where" rather than "how much", and
  they need privileges. Profiling stays a manual step, and `doc/performance.md` describes it.

### D4. Each run is isolated: idle before, quiet after
Before a run, `run` waits until the mapper used less than 50 ms of CPU over 2 s. A timeout
fails the run, so a run never starts on a backlog. After the measured window, it waits until
the output topics have been quiet for 2 s and reports that wait as `drain_s`. A large
`drain_s` means the pipeline fell behind even when the CPU figures look fine.

### D5. Result line: JSON Lines with enough context to compare
Each run prints one JSON object and appends it to `bench/results/<host>-<date>.jsonl`. It
records:

- the schema version, scenario name and parameters, `IMPL`
- the git SHA and whether the tree has uncommitted changes
- the host's CPU model, core count and OS, and the versions of `tedge`, `mosquitto` and
  `tedge-dot`
- the target and achieved readings/s, values out/s and messages/s per topic class
- CPU % and µs per reading per process
- c8y messages/s and bytes/s, `te/errors`, `drain_s`
- missing values (expected minus observed, see D7)

Results are only comparable on the same host and tool versions, and the summary tool says so
when they differ.

### D6. The `bench` connector module, in both builds
Both builds get the module, with protocol id `bench`, the same config keys and the same values:

- **Rust:** a crate `connector-bench` behind the `bench` feature implements `Connector`.
- **C:** `impl/c/connectors/bench/connector_bench.c` behind the CMake option `TDOT_BENCH`
  (default `OFF`, which defines `TDOT_FEATURE_BENCH`) implements `tdot_connector_t`.

The follow-up changes (`native-batched-output` and the others) change both runtimes under the
parity rule, so both have to be measurable with the same synthetic load. The C build is also
where the last load test found the worst costs.

**Values are defined by one algorithm and shared vectors.** Each point has a stream seeded with
FNV-1a 64 over the bytes `<device>\0<point id>` (a zero seed becomes 1), advanced by
xorshift64*. A draw `u = (x >> 11) · 2⁻⁵³` lies in [0, 1). Per reading, a first draw decides
whether the reading changes the value (`u < change`), and a second draw gives the `walk` step
`(2u − 1) · step`.

- `int32` uses the walk rounded half away from zero.
- `bool` flips on each change.
- `string` is the decimal text of the change counter.

`doc/contract/test-vectors/bench/` holds the first 1,000 values of several (device, point,
model, datatype, step, change) cases. The Rust and C unit tests both check them for bit-exact
equality. The C file is compiled with `-ffp-contract=off`, so a fused multiply-add cannot
change the last bit of a `walk` value.

- **Point `address`:**
  - `model`: `"walk"` (default; each reading adds a step in [−step, +step]), `"constant"` or
    `"counter"`
  - `step`
  - `change`: the fraction of readings that change the value (default 1.0)
- **Polled:** `read_points` returns one sample per point straight away, or after the device's
  `latency` (a duration per call, default 0). It returns at most `batch` points per call, so a
  latency per block can be modelled.
- **Pushed:** a point with `subscribe = true` is delivered by push. The device's `push_rate`
  is changes per second across its pushed points, spread evenly by a generator task feeding
  the `SampleSink`. `check_subscription` reports healthy.
- **Datatypes:** `float64`, `int32`, `bool` and `string`, so value-type costs can be measured.
- **Not supported:** writes and methods (`Unsupported` in Rust, the trait default; NULL
  `execute` hooks in C).

**How push maps onto each runtime.** Rust runs a generator task that feeds the `SampleSink`
continuously. The C runtime is single-threaded and drains push once per main-loop tick, so the
C module's `drain_subscriptions` delivers the samples owed since the last drain:
`push_rate × elapsed`, with the remainder carried to the next drain. Over a window both builds
deliver the same count. C delivers them in bursts of one tick, because that is how the C
runtime works. Comparing the two is part of what the harness should show.
`subscribe_device` sets `pt->subscribed` on the accepted points. The C module has no transport,
so its health check never fails.

**Pushed samples carry source timestamps (Rust).** Batching pushed values depends on how often
their timestamps coincide, so the harness has to control it. The device `protocol_address`
takes three more keys, used by pushed points only:

- `ts_jitter`, a duration (default 0): each pushed sample's `ts` is its generation time minus a
  uniform draw from [0, `ts_jitter`], the way an OPC UA source time precedes delivery.
- `ts_resolution`, a duration of at least 1 ms (default 1 ms, the contract's precision, §9):
  `ts` is floored to it. A coarse resolution makes timestamps coincide more often.
- `publish_interval`, a duration (default 0, continuous): samples are held and delivered
  together once per interval, each with its own `ts`, the way an OPC UA subscription delivers
  one notification per publishing interval.

The draws come from a per-device stream (FNV-1a 64 over `<device>\0#ts`), so the values stream
is unchanged by these keys. Timestamps are wall-clock based and are not part of the shared
vectors.

The C sample type has no timestamp field today: the C runtime stamps every sample when it
publishes it, and the C OPC UA module drops the source time that the Rust one uses. Adding the
field changes the C SDK, which this change doesn't do. The C module therefore refuses the three
keys with a message that names the missing support, and the `samples-push-jitter` scenario runs
for Rust only. `native-batched-output` adds the field and extends the C module and the
scenario.

**Latency in C blocks.** `read_points` sleeps (`nanosleep`) for each block. That matches a real
blocking protocol call in the serial C loop, and the `samples-latency` scenario then shows the
cost of serial reads in both builds.

Modelling latency per call and per block (instead of only having an instantly answering
source) is what lets the concurrent-device-reads change show its gain. Today's runtime reads
devices one after another, and the exploration measured that limit with OPC UA.

### D7. Rate shortfall and loss are separate measurements
Each scenario fixes devices × points × poll interval, or the push rate, so the expected
readings in a window are known. The observer counts readings by each sample's own timestamp,
not by arrival: a polled pass arrives as one burst, so arrival counting is off by up to a whole
poll. For a polled run the counting span starts in the quiet part of the poll cycle, after the
first interval, and covers a whole number of intervals, so a pass is never cut in two.

The first runs showed two different things a single "missing" number would mix up. Both
runtimes schedule the next poll from the time it ran (`next_due = now + interval`), so every
wake-up that comes after the due time is added to the interval, and every sample is still
delivered. Rust checks for due points on a 200 ms tick, so a 1 s poll runs every 1.0–1.2 s,
about 11 % fewer readings than configured. C wakes whenever its MQTT socket has traffic, so a
1 s poll runs about every 1.045 s, about 4 % fewer. The C figure shows as 0, 5.9 or 11.8 %,
because a 20 s window counts 17 whole intervals.
So the result reports:

- `rate_shortfall_pct`: expected minus observed readings, as a share of expected. Baselined,
  never gated.
- `missing`: samples lost after publishing, from the gaps in each point's `seq` between the
  first and last sample received. The CI smoke gates on it.

Tail losses after the last received sample are not counted. `drain_s` covers that case.

### D8. A standalone OPC UA simulator: `ot-conformance serve opcua`
`serve opcua --points N [--change-rate R] [--port P]` runs the existing embedded simulator
with N bulk `Double` nodes `ns=2;s=Bulk<i>`, without the conformance checks. It advertises its
own port, so no proxy is needed. It replaces the Python simulator for throughput scenarios. The
Python simulator stays for the e2e suites.

### D9. The `bench` module can never ship, in either build
- Rust: `bench` is not in `default`, and `dist/config.yaml` keeps its explicit list.
- C: `TDOT_BENCH` defaults to `OFF`. The release configure step (`impl/c/cross/build.sh`)
  passes `-DTDOT_BENCH=OFF` explicitly, so a cached `ON` in a reused build directory can't leak
  into a release.
- `ci/check-release-features.sh` fails if `dist/config.yaml` or a Dockerfile's feature list
  names `bench`, or if `impl/c/cross/build.sh` turns `TDOT_BENCH` on or leaves out the
  explicit `OFF`. The `lint` recipe runs it.
- Neither binary lists its modules in `--version`. A default build refuses a `bench` config
  with its existing error (Rust: `protocol 'bench' is not compiled in`; C:
  `unknown protocol 'bench'`), and the harness relies on that check (D10).
- `connector-bench` and the bench tool crate set `publish = false`.

### D10. Scenarios live in `bench/scenarios.toml`
A scenario names its connector setup (generator parameters, or a fixed config such as the OPC
UA one), whether the `ot-*` flows are installed, the rate, the window and the `IMPL`s it runs.
Every baseline scenario runs with `IMPL=rust` and `IMPL=c`, using the same generated config:

| Scenario | Setup | Rates (readings/s) |
|---|---|---|
| `samples-flows` | bench, polled, `ot-*` flows installed | 1k, 2k, 4k |
| `samples-noflows` | bench, polled, no flows | 10k, 50k, 100k |
| `samples-push` | bench, pushed, no flows | 10k, 50k, 100k |
| `samples-push-jitter` | bench, pushed, no flows, `ts_jitter` 50 ms, `publish_interval` 100 ms, `ts_resolution` 1 ms and 10 ms (Rust only, see D6) | 10k, 100k |
| `samples-latency` | bench, polled, 2 ms per 500-point block, 1 vs 4 instances | 50k |
| `opcua-polled` | Rust OPC UA sim, 20k points, 1 s and 200 ms poll | 20k, 100k target |

A scenario can restrict its implementations with `impls = ["rust"]`, and the baseline lists the
restriction and its reason. Only `samples-push-jitter` uses it, until `native-batched-output`
lifts it.

A run with an `IMPL` whose binary was not built with the module fails at once with the build
command to use. It doesn't fall back to the other build: the e2e suites' fallback guard
exists for the same reason.

`just bench <scenario>` runs one scenario. `just bench-baseline` runs the set three times and
writes the median of each metric to `bench/results/baseline-<host>.md`, next to the raw lines.

### D11. CI smoke checks correctness only
A workflow job on `ubuntu-latest` installs thin-edge.io and mosquitto, builds both binaries
with the module (`--features bench`, `-DTDOT_BENCH=ON`), and runs `samples-flows` and
`samples-noflows` at 1,000 readings/s for 10 s with each `IMPL`. It fails on any
`te/errors`, missing values above 0.1 %, or `drain_s` above 5 s. CPU numbers and the rate
shortfall are printed but never compared. The job runs when `impl/rust/crates/sdk`, the flows or `bench/` change.

### D12. How performance changes report results
`doc/performance.md` sets the rule: a change that claims a performance effect has a
**Performance** section in its `design.md` with:

- the scenario names
- the baseline lines it compares against (same host)
- the targets
- the measured after numbers

Its tasks end with running those scenarios and pasting the result. The exploration's numbers go
into the doc as the historical starting point, marked as coming from scratch tooling.

## Risks / Trade-offs

- **[The host's thin-edge version changes the result]** → Every result records versions, and
  the summary refuses a silent comparison across versions. `doc/performance.md` names the
  version used for the baseline.
- **[The observer adds broker fan-out]** → It subscribes only to the topics that a scenario
  counts. It reports its own CPU. The broker figure includes delivery to one extra client, and
  the doc states this.
- **[The faked bridge leaves out the uplink cost]** → Out of scope here (Non-Goals). It can be
  added later as a scenario with a local TLS bridge.
- **[Noise from thermal limits or other load]** → Three repeats and the median. The idle check
  before each run. The doc recommends a quiet machine and a fixed CPU governor on Linux.
- **[Large configs: 100,000 points make a TOML of about 600,000 lines]** → `gen-config` writes
  them, and connector startup is recorded separately from the window, so the load time doesn't
  pollute the steady state.
- **[The two builds' value streams drift apart]** → Shared vectors checked bit for bit in both
  unit test suites. `-ffp-contract=off` on the C file. If the streams differed, values out and
  deadband results would differ between the builds even when both runtimes were correct.
- **[C push arrives in bursts, Rust's is continuous]** → Intended: it is how each runtime
  delivers push. Counts per window match, and `doc/performance.md` explains why the latency
  profiles differ.
- **[A real connector's latency doesn't match the `bench` module's]** → The OPC UA scenario
  against the Rust simulator is the protocol-level check. `bench` is the runtime-level one.
- **[Running `tedge-mapper c8y` offline may break with a thin-edge update]** → The stack
  checks that a test measurement reaches `c8y/measurement/measurements/create` before any run,
  and fails with a clear message.

## Migration Plan

Additive. Nothing changes for users or packages. Rollback is removing `bench/`, the two crates,
the C module and its CMake option, the vectors, the `serve` subcommand, the recipes and the CI
job.

## Open Questions

- Which gateway hardware becomes the reference for absolute numbers, for example a Raspberry Pi
  5 or a specific industrial ARM box? The M1 baseline only gives ratios.
- Should a later scenario include the mosquitto bridge with TLS to a local endpoint, so the
  uplink cost is part of the total?
