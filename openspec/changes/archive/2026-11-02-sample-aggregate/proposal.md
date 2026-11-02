## Why

A user who reads a point every 100 ms but wants one value per second in the cloud has two
options today. A deadband or `min_interval` drops readings and keeps the ones that pass, and
`group_by = "sample"` publishes the last value at each sample time. Both discard most of what was
read, so a short spike or a slow drift between two published values is lost. An industrial
gateway is usually expected to reduce data by averaging: one value per period that accounts for
every reading, often with its minimum and maximum.

`sample` already publishes one series per point at a fixed rate. Computing an aggregate over the
period, instead of keeping only the last value, gives that reduction at the same message cost.

## What Changes

- **New `batch.aggregate` key** for `group_by = "sample"`. Its value is a string or a list of
  strings, each one of `"mean"`, `"min"`, `"max"` or `"last"`.
  - A string publishes that aggregate under the point's own series name, so
    `aggregate = "mean"` keeps dashboards and alarms on the same series.
  - A list publishes each aggregate as `<series>_<aggregate>`. For example,
    `["mean", "min", "max"]` gives `temp_mean`, `temp_min` and `temp_max`.
  - Without `aggregate`, `sample` behaves as today, which is `"last"`.
  - `aggregate` without `group_by` implies `group_by = "sample"`, so
    `batch = { aggregate = "mean", every = "1s" }` is enough.
- **Time-weighted mean.** Each value counts for as long as it held within the period on the
  device clock, including the value that was current when the period started. For evenly polled
  data this equals the plain average. For change-only pushed data it is the correct average.
- **`min` and `max`** cover every value that held during the period, including the value that was
  current at its start.
- **Booleans** are aggregated as 1 and 0. Their mean is the fraction of the period they were
  true.
- **Raw readings.** The aggregate uses every good reading. A point whose effective policy sets
  both `aggregate` and a `report` policy is refused when the configuration loads.
- **Stale time is left out.** Time while the point is stale (link down, failed subscription
  check, last reading not good) does not count. A point with no good time in the period is left
  out of that sample, as today.
- The capability descriptor's `outputs[].batch` shows `aggregate`.
- **Contract 0.3.0**: §5.5 gains the key, the naming rule and the aggregate definitions. It is a
  new key, so the minor version changes.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `measurement-time-policy`:
  - "Grouping policy setting" gains `aggregate`, its validation and its refusal next to `report`.
  - "Sample and hold" publishes the configured aggregates.
  - "Report policy applies first" makes an exception for aggregated points, which refuse
    `report`.
- `native-measurements`: "Measurement shape and naming" adds the `<series>_<aggregate>` names,
  their units, and the clash check.
- `performance-harness`: "Named scenarios and baseline" accepts `"sample:<every>:<aggregate>,..."`
  in a scenario's `group_by`.

## Impact

- Rust:
  - `impl/rust/crates/sdk/src/measure.rs` (`GroupBy::from_table`, the sample slots and
    `describe`)
  - `config.rs` and `library.rs` (key lists, merge, validation)
- C: `impl/c/sdk/src/measure.c`, `config.c` and `config.h`.
- Vectors: `doc/contract/test-vectors/measurements/vectors.json`, plus loader cases in both
  builds.
- Contract and schema: `doc/contract/ot-connector-contract.md` §5.5 and
  `doc/contract/config.schema.json`.
- Docs: `doc/measurement-output.md`, `doc/reducing-data-volume.md` (aggregation as the
  alternative to deadband), and a `changes/` highlight.
- Bench: `native-push-jitter` gains a `sample:1s` run with `mean`, to check the per-reading cost.
- This change builds on `frame-layers` only through the contract version: whichever change
  lands second takes the next version.
