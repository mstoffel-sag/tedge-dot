## Context

The SDK runtime gives a pushed point's module a per-point sampling hint
(`PointRef::interval` in Rust, `tdot_point_t.poll_interval_s` in C). Today that hint is the
point's resolved `poll_interval`:

- Rust: `push_points` (`impl/rust/crates/sdk/src/runtime.rs`) sets `r.interval` to
  point → device → connector `poll_interval`.
- C: `sampling_interval_ms` (`impl/c/connectors/opcua/connector_opcua.c`) returns
  `pt->poll_interval_s * 1000`. A comment there notes that the two builds must derive it the same
  way.

The OPC UA module requests a monitored item with `samplingInterval = hint` and `queueSize = 1`.
It creates one subscription per device with `publishingInterval = min(hint)`. A change is
therefore seen at the next sample and sent at the next publish. That is up to about
sampling + publishing of delay: about 4 s with the default `2s`.

Configuration is strictly validated in both SDKs. Unknown keys are refused with a "did you mean"
hint, and both loaders must refuse the same files with the same messages. The parity harness runs
every e2e suite with `IMPL=rust` and `IMPL=c`.

## Goals / Non-Goals

**Goals:**
- An operator can set how often a pushed point is sampled without changing how often polled
  points are read.
- Every existing configuration keeps exactly its current behaviour.
- The Rust and C builds request the same parameters for the same file.
- The docs explain where push latency comes from.

**Non-Goals:**
- OPC UA server-side `DataChangeFilter`/deadband. The SDK report policy already filters changes.
- `queueSize > 1` or delivering every intermediate sample. A pushed point still reports only the
  latest value per publish.
- A `-1` ("use the publishing interval") value.
- A configurable OPC UA publishing interval (see decision 5).
- Renaming or deprecating `poll_interval`.
- Sampling rates for other push sources (SNMP traps, CAN frames). They have no sampling timer and
  ignore the hint.

## Decisions

### 1. `sampling_interval` is a contract key, not an OPC UA `address`/`connection` key

The key sits next to `poll_interval` on `[connector]`, `[[device]]` and points (inline, in
libraries and in device types), and it is resolved the same way. The SDK already models a
"sampling hint" for push-capable modules, and operators already know how `poll_interval`
inherits. Putting the key at the contract level keeps the inheritance, validation, management
commands and "did you mean" handling in the SDK, where they are already the same in both builds.

*Alternative considered:* OPC UA-only keys (`[connection] sampling_interval`,
`point.address.sampling_interval`). This was rejected. The point-level setting would sit inside
the addressing object, the inheritance logic would be duplicated per module and per build, and
any future push connector with a sampling timer would invent its own spelling.

### 2. Resolution: an explicit `sampling_interval` at any level beats every `poll_interval`

```
effective sampling = point.sampling_interval
                  ?? device.sampling_interval
                  ?? connector.sampling_interval
                  ?? effective poll_interval   (point ?? device ?? connector ?? 2s)
```

The two settings are resolved as separate chains, with `poll_interval` used only as the final
fallback. This is easy to explain ("set `sampling_interval` anywhere and it wins for pushed
points") and gives the same result at every level.

*Alternative considered:* comparing levels, so that a point's `poll_interval` beats the
connector's `sampling_interval`. This was rejected. It is harder to predict, and it would let an
old point-level `poll_interval` silently cancel a new connector-wide `sampling_interval`.

### 3. The runtime resolves the value; modules only read the hint

- Rust: `push_points` puts the effective sampling interval into `PointRef::interval`, so the
  OPC UA module needs no change to how it reads the hint. The field's doc comment is updated to
  say "effective sampling interval for a pushed point; poll interval for a polled point".
- C: `config.c` resolves a new `tdot_point_t.sampling_interval_s` at load time.
  `sampling_interval_ms` returns it. An unset level is held as a negative sentinel, because `0`
  is a valid value.

### 4. `"0"` means the source's fastest rate and is passed through

`parse_duration("0")` already succeeds. OPC UA defines a requested sampling interval of `0` as
"the fastest practical rate", so the module sends it unchanged. `0` is never a default. It is
documented as possibly much chattier on the wire and in the server's load on its data source.

### 5. The OPC UA publishing interval stays derived, not configurable

The subscription keeps requesting `publishingInterval = min(effective sampling interval)` over
the device's subscribed points, as today. When that minimum is `0`, `0` is requested and the
server revises it to its own minimum. That is what an operator opted into with `"0"`.

*Alternative considered:* a `publishing_interval` key in `[connection]` with a per-device
override. This was rejected. With `queueSize = 1` it only rate-limits notifications, and
`report.min_interval` already does that better: it is per point, works for every connector,
publishes the first change after a quiet spell immediately, and does not affect liveness. A
publishing interval is device-wide, delays even a lone change by up to one interval, and drives
the subscription keep-alive. The dead-subscription window in `check_subscription` is about
`publishing_interval × keep_alive_count + publishing_interval`, so `30s` would mean about 10
minutes before a dead session is noticed. Its only remaining use is cutting traffic between the
connector and the server on a slow link. It can be added later if that need appears.

### 6. Log revised values

Servers may revise both intervals (`revisedSamplingInterval`, `revisedPublishingInterval`). Both
builds log one `info` line per subscription when a revised value differs from the requested one,
with the device, point and both values. The connector does not refuse or retry. Operators then
see why a `100ms` request behaves like `1s`.

## Risks / Trade-offs

- [The C and Rust resolutions drift apart] → Shared unit-test table (the same config inputs and
  expected milliseconds) in both SDKs. The C comment on `sampling_interval_ms` is kept and points
  at the new resolver.
- [`"0"` overloads the server or the network] → Opt-in only. The docs warn about it. The
  revised-value log shows what the server granted.
- [async-opcua or open62541 rejects a zero sampling or publishing interval] → Checked
  (task 4.1). Neither client rejects zero. Both pass the requested doubles through and keep the
  server's *revised* values. async-opcua 0.18 schedules its publish loop at
  `max(revised publishing interval, min_publish_interval)`, and `min_publish_interval` defaults
  to 100 ms (`config.rs`), so zero cannot busy-loop it. open62541 1.5 uses the revised
  publishing interval only for its inactivity check (`ua_client_subscriptions.c`). The server
  replaces zero with its fastest supported rate, as OPC UA Part 4 requires. No mapping is
  needed.
- [Operators expect `sampling_interval` to speed up polled points] → The contract and the
  OPC UA spec say plainly that it applies only to pushed points. `subscribe = false` remains the
  way to put a node back on the polling schedule.
- [`"0"` on one point makes the whole device's subscription publish at the server's minimum] →
  Documented with the `"0"` warning. The other points still sample at their own rate, and
  `queueSize = 1` sends at most one value per point per publish.
- [A fast sampling interval shortens the keep-alive window] → The subscription's keep-alive
  window scales with the publishing interval (about ×20), so a device at 200 ms notices a silent
  server within about 4 s. In the C runtime, which handles devices one at a time and blocks on
  each call, that device's teardown and reconnect attempts then delay how soon *other* devices on
  the same unreachable server are noticed. This is an existing property of the C loop, not new
  behaviour, but the e2e suite keeps the sampling device on its own simulator because of it.
- [Reload] → A changed `sampling_interval` already takes effect,
  because a reload that changes devices reconnects them and re-creates the subscription. A task
  verifies this rather than assuming it.

## Migration Plan

No migration. Existing files have neither key and resolve to today's parameters. To roll back,
remove the keys. Packaged configs are unchanged except for a commented-out example in the OPC UA
demo/config.

## Open Questions

- Should the CLI (`describe`) show each point's effective poll and sampling interval? It does
  not show `poll_interval` today, so this would be a separate, small addition. It is left out of
  this change unless it turns out to be needed to debug the e2e test.
- The one-shot `read` CLI command (`impl/rust/src/main.rs` `effective_interval`) only polls, so
  it keeps using `poll_interval`. No change.
