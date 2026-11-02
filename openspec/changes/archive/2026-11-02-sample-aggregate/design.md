## Context

`group_by = "sample"` (from `native-batched-output`) keeps one slot per point and publishes every
good slot each `every`, stamped with the sample time. The slot holds the last value, so a period
publishes one value and discards the rest. Reducing data today means a `report` policy
(deadband, `min_interval`) or sample-and-hold, and both drop readings.

The batcher already has everything an aggregate needs: one slot per point, the device clock
(`Instant`), a sampler per distinct `every` that fires on time, and staleness per device and per
point (`set_stale`, the last reading's quality, the subscription check). The Rust batcher is in
`impl/rust/crates/sdk/src/measure.rs` and the C batcher in `impl/c/sdk/src/measure.c`. Both run
the shared vectors.

The user's decisions:
- the mean is time-weighted
- the aggregate uses raw readings, and `report` is refused on an aggregated point
- a point can publish several aggregates, as suffixed series

## Goals / Non-Goals

**Goals:**
- One value per point and period that accounts for every reading: the time-weighted mean, min,
  max or last.
- O(1) work per reading, and no allocation, on the hot path.
- The same output from both builds, pinned by vectors.

**Non-Goals:**
- Other statistics (count, standard deviation, percentiles, integrals). They can be added to the
  same accumulator later.
- Aggregation in `"time"` or `"frame"`. Those are lossless streams.
- Aggregating samples for the flows. Samples stay one per reading, and alarms still see every
  value.
- Windows aligned to wall-clock boundaries. The samplers keep their current phase, from when the
  batcher was built.

## Decisions

### D1. Accumulator per slot
Each slot of an aggregated point holds `{held value, since, weighted sum, good time, min, max}`.
A reading closes the held value's interval:

```
sum  += held × (now − since)
good += now − since
min/max over held
held = value; since = now
```

At the sample time the same step closes the last interval. The batcher then publishes `sum ÷
good`, `min`, `max` or `held`, and resets: `since = sample time`, `sum = good = 0`, and
`min = max = held`. That reset is what makes the value current at the start count from the start
of the period.
- Only the aggregates a point asks for are computed, but the accumulator is the same for all, so
  the cost is one multiply-add and two compares per reading.
- Time is the device clock, which is monotonic, not the source timestamp. Sample times already
  use it, and a jittered source time must not distort the weights.
- *Rejected: storing the readings and aggregating at sample time.* Memory would grow with the
  reading rate.

### D2. Stale intervals do not count
Going stale (the device link down, a failed subscription check, or a reading that is not good)
closes the held interval and stops accumulating. Becoming good again starts a new interval from
the good reading. A point with `good == 0` at the sample time is left out, which is today's
rule, generalised. `last` after a stale stretch is the last good value only if the point is good
again at the sample time, also as today.

### D3. `report` is refused, not ignored
The loader resolves the effective `batch` and `report` per point, which it already does for both.
It refuses the point when an `aggregate` is set and the `report` policy is not passthrough. The
error names the point and both keys. A deadband before an average biases it, and silently
ignoring `report` would confuse anyone reading the configuration.
- The check runs after merging, so a device-level `report` with a connector-level `aggregate`
  is caught on every point it reaches.
- *Rejected: applying report first, as today's `sample` does.* The user chose raw readings.

### D4. Naming: a string keeps the name, a list suffixes
`aggregate = "mean"` publishes the mean under the point's series, so a user switching from
sample-and-hold keeps their dashboards. `aggregate = ["mean", "max"]` publishes
`<series>_mean` and `<series>_max`. A one-element list also suffixes, so the form chooses the
naming, not the count.
- Underscore, not a dot: c8y treats the series name as one key, and a dot would look like the
  `point_separator` split.
- Every suffixed series gets the point's unit in the `/meta` units message.
- A suffixed name that clashes with another series of the device and group is refused when the
  configuration loads. Clashes between plain series stay as they are today.
- The size estimate counts each suffixed series separately, so splits stay under the budget.

### D5. Validation and descriptor
- `aggregate` joins the `batch` key lists in both builds, merged key by key like the durations.
- A table that sets `aggregate` without `group_by` is read as `group_by = "sample"` at that
  level, before merging. `batch = { aggregate = "mean", every = "1s" }` is then a complete
  policy, and a point can opt into aggregation under a device that chose `"frame"`. Applying it
  per table, not to the merged result, keeps the rule local: an `aggregate` inherited from the
  connector does not override a device's explicit `group_by`.
- A table that sets `group_by = "time"` or `"frame"` together with `aggregate` is refused. An
  inherited `aggregate` that the effective policy does not use is ignored, by the same rule as
  `window` and `every`.
- `call_method` points refuse it, as they refuse `batch`.
- The descriptor's `outputs[].batch` includes `aggregate` as configured, either a string or a
  list.

### D6. Contract 0.3.0
The contract gains a key, so its minor version goes up. If `frame-layers` lands first with
0.2.1, this change goes from 0.2.1 to 0.3.0. If this change lands first, it takes 0.3.0 and
`frame-layers` takes 0.3.1.

### D7. The C build finds the shortest number form by binary search
Found during the bench: with three aggregates, the C connector's CPU doubled (45.8 % to 97.5 % of a
core) and it fell behind (266,632 of 300,000 values/s). A profile showed libc's `__dtoa`,
`__vfprintf` and `strtod` dominating, called from `tdot_format_number`. That function finds the
shortest form that reads back as the same double by trying each precision from 0 to 16, which
takes up to 17 `snprintf` and `strtod` calls for a value with no short decimal form, such as a
mean or a minimum of noisy readings. Rust uses `ryu` and is not affected.
- Away from a power of two, the doubles that read back as `d` lie symmetrically around it, so a
  precision that reads back makes every longer one read back too. A binary search then finds the
  same shortest precision in about five tries.
- At an exact power of two, the gap below is half the gap above, so a longer form can fall out of
  the interval; the formatter keeps the linear search there.
- `tests/measure.c` checks 200,000 random doubles, means and powers of two against the linear
  search: same significant digits, and each reads back as the same double.
- *Rejected: vendoring a ryu port for C.* It would be faster still, but it adds a dependency for
  one function; the binary search removes most of the cost.

## Performance

This change claims no throughput gain; its cost on the hot path has to stay small. The scenario
is `native-push-jitter` (100 devices × 1,000 pushed points, 100,000 readings/s, 20 s window)
with `group_by = "sample:1s"`, `"sample:1s:mean"` and `"sample:1s:mean,min,max"` (the harness's
new suffix for `aggregate`), three runs each, on the M1 Max of `frame-layers`. All lines are in
`bench/results/Reubens-MacBook-Pro-2-2026-11-02.jsonl`. The Rust lines are from 02:19 to 02:28
UTC. The C lines from 02:34 on are after D7; the ones from 02:22 to 02:30 are before it.

The harness's µs per reading divides by the values it counts in the measurements, and three
aggregates publish three values per reading, so connector CPU at the same reading rate is the
comparable figure.

Medians of three runs (0 missing values and 0 `te/errors` in every run):

| Policy | Rust connector CPU | C connector CPU (before D7 → after) | Mapper CPU (Rust / C) | c8y messages/s |
| --- | --- | --- | --- | --- |
| `sample:1s` | 35.3 % | 45.8 % → 27.5 % | 10.6 % / 11.9 % | 300 |
| `sample:1s:mean` | 33.7 % | 46.7 % → 28.3 % | 10.4 % / 11.3 % | 300 |
| `sample:1s:mean,min,max` | 40.3 % | 97.5 % → 62.7 % | 29.8 % / 33.5 % | 900 |

| Target | Measured | Met |
| --- | --- | --- |
| The accumulator adds at most 0.1 µs per reading in Rust (`mean` against `sample:1s`, same output) | −1.6 points of a core: within the noise between runs (34.5 to 36.0 % for `sample:1s`) | Yes |
| The same in C | +0.8 points of a core: about 0.08 µs per reading | Yes |
| One aggregate leaves the mapper unchanged | 10.6 → 10.4 % (Rust), 11.9 → 11.3 % (C) | Yes |

- Three aggregates publish three series per point. The size budget keeps a message at about 333
  series, so messages, and with them mapper CPU, triple. That is the cost of the output asked for,
  not of the accumulator. The guide says that each aggregate is a series in the cloud.
- Before D7 the C connector could not keep up with three aggregates: 97.5 % of a core and 266,632
  of 300,000 values/s. After D7 it delivers them all at 62.7 %. D7 also lowered plain
  `sample:1s` from 45.8 % to 27.5 %, and any C path that publishes numbers without a short
  decimal form gains from it.

## Risks / Trade-offs

- **[Three aggregates triple the series, so the messages]** → The size budget splits them, so
  messages and mapper CPU grow with the aggregates configured (measured above). The guide notes
  that each aggregate is a series in the cloud; `aggregate = "mean"` costs the same as
  sample-and-hold.
- **[Floating-point differences between builds]** → The mean is `sum / good` in doubles, with time
  in integer nanoseconds (the C device clock is already int64 ns). The vectors compare JSON
  numbers formatted by the shared shortest-round-trip rule. A vector whose mean is not exactly
  representable pins the expected text, so a difference in summation order shows up.
- **[A refused configuration on upgrade]** → Not possible: no existing configuration sets
  `aggregate`.
- **[Users expect calendar-aligned periods]** → Documented: periods start when the configuration
  is loaded. Alignment is a possible follow-up.

## Migration Plan

Opt-in through a new key. Nothing changes for existing configurations. Rollback means removing
the key.

## Open Questions

None.
