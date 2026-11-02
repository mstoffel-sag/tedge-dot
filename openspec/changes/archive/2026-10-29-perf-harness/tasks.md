## 1. Shared value algorithm and vectors

- [x] 1.1 Write the value algorithm (FNV-1a 64 seed over `<device>\0<point id>`, xorshift64*, `u = (x >> 11) · 2⁻⁵³`, change draw then step draw, the per-datatype rules) in `doc/contract/test-vectors/bench/README.md`
- [x] 1.2 Generate `doc/contract/test-vectors/bench/values.json`: the first 1,000 values for cases covering every model, every datatype, `change` of 1.0, 0.2 and 0, and a non-ASCII point id

## 2. Rust `bench` module

- [x] 2.1 Create `impl/rust/crates/connector-bench` (`publish = false`) with the config model: point `address` (`model`, `step`, `change`) and device `protocol_address` (`latency`, `batch`, `push_rate`), validated with errors that name the place
- [x] 2.2 Implement the value streams and a unit test that matches `values.json` bit for bit, plus the change-ratio test
- [x] 2.3 Implement `read_points` with `latency` per `batch` block, and `connect`, `disconnect` and `capabilities`; writes and methods stay `Unsupported`
- [x] 2.4 Implement `subscribe` with a generator task delivering `push_rate` samples/s spread evenly, plus `pushes_point` and `check_subscription`; a test checks the rate within ±5 %
- [x] 2.5 Implement pushed source timestamps: `ts_jitter`, `ts_resolution` and `publish_interval` from a per-device stream, with tests for the jitter bound, the resolution, grouped delivery and unchanged values
- [x] 2.6 Add the `bench` feature to `impl/rust/Cargo.toml` (not in `default`) and register `bench` in the protocol selection in `impl/rust/src/main.rs`

## 3. C `bench` module

- [x] 3.1 Add `option(TDOT_BENCH ... OFF)` to `impl/c/CMakeLists.txt` (defines `TDOT_FEATURE_BENCH`), compile `impl/c/connectors/bench/connector_bench.c` with `-ffp-contract=off`, and register `bench` in the protocol selection in `impl/c/src/main.c`
- [x] 3.2 Implement config validation with the same keys and messages as the Rust module (a parity test feeds both the same invalid configs), and refuse `ts_jitter`, `ts_resolution` and `publish_interval` with a message naming the missing support
- [x] 3.3 Implement the value streams and a test in `impl/c/tests/` that matches `values.json` bit for bit
- [x] 3.4 Implement `read_points` with a blocking `nanosleep` of `latency` per `batch` block, plus connect, disconnect and the descriptor hooks; `execute` hooks stay NULL
- [x] 3.5 Implement `subscribe_device` (sets `pt->subscribed`) and `drain_subscriptions` delivering `push_rate × elapsed` with the remainder carried over, plus the subscription health hook; a test checks the rate within ±5 % at 10 ms and 200 ms ticks

## 4. Release guard

- [x] 4.1 Pass `-DTDOT_BENCH=OFF` explicitly in `impl/c/cross/build.sh`
- [x] 4.2 Add `ci/check-release-features.sh`: it fails if `bench` appears in `dist/config.yaml` or a Dockerfile feature list, or if `impl/c/cross/build.sh` turns `TDOT_BENCH` on or leaves out the explicit `OFF`; run it from the `lint` recipe
- [x] 4.3 Check that both default builds refuse `protocol = "bench"` with their existing unknown-protocol error

## 5. Standalone OPC UA simulator

- [x] 5.1 Add `ot-conformance serve opcua --points N [--change-rate R] [--port P]`, reusing `src/sim/opcua.rs` without the proxy and advertising the listening endpoint
- [x] 5.2 Test: both connectors read `ns=2;s=Bulk<N-1>` from `serve opcua --points N` and get `N-1` with good quality

## 6. Harness tool `tedge-dot-bench`

- [x] 6.1 Create `impl/rust/crates/bench` (`publish = false`, binary `tedge-dot-bench`) with subcommands `stack`, `gen-config`, `observe`, `run` and `summary`
- [x] 6.2 `stack up`: private config dir, mosquitto on a free port, `tedge-mapper c8y` with placeholder `c8y.url` and free proxy/HTTP ports, retained bridge health, child-device registration, and the test-measurement check; `stack down` by tracked process id
- [x] 6.3 `gen-config`: write connector configs for devices × points, poll interval, push rate, latency and batch, split over N instances; the same files serve both implementations
- [x] 6.4 Per-process CPU sampling: `/proc/<pid>/stat` on Linux and `proc_pidinfo` on macOS, with a unit test on the running process
- [x] 6.5 `observe`: subscribe to the scenario's topic classes (samples, `m/`, Cumulocity measurements, `te/errors`), count messages and the values inside them, track the time of the last message; check that it counts 200,000 msg/s without loss
- [x] 6.6 `run`: select the binary by `IMPL` (Rust or C), check it has the `bench` module before the window and fail without falling back, idle wait (fails after 60 s), window measurement, `drain_s`, expected and observed readings, rate shortfall and missing values (gaps in each point's `seq`), and the result line printed and appended to `bench/results/<host>-<date>.jsonl`
- [x] 6.7 Install or remove the `ot-*` flows (from `flows/`, with their `params.toml` templates) in the stack mapper's flows directory per scenario
- [x] 6.8 `summary`: median of three runs per scenario, rate and implementation into `bench/results/baseline-<host>.md`, Rust and C side by side, with a warning when hosts or tool versions differ

## 7. Scenarios and recipes

- [x] 7.1 Write `bench/scenarios.toml` with `samples-flows`, `samples-noflows`, `samples-push`, `samples-push-jitter` (`impls = ["rust"]`), `samples-latency` (1 vs 4 instances) and `opcua-polled`, each run for both implementations unless restricted, and make the runner and `summary` honor and list `impls`
- [x] 7.2 Add `just bench <scenario>` (both implementations unless `IMPL` is set), `just bench-baseline`, `just bench-smoke`, `just bench-stack-up|down`, and build recipes for both binaries with the module (`--features bench`, `-DTDOT_BENCH=ON`)
- [x] 7.3 Check each scenario end to end on the developer machine for both implementations: zero `te/errors`, missing values at or under 0.1 % where the pipeline keeps up, the rate shortfall recorded, and Rust results that match the exploration's numbers in order of magnitude (e.g. `samples-flows` mapper ≈ 400–600 µs per reading)

## 8. Baseline and documentation

- [x] 8.1 Run `just bench-baseline` on `main` for both implementations and commit the raw `.jsonl` and `baseline-<host>.md`
- [x] 8.2 Write `doc/performance.md`: the stack and its offline trick, how to run and read a result, the pitfalls (mapper backlog, 16,184-byte limit on the c8y size, one host's numbers are ratios), why C push arrives in tick bursts and Rust push continuously, manual profiling (`sample` on macOS, `perf` on Linux), the exploration's numbers as the starting point, and the **Performance** section rule for changes
- [x] 8.3 Add the harness to the layer table in `doc/testing.md`

## 9. CI smoke

- [x] 9.1 Add a workflow job on `ubuntu-latest`: install thin-edge.io and mosquitto, build both binaries with the module, run `just bench-smoke` (`samples-flows` and `samples-noflows` at 1,000 readings/s for 10 s, both implementations), and fail on `te/errors`, more than 0.1 % missing (lost) samples or `drain_s` above 5 s
- [x] 9.2 Trigger the job on changes under `impl/rust/crates/sdk`, `impl/c/sdk`, `flows/`, `bench/` and the bench modules, and print the result lines in the job summary
