## 1. Contract and vectors

- [x] 1.1 Update contract §5 (`ts` rule, `measured`), §3 (`output`, `measurement`, `batch`, `sample`) and add a section on measurement output and grouping policies; bump the contract minor version
- [x] 1.2 Write `doc/contract/test-vectors/measurements/`: readings and policy in, expected messages, splits and units metadata out, covering all three policies, repeated points, bad quality, booleans, strings, `meta.measurement = false`, long and non-ASCII names, and a split at the size budget
- [x] 1.3 Add `output`, `measurement`, `batch` and `sample` to the config JSON schema and `measured` to the sample schema

## 2. Source and read time

- [x] 2.1 Rust: add `source_ts` to `Sample`; the runtime sets `ts` once per `read_points` call and per sink delivery; envelope `ts` uses `source_ts` when set
- [x] 2.2 Rust: move the OPC UA data value timestamp into `source_ts`, and stop the other modules stamping `ts` (audit every `connector-*` crate). Audited: the runtime now overwrites `ts` per call, so a module's own stamp never reaches the wire; modules keep filling the field for direct callers of `read_points` (the CLI, integration tests). The bench module's pushed generation time moved to `source_ts`
- [x] 2.3 C: add `source_ts_ms` to `tdot_sample_t` (zeroed by `tdot_sample_init`); take one read time per `read_points` call; envelope uses the source time when set
- [x] 2.4 C: fill `source_ts_ms` in the OPC UA module from the source, then server, timestamp
- [x] 2.5 Tests in both builds: a polled pass shares one `ts`; an OPC UA push keeps its source time (unit test plus an e2e check that both builds publish the same `ts`)

## 3. Configuration

- [x] 3.1 Rust: parse `[connector] output` and `measurement`, `batch` at connector/device/point/library level with key-by-key inheritance, and point `sample`, with validation messages that name the place
- [x] 3.2 C: the same keys, inheritance and messages; a parity test feeds both builds the same invalid configs
- [x] 3.3 Both builds: list each device's effective `output` and `batch` in the capability descriptor

## 4. Point table and batcher (Rust)

- [x] 4.1 Per-device slot arrays and per-point precomputed escaped group/series names, built at config load and rebuilt on reload
- [x] 4.2 Write slots after the report policy and map; a withheld reading leaves the slot unchanged
- [x] 4.3 Batcher for `time` and `frame` (open message per group, repeated-point flag, flush on time change, window end or end of pass) and for `sample` (write/read side swap, stale slots left out, event points published with their own time)
- [x] 4.4 Serialize measurements into a reusable buffer with the size estimate and split at 15,000 bytes; publish units metadata on change, before first use
- [x] 4.5 Publish samples only for points flows need, with `measured`; samples mode unchanged
- [x] 4.6 Run the measurement vectors in the SDK unit tests

## 5. Point table and batcher (C)

- [x] 5.1 Slot arrays and precomputed names, allocated at config load and on reload
- [x] 5.2 Slot writes after the report policy and map, in both the poll path and `drain_subscriptions`
- [x] 5.3 Batcher for the three policies, with frame and sample deadlines driving the main-loop wait the way pushed points' intervals already do
- [x] 5.4 Buffer-based measurement serialization with the size estimate and split; units metadata on change
- [x] 5.5 Sample routing with `measured`; samples mode unchanged
- [x] 5.6 Run the measurement vectors in `impl/c/tests/`

## 6. Flows

- [x] 6.1 `ot-measurement` ignores samples with `"measured": true`, with a test in `flows/test-flows.sh`
- [x] 6.2 Move the `m/+` sources of `ot-alarm` and `ot-event` into new `ot-alarm-series` and `ot-event-series` flows with the same params and behavior, and remove `m/+` from the original flows' inputs
- [x] 6.3 Package migration: when an existing `ot-alarm` or `ot-event` params file sets `series`, configure the new series flow with it; settle how unused series flows ship (design open question) and document it. Settled: shipped inactive in `/usr/share/tedge-dot/flows/`, installed into the mapper by `postinstall.sh` only when the old params file names a series

## 7. Harness

- [x] 7.1 C `bench` module: accept `ts_jitter`, `ts_resolution` and `publish_interval` using `source_ts_ms`, matching the Rust module's rules; remove the refusal and its test
- [x] 7.2 Lift `impls = ["rust"]` from `samples-push-jitter`
- [x] 7.3 Add `native-polled`, `native-push-jitter` (once each for `time`, `frame` 100 ms and `sample` 1 s) and `native-flows` to `bench/scenarios.toml`; the observer counts values inside measurement messages and lost readings per policy

## 8. End-to-end and docs

- [x] 8.1 e2e: one measurement-mode device per protocol suite, run with `IMPL=rust` and `IMPL=c`, checking measurement topics, series names, units metadata and that alarm points still raise alarms
- [x] 8.2 Write the user doc for measurement output: switching modes, naming, the three policies with their timestamp and loss trade-offs, `sample = true` for third-party consumers, and the series flows
- [x] 8.3 Add a release highlight in `changes/native-batched-output.md`

## 9. Performance

- [x] 9.1 Run `native-polled`, `native-flows` and `native-push-jitter` for both builds on the baseline host, three times each
- [x] 9.2 Fill the **Performance** table in `design.md` with the baseline and after result lines, and state whether each Rust target was met; if a target is missed, profile it and record the cause before closing the change
