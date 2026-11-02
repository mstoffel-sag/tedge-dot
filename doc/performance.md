# Performance harness

The harness measures what a reading costs on its way from a connector to a Cumulocity
measurement, per stage: the connector, the MQTT broker and `tedge-mapper c8y`. It drives the
real Rust and C runtimes and the host's real mapper on a private stack, with no connection to
Cumulocity, and records one JSON line per run. The goal it serves is about 100,000 readings per
second on one gateway.

## Quick start

```sh
just bench-build                    # both connectors with the bench module, the harness, the OPC UA simulator
just bench samples-noflows          # one scenario, both implementations, recorded in bench/results/
IMPL=c just bench samples-flows --rate 2000 --window 30s
just bench-baseline                 # every scenario three times per implementation, then the summary
```

The host needs `tedge`, `tedge-mapper` and `mosquitto` on the `PATH`, plus the C build's
libraries (see `impl/c/README.md`). Use thin-edge.io 2.0.2 or later; until it is released,
install it from the main channel (`install.sh --channel main`). In CI the 2.0.1 mapper did not
convert measurements on the offline stack within 60 s, so `stack up` failed its check. Nothing
runs as root, and nothing touches the host's own broker or mapper.

## How it works

**The stack** (`tedge-dot-bench stack up`, or any `run`) starts a private mosquitto and
`tedge-mapper c8y` from `bench/.stack/`, on free ports. The mapper only converts measurements
when it believes the cloud bridge is up and the device is registered, so the stack:

- sets `c8y.url` to a placeholder host and `mqtt.bridge.built_in = false`
- moves the mapper's proxy and HTTP servers to free ports
- publishes a retained `{"status":"up"}` on
  `te/device/main/service/mosquitto-c8y-bridge/status/health`
- registers each scenario's child devices with the same message the `ot-registration` flow
  sends

Offline, the mapper starts converting about 30 s after it starts, so `stack up` waits up to
60 s for its test measurement to reach `c8y/measurement/measurements/create`. A stack started
by `run` is stopped at the end; one started with `stack up` stays up across runs. `stack down`
stops only the processes the stack started, by PID.

**The `bench` module** is a connector module that generates readings from its configuration
alone, in both builds, so the runtimes are measured without a simulator in the way. Both
builds produce the same values (`doc/contract/test-vectors/bench/`). It is behind the `bench`
Cargo feature and the `TDOT_BENCH` CMake option, and `just lint` fails if a release would
include either. Its keys:

| Key | Where | Meaning |
|---|---|---|
| `model` | point `address` | `"walk"` (default), `"constant"` or `"counter"` |
| `step`, `change` | point `address` | Walk step (default 1.0); fraction of readings that change the value (default 1.0) |
| `latency`, `batch` | device `protocol_address` | Time per block of `batch` points in a read (blocking in C) |
| `push_rate` | device `protocol_address` | Pushed samples per second across the device's points; above 0, every point not set `subscribe = false` is pushed |
| `ts_jitter`, `ts_resolution`, `publish_interval` | device `protocol_address` | Source timestamps on pushed samples (generation time, moved back by up to `ts_jitter`, down to a multiple of `ts_resolution`), and delivery once per `publish_interval` |

**A run** (`tedge-dot-bench run <scenario>`) does this, in order:

1. It checks that the binary has the `bench` module, and fails naming the build to use. It
   never falls back to the other implementation.
2. It installs or removes the `ot-*` flows in the mapper's flows directory, as the scenario
   says, and registers the devices.
3. It waits until the mapper used less than 50 ms of CPU over 2 s, and fails after 60 s.
4. It writes the connector configs, starts the instances, waits for the first sample and a
   warm-up, then measures the window.
5. It stops the connectors, waits until the output topics are quiet for 2 s, and reports that
   wait as `drain_s`.

Scenarios are in `bench/scenarios.toml`. `ot-conformance serve opcua --points N` is the OPC UA
server for the OPC UA scenario.

## Reading a result

Each run prints one JSON line and appends it to `bench/results/<host>-<date>.jsonl`.
`tedge-dot-bench summary` writes the median of the last three runs per scenario, rate and
implementation to `bench/results/baseline-<host>.md`, with Rust and C side by side.

| Field | Meaning |
|---|---|
| `achieved_rate` | Readings per second the connector took, counted by each sample's own timestamp |
| `rate_shortfall_pct` | How far that falls short of the configured rate: a connector that polls late or cannot keep up |
| `missing`, `missing_pct` | Samples lost after the connector published them, from the gaps in each point's `seq` |
| `cpu_pct.*` | CPU time during the window divided by the window, per process (connector instances summed) |
| `us_per_reading.*` | CPU time per reading taken in the window, per process |
| `msgs_per_s.*`, `values_per_s.*` | Per topic class: samples, thin-edge measurements, Cumulocity measurements |
| `c8y_bytes_per_s`, `te_errors` | Cumulocity output volume, and messages the mapper rejected |
| `drain_s` | How long output kept arriving after the window: a pipeline that fell behind |

Readings are counted by their own timestamp, not by when they arrive, because a polled pass
arrives as one burst. For a polled run the counting span starts in the quiet part of the poll
cycle, after the first interval, and covers whole intervals, so the expected count is exact.

## Pitfalls

- **A mapper backlog spoils the next run.** A run that floods the mapper leaves work queued,
  and the next run would measure it. The idle wait before each run exists for this. A large
  `drain_s` means the pipeline fell behind even when the CPU figures look fine.
- **Cumulocity's 16,184-byte limit applies to the c8y size, not the thin-edge size.** The
  mapper drops larger messages (`limit-payload-size`) and reports each one on `te/errors`. A
  batch grows by about 10 bytes per series when the mapper converts it: 700 series came to
  16,087 bytes, and a 12 KB thin-edge batch was already too large.
- **One host's numbers are ratios, not another host's capacity.** Compare results only from
  one host with one set of tool versions. The summary warns when they differ.
- **The broker figure includes the observer.** The observer subscribes only to the topics a
  scenario counts, and its own CPU is reported separately.
- **Polls run on their interval's grid.** Since concurrent reads (§5.6), both runtimes
  schedule a point's next poll one interval after the one it was due at, not after the moment
  the loop got to it, so a late tick no longer pushes every later poll back. Before that, a 1 s
  poll ran every 1.0 to 1.2 s in Rust (a shortfall of about 11 %) and about every 1.045 s in C.
  A pass that takes longer than the interval still stretches it.
- **Without latency, the bench hides the cost of round trips.** A local simulator answers in
  microseconds, so a scenario without `latency` measures CPU, not how a connector copes with a
  PLC that takes 20 ms per read. `polled-latency` sets `latency` on the bench module;
  `opcua-latency` starts the OPC UA simulator with `--latency`, a proxy that delays each
  direction by half the round trip.
- **The rate shortfall of a polled run moves in whole polls.** A 20 s window at 1 s polls
  counts 17 intervals, so one poll fewer is 5.9 %. The C figure is 0, 5.9 or 11.8 % from run
  to run for the same 4 % lateness.
- **C push arrives in tick bursts, Rust push continuously.** The C runtime drains push once per
  loop tick, so the C `bench` module delivers what is owed since the last drain. Counts per
  window match, but C samples carry the drain time, so short windows show more noise in the
  rate.

## Profiling

The harness answers "how much". For "where", profile one process during a run:

```sh
# macOS
sample <pid> 10 -file /tmp/connector.txt
# Linux
perf record -g -p <pid> -- sleep 10 && perf report
```

The connector PIDs are in `bench/.stack/runs/<run>/` logs, and `ps` shows them during a run.

## Starting point

An exploration on 2026-10-28 measured the pipeline with scratch scripts before the harness
existed (M1 Max, thin-edge.io 2.0.2-rc423, local broker, no cloud). These numbers came from
throwaway tooling, so treat them as orientation only. The recorded baseline in `bench/results/`
replaces them.

| What | Cost |
|---|---|
| Mapper with the four `ot-*` sample flows | About 490 µs per reading |
| A flow that only parses its message | About 110 µs per message |
| Built-in c8y conversion, unbatched | About 92 µs per message |
| Built-in c8y conversion, 500 series per message | 1.56 µs per reading |
| Rust connector, OPC UA, per sample | 12.5 µs (allocation 36 %, `sendto` 19 %, JSON 16 %, clock reads and timeouts 10 %, OPC UA decode under 3 %) |
| One Rust OPC UA instance, 20,000 polled points | 52,700 readings/s: bound by its CPU per sample, not by sequential reads, which a local simulator never shows (see `opcua-latency`) |
| Spike: point table and one batched measurement per device | 0.33 µs per reading on the connector; 2 % of a core for the broker, 17 % for the mapper at 100,000/s |

## Reporting a performance change

An openspec change that claims a performance effect has a **Performance** section in its
`design.md`, with:

- the scenario names it was measured with
- the baseline result lines it compares against, from the same host
- its targets
- the measured after numbers, and whether each target was met

Its tasks end with running those scenarios and pasting the result.
