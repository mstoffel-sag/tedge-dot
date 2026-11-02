## Why

Under `batch.group_by = "frame"`, a point that changes twice within a window publishes the open
message at once and starts a new one. With 1,000 points per device, a point repeats after about
40 readings, whatever the window (the birthday bound). Messages therefore stay at about 32 to 38
values, and messages per second grow with the window instead of shrinking. The mapper's cost is
per message (about 92 µs per conversion), so a longer window, which users would choose for
pushed telemetry, saves nothing today.

A simulation of `native-push-jitter` (100 devices × 1,000 points, 100,000 readings/s, no report
policy) gives:

| Window | Messages/s now | Messages/s with layers |
| --- | --- | --- |
| 100 ms | 3,136 | 2,132 |
| 500 ms | 2,668 | 800 |
| 1 s | 2,610 | 550 |

The `native-batched-output` target of "≥ 100 values per message with a 100 ms frame" also
measured the wrong thing. That target was missed, and it would still be missed with layers. The
mapper's load depends on messages per second, and 500 ms is the more realistic window for
unfiltered pushed data.

## What Changes

- **Layers in a frame.** A reading goes into the frame's first message that does not hold its
  point yet: a point's first value in the window goes into layer 0, its second into layer 1, and
  so on. All layers are published when the window ends, in layer order. No reading is dropped,
  and a repeat no longer publishes anything early.
- **Message times stay in order.** For each topic and grouping policy, every message time is
  later than the time of the message before it. A layer is stamped with its first reading's
  time, or 1 ms after the previous message's time when that is later. A point's values
  therefore reach the cloud in time order even when source timestamps jitter. Today a
  repeat after jitter can store a newer value with an older time.
- **A cap on layers.** At 64 layers, a reading that would open a 65th publishes the frame's
  layers and starts a new frame in the same window, as a repeat does today. This bounds the
  memory a single fast-changing point can hold.
- **Bench.** `native-push-jitter` also runs `frame:500ms`. Its target is restated as mapper CPU
  and messages per second instead of values per message.
- **Docs.** `doc/measurement-output.md` and contract §5.5 describe layers. The guide explains
  that a `min_interval` of at least the window already prevents repeats.
- **Behavior change for `frame` users:** a repeated point's earlier values are now published at
  the end of the window instead of at once. They are still published within the existing bound
  (the window plus one runtime pass). Message times can move by up to 1 ms per message to keep
  them in order. `"time"` and `"sample"` do not change.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `measurement-time-policy`: the "Group by frame" requirement replaces publish-on-repeat with
  layers, adds strictly increasing message times per topic and policy, and adds the layer cap.

## Impact

- Rust: `impl/rust/crates/sdk/src/measure.rs`. `Batcher::add`, `close` and `due` handle a list
  of layers per topic and policy. A per-point layer count replaces the `in_open` flag, and each
  key keeps its last message time.
- C: `impl/c/sdk/src/measure.c`, with the same changes on its precomputed slots.
- Vectors: `doc/contract/test-vectors/measurements/vectors.json`. The repeat case changes, and new
  cases cover layers, ordering and the cap.
- Contract: `doc/contract/ot-connector-contract.md` §5.5, in the grouping table. The version goes
  from 0.2.0 to 0.2.1: the behavior changes, but no key or message shape does.
- Bench: `bench/scenarios.toml` and the results file.
- Docs: `doc/measurement-output.md` and a `changes/` highlight.
- No configuration keys change.
