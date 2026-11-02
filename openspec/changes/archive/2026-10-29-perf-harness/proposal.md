# Proposal

## Why

The goal is to read about 100,000 points per second on one gateway. An exploration on
2026-10-28 (M1 Max, tedge 2.0.2-rc423, local broker, no cloud) put today's pipeline at a few
thousand readings per second end to end:

- **Mapper.** The `ot-*` flows cost about 490 µs of mapper CPU per reading. A flow that only
  parses its message costs about 110 µs, and the built-in Cumulocity conversion costs about
  92 µs per unbatched message, so the cost is per message, not per JavaScript line.
- **Connector.** The Rust connector costs 12.5 µs per sample. Allocation takes 36 % of that,
  one `sendto` per MQTT message 19 %, the JSON envelope 16 % and clock reads and publish
  timeouts 10 %. OPC UA decoding takes less than 3 %.
- **Reads.** One connector instance read 52,700 samples/s from 20,000 polled OPC UA points. It
  is bound by sequential reads, not by output. Four instances reached 78,000/s.
- **Batched output.** A throwaway spike that writes readings into a point table and publishes
  one batched measurement per device needed 0.33 µs per reading on the connector side, 2 % of
  a core on the broker and 17 % on the mapper at 100,000 readings/s. A vectorizable deadband
  pass gave no gain.

These numbers came from scripts in a session scratchpad. They are gone, they ran against a
stand-in rather than the real runtime, and the Python OPC UA simulator capped one run at 68 %
of its own CPU. The follow-up changes (native batched output, concurrent device reads, sampled
push) each claim a performance gain. Without a harness in the repository, nobody can check
those claims, compare before and after, or repeat a measurement on a real gateway.

## What Changes

- **A local bench stack** (`tedge-dot-bench stack`). It starts a private mosquitto and the real
  `tedge-mapper c8y` with no cloud connection: a placeholder `c8y.url`, the proxy and HTTP
  servers on spare ports, a retained bridge health message, and registered child devices. It
  uses the thin-edge.io and mosquitto installed on the host, so the same stack runs on a
  developer machine and on a gateway.
- **A harness runner.** It waits until the mapper is idle, runs one scenario for a fixed
  window, and prints one JSON result line. The line holds readings/s, values out, messages/s,
  CPU % and µs per reading for the connector, the broker and the mapper, Cumulocity messages
  and bytes, and the `te/errors` count.
- **A synthetic `bench` connector module in both builds.** It generates readings at a
  configured rate and shape: polled or pushed, devices × points, change ratio, and an optional
  per-read latency. In the Rust build, pushed samples can carry jittered source timestamps
  delivered once per publishing interval, so the effect of timestamps on batching can be
  measured. The harness then drives the real Rust and C runtimes with no simulator in
  the way. Both builds produce the same values from the same config, checked against shared
  test vectors. The module is behind the `bench` Cargo feature (Rust) and the `TDOT_BENCH`
  CMake option (C). Neither is on by default or in a release build, and a check fails if a
  release would include either one.
- **A standalone OPC UA simulator.** A `serve` subcommand of `ot-conformance` runs its existing
  async-opcua simulator with N bulk nodes. It replaces the Python simulator for OPC UA
  throughput scenarios.
- **Baseline scenarios for today's pipeline:** per-point samples with and without the `ot-*`
  flows, polled and pushed `bench` points, and polled OPC UA against the Rust simulator. Each
  scenario runs at several rates, with `IMPL=rust` and `IMPL=c`.
- **A recorded baseline** from `main` in `bench/results/`, and `doc/performance.md` explaining
  the method, the pitfalls, and the rule that a performance change reports before and after
  numbers and its pass targets in its `design.md`.
- **A CI smoke job** that runs the harness at a low rate and fails on rejected messages or
  missing values. It does not judge CPU or throughput, because shared runners are too noisy.

## Capabilities

### New Capabilities
- `performance-harness`: the bench stack, the runner and its result format, the scenarios, the
  recorded baseline, the CI smoke, and the reporting rule for performance changes.
- `bench-connector`: the synthetic `bench` protocol module: its configuration, the readings it
  generates, its push and latency behavior, and the rule that it never ships.

### Modified Capabilities
<!-- None: no existing requirement changes. The connector runtime, the contract and the flows
     are measured, not changed. -->

## Impact

- **New:** `bench/` (scenarios and results), `impl/rust/crates/bench` (the `tedge-dot-bench`
  tool: stack, config generator, observer, runner), `impl/rust/crates/connector-bench`, a
  `bench` feature on the `tedge-dot` binary, `impl/c/connectors/bench` with the `TDOT_BENCH`
  CMake option (default `OFF`), shared value vectors in `doc/contract/test-vectors/bench/`, a
  `serve` subcommand in `ot-conformance`, `doc/performance.md`, `just bench-*` recipes and a CI
  job.
- **Packaging:** none. `dist/config.yaml` keeps its explicit feature list and the C release
  build (`impl/c/cross/build.sh`) passes `-DTDOT_BENCH=OFF` explicitly. A new check makes
  the build fail if a release build would include `bench` or `TDOT_BENCH`.
- **Dependencies:** none in the product. The harness needs `tedge`, `tedge-mapper` and
  `mosquitto` on the host. It records their versions in each result.
- **C build:** a new `bench` module behind a CMake option that is off by default. Every
  scenario runs both binaries through the same `IMPL` switch the e2e suites use, so follow-up
  changes, which have to change both runtimes, get measured in both. The 100,000/s target
  applies to the Rust build. The C build is measured and its baseline recorded, but it has no
  throughput target.
- **Follow-ups this unblocks:** `native-batched-output` (targets: connector ≤ 1 µs per reading,
  mapper ≤ 25 % of a core at 100,000/s, zero rejected messages), concurrent device reads, and
  sampled push.
