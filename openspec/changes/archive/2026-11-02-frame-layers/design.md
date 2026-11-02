## Context

`native-batched-output` added `batch.group_by = "frame"`: readings within a window of the device
clock share one thin-edge measurement, stamped with the first reading's time. A thin-edge
measurement holds one value per series. When a point repeats within the window, the batcher
publishes the open message at once and opens a new one in the same window. That keeps every
value, but a message only lasts until the first repeat. With N points drawn at random, a repeat
comes after about √(πN/2) readings, which is about 40 for 1,000 points. The window does not
change that.

The bench measured 32 values per message for `frame:100ms` in `native-push-jitter`, in both
builds, and a simulation of the batcher's rule reproduces it (31). The same simulation gives:

| Window | Readings per device and window | Messages per window now | With layers |
| --- | --- | --- | --- |
| 100 ms | 100 | 3.2 | 2.1 |
| 200 ms | 200 | 5.7 | 2.7 |
| 500 ms | 500 | 13.3 | 4.0 |
| 1 s | 1,000 | 26.1 | 5.5 |

With layers, messages per window grow with the most changes of any single point, which rises
slowly. So values per message keep growing with the window.

When a report policy is used, readings are filtered before they reach the batcher. With 30 % or
fewer of readings passing at 100 ms, repeats are rare and both rules publish about one message
per device and window. A `min_interval` of at least the window makes repeats impossible. Layers
therefore matter for unfiltered pushed data with a window of a few hundred milliseconds or more.

The Rust batcher is `impl/rust/crates/sdk/src/measure.rs`. It keeps one `Open` per (topic,
policy) in `DeviceTable::open` and an `in_open` flag per point. The C batcher,
`impl/c/sdk/src/measure.c`, keeps one `mopen_t` per precomputed slot (`p->slot`) and an
`in_open` flag per point. Both run the shared vectors in
`doc/contract/test-vectors/measurements/vectors.json`.

## Goals / Non-Goals

**Goals:**
- A frame publishes about one message per window plus one per extra change of its busiest point,
  instead of one per birthday collision.
- Each point's values keep their time order in the cloud, including when source times jitter.
- The Rust and C builds produce identical output, pinned by the vectors.
- The bench target measures the mapper's load (messages per second, CPU), and the scenario runs
  the 500 ms window.

**Non-Goals:**
- Averaging or other aggregation. That is the follow-up `sample-aggregate`.
- Changing `"time"` or `"sample"`.
- Batching across devices. Each device has its own measurement topic.
- New configuration keys. The layer cap is fixed.

## Decisions

### D1. Layers, indexed by each point's count in the frame
Each point keeps a count of its values in the current frame, instead of the `in_open` flag. A
reading goes into layer `count[k]`, which is created if it doesn't exist, and the count goes up.
Layers are a short vector per (topic, policy). Publishing the frame drains them in order and
resets the counts of the points they held.
- This is O(1) per reading, like today. The counts are reset from each layer's point list,
  which the batcher already keeps for `in_open`.
- *Rejected: searching for the first layer without the point.* That gives the same result, since
  a point's values fill layers 0, 1, 2… in order, but costs O(layers) per reading.
- *Rejected: publishing layer 0 early once it is "full enough".* Later readings of new points
  could then no longer join it, and it adds a tuning knob.

### D2. Strictly increasing layer times per topic and policy
Each (topic, policy) keeps the time of its last layer, across frames. A new layer's time is
`max(first reading's time, last + 1 ms)`. Split parts share their layer's time.
- Without this, a jittered source can stamp layer 1 earlier than layer 0. A point's newer value
  would then be stored with an older time, and Cumulocity's latest value would show the older
  one. Today's rule has the same flaw when the reading after a repeat carries an earlier time;
  layers would make it more likely, because any point can open a layer.
- The move is 1 ms per layer, and only when source times go backwards. Without jitter, times are
  unchanged: polled passes and in-order push keep their exact times.
- The last time is kept across frames, so frame 2 also stays after frame 1. It lives in the
  batcher, so a configuration reload (which rebuilds the batcher) starts it again. One frame
  around a reload can then be out of order, which is acceptable.
- *Rejected: no ordering guarantee, only documenting it.* That stores wrong latest values, and
  the fix costs one integer per (topic, policy).
- *Rejected: stamping every layer with the window's start or end.* That moves times further, and
  layers of one frame would collide on the same time.

### D3. A cap of 64 layers
A reading that would open layer 64 (the 65th) publishes the frame's layers and starts a new
frame in the same window. That is today's behavior, applied at a higher limit.
- A point that changes every millisecond in a 1 s window would otherwise hold 1,000 one-value
  layers until the window ends. The cap bounds what a device holds to 64 open messages per
  (topic, policy).
- 64 is well above the 5 to 8 layers the simulation reaches at 1 to 2 s without a report
  policy, so the cap only catches a runaway point.
- *Rejected: a `batch.max_layers` key.* Nobody has asked for it, and it can be added later.

### D4. Publishing at the end of the window only
A repeat no longer publishes the earlier layer at once. Every layer waits for the end of the
window (plus one pass), which is the latency bound the spec already gives. A repeated point's
first value can arrive up to one window later than today. That is the price of batching it, and
it stays within the bound users already accepted with the window.

### D5. The bench measures messages per second
`native-push-jitter` gains `frame:500ms` next to `frame:100ms`. The target is stated in mapper
CPU and messages per second, and values per message is still reported.

### D6. Contract 0.2.1
§5.5's grouping table changes for `"frame"`: published "when the window ends, in layers", and
times strictly increasing. No key or message shape changes, so the version goes to 0.2.1.

## Performance

Scenario: `native-push-jitter` (100 devices × 1,000 points, push, `ts_jitter` 50 ms,
`publish_interval` 100 ms, 100,000 readings/s, 20 s window), with `group_by = "frame:100ms"`
and `"frame:500ms"`, both builds, three runs each. All lines come from one Apple M1 Max, with
thin-edge.io 2.0.2~423 and mosquitto 2.1.2. The host's name changed between 2026-10-29 and
2026-11-02, so the two result files record different host names.

- Before, `frame:100ms`: `bench/results/3e017f31-3ad1-40a9-b508-3e3bb163749c-2026-10-29.jsonl`
  (`native-batched-output`, sha 9c1254d).
- Before, `frame:500ms`: `bench/results/Reubens-MacBook-Pro-2-2026-11-02.jsonl`, the lines
  stamped before 01:30 (main at f1f0e45, before this change; `dirty` because the scenario list
  was narrowed for the run).
- After: the same file, from 01:30 on.

Medians of three runs:

| Policy | Build | c8y messages/s | Values per message | Mapper CPU | Connector µs per reading |
| --- | --- | --- | --- | --- | --- |
| `frame:100ms` | Rust | 3,068 → **2,025** | 32.6 → 49.2 | 32.9 % → **24.1 %** | 3.86 → 4.19 |
| `frame:100ms` | C | 3,123 → **2,021** | 32.0 → 49.5 | 41.0 % → **26.9 %** | 6.35 → 6.24 |
| `frame:500ms` | Rust | 2,657 → **915** | 37.6 → 109.1 | 31.4 % → **17.2 %** | 4.13 → 3.82 |
| `frame:500ms` | C | 2,647 → **902** | 37.7 → 111.1 | 34.6 % → **15.8 %** | 5.95 → 5.33 |

Every run had 0 missing values, 0 `te/errors` and a drain under 0.1 s.

| Target (Rust) | Measured | Met |
| --- | --- | --- |
| `frame:100ms` ≤ 2,300 c8y messages/s | 2,025 | Yes |
| `frame:500ms` ≤ 1,000 c8y messages/s | 915 | Yes |
| `frame:500ms` mapper CPU ≤ 25 % of a core | 17.2 % | Yes |
| `frame:100ms` mapper CPU lower than before | 32.9 % → 24.1 % | Yes |
| 0 lost, 0 errors | 0 / 0 | Yes |

- The measured messages per second are close to the simulation: 2,132 predicted against 2,025
  at 100 ms, and 800 against about 910 at 500 ms. The push module delivers in batches every
  100 ms, which the simulation did not model.
- The C build reaches the same message counts as Rust, as the shared vectors require. Its mapper
  CPU fell further because it started higher.
- The connector cost does not change beyond run-to-run noise. Placing a reading is still O(1),
  and the old rule's early publishes are gone.
- At 500 ms, values per message now pass the 100 that `native-batched-output` targeted for
  100 ms.

## Risks / Trade-offs

- **[A repeat's values arrive later than before]** → Bounded by the window plus one pass, as the
  spec already states. The changes highlight says so for existing `frame` users.
- **[Times move by 1 ms per layer under jitter]** → Only when a source time goes backwards. The
  move keeps each point in order, which is what a time series needs. The guide documents it.
- **[More memory per device]** → At most 64 open messages per (topic, policy). Each holds only the
  readings that arrived, so the total is the same as the readings in one window.
- **[C and Rust diverge]** → New vectors for layers, ordering and the cap run in both unit
  suites. No e2e test covers `frame` today. A new OPC UA one (a pushed node that changes faster
  than the window) runs on both builds.

## Migration Plan

No configuration change. Upgrading changes when and how `frame` messages are published (D4) and
can move times by 1 ms (D2). Downgrading restores the old rule. Nothing is stored.

### D7. 500 ms in the examples
The guide and the contract show `window = "500ms"` instead of `"100ms"` (in the grouping table,
the `frame` example, the descriptor example and the contract's device example). 500 ms is where
layers start to pay off for unfiltered pushed data. A user with a deadband or `min_interval` at
100 ms gets about one message per window with either rule, so the shorter window stays valid
for them.

## Open Questions

None.
