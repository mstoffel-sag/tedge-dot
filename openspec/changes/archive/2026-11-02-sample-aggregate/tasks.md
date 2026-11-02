## 1. Configuration

- [x] 1.1 Rust: add `aggregate` to the `batch` key lists in `config.rs` and `library.rs`, parse a string or a non-empty list without repeats of `mean`, `min`, `max`, `last`, read a table with `aggregate` and no `group_by` as `group_by = "sample"` before merging, and refuse it in a table that sets `group_by` to `"time"` or `"frame"`; ignore an inherited one the effective policy does not use
- [x] 1.2 Rust: refuse a point whose effective policy has an `aggregate` and a non-passthrough `report`, naming the point, `aggregate` and `report`; refuse it on `call_method` points with `batch`
- [x] 1.3 Rust: resolve the suffixed series names and refuse a clash with another series of the device and group, naming both points
- [x] 1.4 C: the same parsing, merging and refusals in `config.c` / `config.h` (`check_batch_values`, `resolve_point_batches`), with the same messages
- [x] 1.5 Add loader cases for each refusal and for the implied `sample` (a point's `aggregate` under a device's `frame`; `aggregate` with no `every` anywhere) to both builds' tests (`POINT_CASES` in Rust, `tests/config.c` in C), and update `doc/contract/config.schema.json`

## 2. Vectors

- [x] 2.1 Add vectors: time-weighted mean of change-only data (10 → 20 at 900 ms gives 11); mean of evenly polled readings (5.5); min and max including the carried value; boolean duty cycle (0.25)
- [x] 2.2 Add vectors: stale time left out (link down for half the period); no good time leaves the point out; a list publishes `<series>_<aggregate>` with units for each; a string keeps the series name
- [x] 2.3 Add a vector whose mean is not exactly representable, pinning the formatted text
- [x] 2.4 Document any new step or field in `doc/contract/test-vectors/measurements/README.md`

## 3. Rust batcher

- [x] 3.1 In `measure.rs`, extend the sample slot with the accumulator (held value, since, weighted sum, good time, min, max) and update it per reading in O(1) without allocation
- [x] 3.2 Close the interval on stale (`set_stale`, a reading that is not good, a failed check) and reopen it on the next good reading
- [x] 3.3 At the sample time, close the last interval, publish the configured aggregates under the resolved names, and reset the accumulator to the held value; keep `"last"` as today's output without `aggregate`
- [x] 3.4 Include `aggregate` in `describe` and in the units metadata for suffixed series; count suffixed series in the size estimate
- [x] 3.5 Run the vectors and the Rust workspace tests

## 4. C batcher

- [x] 4.1 In `measure.c`, add the accumulator to the per-point slot, with time in int64 ns
- [x] 4.2 Implement stale handling, publication at the sample time, naming, units and the size estimate as in Rust
- [x] 4.3 Add `aggregate` to the C descriptor's `outputs`
- [x] 4.4 Run `ctest` and check that both builds pass every vector

## 5. Contract, docs and e2e

- [x] 5.1 Update §5.5 in `doc/contract/ot-connector-contract.md` (the key, the aggregate definitions, naming, the refusal with `report`) and set the version per D6; update the connector specs that state the version
- [x] 5.2 Add an "Aggregating" section to `doc/measurement-output.md`, and point to it from `doc/reducing-data-volume.md` as the alternative to a deadband
- [x] 5.3 Add `changes/sample-aggregate.md`
- [x] 5.4 Add an e2e test with a polled point under `aggregate = ["mean", "max"]` on a simulator with a changing value, checking both series, their units, and that the mean lies between the simulator's min and max; run it with both builds

## 6. Performance

- [x] 6.1 Extend the harness's `group_by` scenario syntax with an aggregate suffix (`"sample:1s:mean,min,max"`) in `crates/bench/src/scenario.rs`
- [x] 6.2 Run `native-push-jitter` with `sample:1s` with and without the aggregates, for both builds, three times each, and fill design.md's Performance section with the lines and whether the target was met
- [x] 6.3 Run `just lint`, the Rust workspace tests, `ctest`, the flow tests and `openspec validate sample-aggregate --strict`
