# Proposal

## Why

The goal is to read about 100,000 points per second on one gateway. Today every reading
becomes its own sample message, and the `ot-measurement` flow turns each one into its own
measurement. The exploration on 2026-10-28 measured this path at about 490 µs of mapper CPU
and 10–12.5 µs of connector CPU per reading, so a gateway manages a few thousand readings per
second. A design spike that wrote readings into a point table and published one batched
measurement per device needed 0.33 µs per reading on the connector side and 17 % of a core
for the mapper at 100,000 readings/s, with no rejected messages.

The point table also gives every protocol one shared boundary. A Modbus block read, an OPC UA
notification and a decoded CAN frame all set a slot. Batching, the message size budget and
the timestamp policy are then implemented once instead of per protocol.

A thin-edge measurement has a single `time`. Readings batch only when they share a timestamp,
so the batch rule is a timestamp rule. Polled readings can share their read time. Pushed
readings carry source times that rarely coincide, so they need a policy. The two builds also
disagree today: the Rust OPC UA module stamps pushed samples with the source time, while the C
sample type has no timestamp and the C runtime stamps every sample when it publishes it.

## What Changes

- **A point table in the SDK runtime of both builds.** Each device has one slot per point. A
  module's reads and pushed samples set slots, and the module API stays as it is. Report by
  exception and value mapping keep their rules and run before a slot is set.
- **Source timestamps in both builds.** A sample can carry a source time. The C sample type
  gains the field, and the C OPC UA module fills it, matching the Rust one. The envelope `ts` is
  the source time when there is one, else the read time.
- **One read time per read call.** All samples returned by one `read_points` call share its
  completion time, so a polled pass batches into one message.
- **Native measurement output** (`[connector] output = "measurements"`, opt-in in this release).
  The runtime publishes thin-edge measurements on `te/device/<device>///m/<group>`, batched per
  device and group under a size budget that stays below Cumulocity's 16,184-byte limit. It
  publishes the units' retained metadata itself. Naming follows the `ot-measurement` rules: the
  point's `meta.measurement`, then a point-id separator, then protocol and point id.
- **Timestamp policies per device**, set with `batch = { group_by = ... }`:
  - `"time"` (default): readings sharing a timestamp share a message, and nothing is moved or
    lost.
  - `"frame"`: readings within a window share a message stamped with the first reading's time.
    A timestamp moves by at most the window, and nothing is lost.
  - `"sample"`: every slot is published at a fixed period, stamped with the sample time. This is
    lossy by choice and only valid while the source is healthy.
- **Samples only where flows need them.** In measurement mode, sample envelopes are published
  only for points with `meta.alarm`, `meta.event`, `meta.parameter`, writable access, or an
  explicit `sample = true`. Those samples carry `"measured": true`, and `ot-measurement` skips
  them.
- **Flows stop waking on measurements.** The `m/+` sources of `ot-alarm` and `ot-event` move to
  separate `ot-alarm-series` and `ot-event-series` flows. A measurement then wakes no JavaScript.
- **Harness follow-through.** The C `bench` module gains the `perf-harness` timestamp keys,
  `samples-push-jitter` runs for both builds, and new `native-*` scenarios record the before and
  after numbers.

## Capabilities

### New Capabilities
- `native-measurements`: the measurement output mode, naming, batching and size budget, units
  metadata, which points still publish samples, and the flow changes that go with it.
- `measurement-time-policy`: source and read timestamps in both builds, one read time per read
  call, the `time`, `frame` and `sample` grouping policies, and how event points and unhealthy
  sources are treated.

### Modified Capabilities
- `bench-connector` (from `perf-harness`): the source-time keys work in both builds, using the
  new C source time.
- `performance-harness` (from `perf-harness`): the `native-*` scenarios, and counting readings
  from measurement values in measurement output.

The report policy (contract §5.3), value mapping (§4.3) and push delivery keep their
requirements; they run before a slot is set.

## Impact

- **SDK runtime, both builds:** a point table and batcher (`impl/rust/crates/sdk`,
  `impl/c/sdk`), config keys `[connector] output` and `batch` at connector, device and point
  level with the usual inheritance, and capability descriptor fields for the effective output
  and batch policy.
- **C SDK API:** `tdot_sample_t` gains a source time. This is additive: a module that doesn't
  set it keeps today's behavior.
- **Modules:** the C OPC UA module sets the source time. The Rust modules stop stamping their own
  read time, which the runtime now sets per call.
- **Contract:** §5 gains `measured` and the rule for `ts`, a new section describes measurement
  output and timestamp policies, and §3 gains `output`, `batch` and `sample`. These are
  additive, and the contract minor version goes up.
- **Flows:** `ot-measurement` skips `measured` samples, and `ot-alarm-series` and
  `ot-event-series` take over the `m/+` sources.
- **Default behavior:** unchanged. `output` defaults to `"samples"` in this release. Flipping the
  default is a later decision, informed by this change's measurements.
- **Performance targets** (Rust, `perf-harness`, same host as the baseline): connector ≤ 1 µs
  per reading and mapper ≤ 25 % of a core at 100,000 readings/s, zero `te/errors`, ≤ 0.1 %
  missing values. C is measured against its own baseline and has no target.
- **Depends on** `perf-harness` for the measurements.
