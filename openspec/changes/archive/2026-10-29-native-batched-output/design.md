# Design

## Context

Today every reading takes this path:

```
module ─▶ runtime (report policy, map) ─▶ sample envelope per reading
      ─▶ broker ─▶ ot-measurement + ot-alarm + ot-event + ot-parameter-state (JS, per message)
      ─▶ m/<group> per reading ─▶ c8y conversion per message ─▶ bridge
```

Measured on 2026-10-28 (M1 Max, tedge 2.0.2-rc423, local broker, `perf-harness` method):

- **Mapper:** about 490 µs per reading with the four sample flows. A flow message costs about
  110 µs even when the flow does nothing, and the built-in c8y conversion costs about 92 µs per
  unbatched message. At 500 series per message the conversion costs 1.56 µs per reading.
- **Connector:** 10–12.5 µs per reading. Allocation takes 36 %, `sendto` 19 %, the JSON
  envelope 16 %, and clock reads and publish timeouts 10 %.
- **Spike:** a point table with one batched measurement per device cost 0.33 µs per reading on
  the connector, 2 % of a core on the broker and 17 % on the mapper at 100,000 readings/s, with
  zero `te/errors`. A batch's c8y size is about 10 bytes per series larger than its thin-edge
  size, so a 12 KB thin-edge batch was rejected and an 8 KB one was not.
- **Conflating frames lose data:** keeping only the latest value per point lost 40 % of
  changes with a 1 s frame and 5.5 % with 100 ms.

What the code does with timestamps today:

- The Rust `Sample` has one `ts`, set by the module. The Rust OPC UA module uses the source
  timestamp, then the server timestamp, then "now". Polled OPC UA reads request no timestamps,
  so they use "now". Other modules stamp "now" per sample, so a pass over 1,000 points spans
  several milliseconds.
- The C `tdot_sample_t` has no timestamp. The C runtime stamps each sample when it builds the
  envelope (`tdot_envelope_sample`), so the C OPC UA module drops source timestamps. This is a
  parity gap today.
- Contract §9 sets millisecond precision, and §5 calls `ts` the read completion time, which the
  Rust OPC UA push path already doesn't follow.
- `ot-measurement`'s `combine` mode keeps the latest value per series and stamps the batch with
  the last sample's time, so it is a conflating frame.

## Goals / Non-Goals

**Goals:**
- Reach the `perf-harness` targets for the Rust build at 100,000 readings/s.
- One shared boundary for every protocol: modules set slots, and the runtime owns batching,
  the size budget and timestamps.
- No reading lost and no timestamp moved unless the user chooses a policy that does so.
- Both builds publish the same messages for the same config.
- Users who don't opt in see no change.

**Non-Goals:**
- Concurrent device reads. The table is designed so that change can write slots from several
  tasks, but this change keeps sequential reads.
- A new module API. Modules keep `read_points` and the push sink, and only gain an optional
  source time.
- Flipping the default `output`.
- Changing the report policy, mapping or alarm semantics.
- SIMD. The spike showed no gain over a plain loop.

## Decisions

### D1. The table lives in the runtime, behind the existing module API
The runtime already receives every reading, as the `read_points` result or through the push
sink. It writes each reading into the device's table after the report policy and the map. No
module changes, apart from the source time (D3).
- *Alternative: modules write slots directly through a new bulk API.* It saves one copy, but
  it means a new API in 7 modules in 2 builds for a gain the spike shows we don't need (the copy
  is a few ns). It can come with concurrent reads.

### D2. Slot layout and buffering
Each device has arrays indexed by point position, built at config load: the point's topic,
its escaped series key, whether it can become a series and publishes samples, its effective
policy, the "in the open message" flag and the sample slot. An open message is kept per topic
and policy (in C, each point knows its message's index). The policies differ in when they read
the slots:

- `time` and `frame` stream: each reading is appended to the open message for its group, and
  the "in the open message" flag gives the O(1) repeated-point check.
- `sample` reads every slot at the sample time.

Both runtimes take readings and publish samples on one loop, so the sample reads the slots in
place. *Changed from the proposal, which planned a write side and a read side swapped at each
sample: with one writer and one reader on the same loop the swap only adds a copy. Concurrent
device reads, when they come, add it.*

### D3. Timestamps: source time, else one read time per call
- Rust: `Sample` gains `source_ts: Option<OffsetDateTime>`. The runtime sets `ts` to the
  completion time of the `read_points` call (or the sink delivery), and the envelope and
  measurement use `source_ts` when it is set. The Rust OPC UA module moves its data value
  timestamp into `source_ts`. The runtime overwrites `ts` for every sample, so what the other
  modules put there never reaches the wire; they keep filling it for callers of `read_points`
  outside the runtime (the CLI, integration tests).
- C: `tdot_sample_t` gains `double source_ts_ms` (0 means none), which `tdot_sample_init`
  zeroes. The runtime takes one read time per `read_points` call. The C OPC UA module fills
  `source_ts_ms` the way the Rust one does, which closes the parity gap.
- Contract §5: `ts` is the source time when the protocol supplies one, else the completion time
  of the read call. This documents what the Rust push path already does.

The grouping key is the millisecond timestamp, because that is the contract's precision.

### D4. Three grouping policies, `time` by default
- `time` is lossless and never moves a timestamp, so nobody gets a time they didn't ask for.
  Polled points share the read time, so they batch fully under it. Pushed points batch as far as
  their timestamps coincide, at about 1 ms resolution.
- `frame` is the opt-in for pushed throughput. It is lossless, and it moves a timestamp by less
  than the window. The first reading's time is used, rather than the window start or end, so a
  frame that holds only one polled pass keeps the exact read time.
- `sample` is the opt-in for fixed-rate output. It loses readings superseded between samples,
  stamps with the sample time, and leaves stale slots out, so it never repeats a value the
  source can no longer vouch for.
- *Rejected: latest per frame (conflation).* It loses up to 40 % of changes and still moves
  timestamps. `sample` is the explicit lossy option. `ot-measurement`'s `combine` keeps working
  in samples mode, unchanged.

### D5. Serialization without a value tree
Measurements are written straight into a reusable per-device buffer: the group and series names
are escaped once per point at config load and copied in, and values are formatted with a
shortest-round-trip float formatter. One MQTT publish goes out per message. This targets the
allocation, JSON and `sendto` shares of today's profile. In C the same buffer approach replaces
the cJSON tree for measurements; sample envelopes keep their current code.

The size estimate is the buffer length plus 12 bytes per series, and a batch is split before
that passes 15,000 bytes. That leaves margin under 16,184 for the measured expansion of about
10 bytes per series. The harness checks it against the real mapper (zero `te/errors`).

### D6. Naming and units move from the flow to the runtime
The runtime applies `ot-measurement`'s naming precedence, and the flow-wide parameters become
a `[connector] measurement` table: `group`, `series`, `point_separator`, `include_boolean`.
`target_topic` is not carried over: the group decides the topic, as in the flow's default. The
units metadata follows the flow's rules (retained, only on change, cleared when empty). The
runtime can publish it on change without a store, because it knows every point's unit at
config load.

### D7. Samples only where flows need them
In measurement mode a point publishes a sample only if a flow reads it: `meta.alarm`,
`meta.event` (the per-signal sources), `meta.parameter` or writable access (`ot-parameter-state`),
or `sample = true` for third-party consumers. `sample = false` forces it off. Such a sample
carries `"measured": true` when the reading also became a series, and `ot-measurement` skips it.
That is one line in the flow and works for mixed fleets.

`ot-alarm` and `ot-event` subscribe to `te/+/+/+/+/m/+` for their single-series sources. In
measurement mode that would wake both flows on every batch, at about 110 µs each, which
undoes most of the gain. The series sources move to `ot-alarm-series` and `ot-event-series`
with the same parameters. Those flows only do something when `params.series` is set.

### D8. Opt-in now, default later
`output = "samples"` stays the default. Switching a connector means setting
`output = "measurements"` and leaving the flows installed. A later change can flip the default
once both builds have run on the measured scenarios and a release has shipped the option.

### D9. Parity through shared vectors
`doc/contract/test-vectors/measurements/` lists readings (with source and read times, quality,
meta and units) and a policy, and gives the expected messages: topics, bodies with the time as
a placeholder for read times, splits and units metadata. Both builds' unit tests run them, and
the e2e suites run one measurement-mode device per protocol with `IMPL=rust` and `IMPL=c`.

### D10. Harness follow-through
- The C `bench` module accepts `ts_jitter`, `ts_resolution` and `publish_interval` using the
  new source time. `samples-push-jitter` drops its `impls = ["rust"]` restriction.
- A scenario sets `output`, a list of `group_by` policies (`"time"`, `"frame:<window>"`,
  `"sample:<every>"`) and an `alarm_share`. In measurement output the harness counts readings
  from the measurement values by their `time`, reports `values_per_message`, and reports as
  missing the values that did not reach `c8y/measurement/measurements/create`.
- New scenarios in `bench/scenarios.toml`, both builds:
  - `native-polled`: bench, polled, 100 devices at 1 s, the points per device following the
    rate (10k/50k/100k readings/s): the shape of `samples-noflows`, so the two compare directly.
  - `native-push-jitter`: bench, pushed, `ts_jitter` 50 ms, `publish_interval` 100 ms, run
    once each with `group_by = "time"`, `"frame"` (100 ms) and `"sample"` (1 s). It reports
    values per message and lost readings.
  - `native-flows`: as `native-polled` at 100k, with 1 % of points carrying `meta.alarm` and
    the `ot-*` flows installed.

## Performance

Measured on 2026-10-29 with `perf-harness` on the baseline host (M1 Max, thin-edge.io
2.0.2-rc423, mosquitto 2.1.2), three runs per scenario and implementation, medians shown. The
result lines are in `bench/results/3e017f31-3ad1-40a9-b508-3e3bb163749c-2026-10-29.jsonl`: the
host's name changed between the two days, the machine and tool versions did not. Unrelated
containers were running on the host during both. Every run published zero `te/errors` and lost
no value.

| Scenario (100k readings/s) | Baseline (`main`), Rust / C | Target (Rust) | After, Rust | After, C | Rust target |
|---|---|---|---|---|---|
| `native-polled` vs `samples-noflows` | connector 7.15 / 9.72 µs, 1 message per reading | connector ≤ 1 µs/reading | 0.93 µs, 333 values/message | 8.08 µs, 250 values/message | Met |
| | mapper 0 % (no flow converts the samples) | mapper ≤ 25 % of a core, 0 `te/errors`, ≤ 0.1 % missing | mapper 9 %, 0, 0 % | mapper 13 %, 0, 0 % | Met |
| `native-flows` vs `samples-flows` | mapper 147 / 145 % at 4,000 readings/s, drain 3.8 / 5.6 s | mapper ≤ 35 % | 35.0 % (34.2–36.8), drain 0.3 s | 43 %, drain 0 s | Met, at the limit |
| `native-push-jitter` vs `samples-push-jitter` (1 ms) | connector 15.80 / 9.64 µs, 1 message per reading | `frame` 100 ms ≥ 100 values/message | `time` 1, `frame` 32, `sample` 333 values/message | `time` 1, `frame` 31, `sample` 333 | Missed |

Mapper CPU per policy in `native-push-jitter`, Rust / C: `time` 232 / 240 % with 49 / 46 s of
drain, `frame` 32 / 41 %, `sample` 10 / 13 %.

**Causes recorded for the targets at or past their limit:**

- *`native-polled` connector* first measured 1.04–1.06 µs. Reading the poll path (not a profiler
  run) showed the runtime cloning every sample before mapping it; judging the pass's health
  before handing the samples on removed the copy, and the connector went to 0.92–0.95 µs.
- *`native-flows` mapper* is, by estimate rather than a profile, the 1 % of points with
  `meta.alarm`: about 1,000 samples/s still go through the four sample flows (`ot-measurement`,
  which drops them as `measured`, `ot-alarm`, `ot-event`, `ot-parameter-state`). At the roughly
  110 µs per flow message the exploration measured, that is about 0.4 of a core, close to the
  35 % observed; `native-polled`, without flows, costs the mapper 9 %. The measurements wake no
  flow. A site
  with more alarm points pays per alarm point; declaring the alarm on fewer points, or a native
  alarm path in the runtime, is what lowers it further.
- *`frame` values per message* is bounded by the scenario, not the batcher: at 1,000 pushed
  readings/s per device, a 100 ms frame holds about 100 readings drawn at random from 1,000
  points, and a point repeats after about 32 of them (the birthday bound). A repeat closes the
  message so that no value is overwritten, which caps a message near 32 values. The target of 100
  values/message was not reachable at this rate even without repeats; it needs fewer devices
  with more readings each, or a longer window. Letting a repeated point open a second message
  while the first keeps filling would roughly double the values per message, and is left for a
  follow-up.
- *`time` with jittered source times* is as expected: millisecond times rarely coincide, so it
  publishes one value per message, and the mapper falls behind. This is what `frame` and
  `sample` are for, and the guide says so.
- The rate shortfall of the polled scenarios (about 12 % in Rust) is the poll timing found by
  `perf-harness` (`next_due = now + interval`), unchanged by this change.

## Risks / Trade-offs

- **[Frame moves timestamps]** → Opt-in only. The descriptor shows the effective policy, and
  the docs state the bound (less than one window).
- **[Samples mode and measurement mode differ in what consumers see]** → `measured` marks the
  overlap, and only points with flows publish samples. A third-party consumer of samples sets
  `sample = true` on its points, and the migration guide says so.
- **[Readings held in an open frame are lost on a crash]** → At most one window per device, the
  same exposure as the flow's `combine` today.
- **[Size estimate wrong for unusual names]** → The estimate uses the escaped names. The vectors
  include long and non-ASCII names, and the harness fails on any `te/errors`.
- **[C memory for large tables]** → Slot arrays are allocated at config load from the point
  count, sized like the existing per-point state. The `native-polled` scenario records the RSS.
- **[Moving the `m/+` sources breaks existing configs of `ot-alarm`/`ot-event`]** → Their params
  move unchanged to the new flows. The package migration copies a params file that sets
  `series` to the new flow, and the release highlight says so.

## Migration Plan

1. Ship with `output = "samples"` by default. Nothing changes for existing installs, apart
   from the series sources moving to their own flows, which the package migration handles.
2. Document switching a connector to `output = "measurements"`.
3. Decide the default flip in a later change, using this change's measurements and field
   reports.

Rollback: set `output = "samples"`. The flows still handle samples.

## Open Questions

- Is the 100,000 readings/s goal the edge acquisition rate or the cloud egress rate? At 100,000
  values/s the egress is about 2.3 MB/s, or 8.6 billion values a day. That points users to
  `report` and `sample` for egress, and this change's targets are for acquisition.
- Should CAN signals count as event points under `sample`, since the module can't read them on
  demand? This design treats only `meta.event` with `every` as events, and a CAN signal as state.
- How should the package ship `ot-alarm-series` and `ot-event-series` so they cost nothing
  when unused? Two options: inactive until configured, or installed only by the migration when
  a params file sets `series`.
