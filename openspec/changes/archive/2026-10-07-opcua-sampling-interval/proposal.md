## Why

A pushed (subscribed) OPC UA point is still sampled on a timer, but by the server. Today that
timer is the point's resolved `poll_interval`, and the subscription's publishing interval is the
fastest of those. An operator who switches a node to a subscription to get changes quickly still
gets them at the polling rate: with the default `2s`, a change can take about 4 s to arrive (up to
one sampling interval plus one publishing interval). Nothing in the configuration or the docs says
so. The name `poll_interval` also hides that this setting controls server-side sampling.

## What Changes

- New optional `sampling_interval` (duration string) on `[connector]`, `[[device]]` and points,
  including point libraries and device types. It is resolved point → device → connector, like
  `poll_interval`. It applies only to points delivered by push. A polled point ignores it.
- When no level sets `sampling_interval`, the resolved `poll_interval` is used, as today. Existing
  configurations behave exactly as before.
- `sampling_interval = "0"` asks the source for its fastest sampling rate. For OPC UA this is the
  server's minimum. It is opt-in and never a default.
- The SDK runtime (Rust and C) resolves the effective sampling interval and hands it to the
  module as the per-point sampling hint. The hint keeps its current meaning: the module's sampling
  rate for a pushed point.
- OPC UA: the subscription's publishing interval stays derived, not configurable: the fastest
  effective sampling interval among the device's subscribed points. Rate limiting belongs to the
  point's `report.min_interval`, which is per point and adds no delay to the first change. A
  long publishing interval would also stretch the subscription keep-alive and slow the
  detection of a dead session.
- OPC UA: when the server revises a requested sampling or publishing interval, both builds log
  the requested and revised values.
- Docs: the contract (§3.1, §3.2) and the OPC UA connector spec explain sampling, publishing and
  the latency that follows, with a worked example.
- Not breaking. Nothing changes until a configuration sets `sampling_interval`.

## Capabilities

### New Capabilities
- `push-sampling-interval`: the contract-level `sampling_interval` key (levels, inheritance,
  fallback to `poll_interval`, `"0"`, validation, management commands), how the SDK runtime
  resolves it and hands it to the module, and how the OPC UA connector maps it onto
  CreateMonitoredItems and derives the subscription's publishing interval from it.

### Modified Capabilities
<!-- None: the existing specs (literal-parameters, opcua-pki-management, opcua-secure-connections,
     point-value-mapping) have no requirements about sampling or publishing rates. -->

## Impact

- **Contract**: `doc/contract/ot-connector-contract.md` (§3.1 connector/device keys, §3.2 point
  fields, the known-key lists used for "did you mean" suggestions).
- **Rust SDK**: `impl/rust/crates/sdk/src/config.rs` (new fields), `library.rs` (known keys,
  duration validation, libraries/device types), `runtime.rs` (`push_points` resolution),
  `connector.rs` (`PointRef` docs for the sampling hint).
- **C SDK**: `impl/c/sdk/src/config.c`, `include/tedge_dot/config.h` (`sampling_interval_s` on
  connector, device and point; strict-key lists; validation messages identical to Rust).
- **OPC UA connector**: `impl/rust/crates/connector-opcua/src/lib.rs` (subscribe: sampling interval,
  derived publishing interval, revised-value log) and
  `impl/c/connectors/opcua/connector_opcua.c` (`sampling_interval_ms`, subscription creation).
- **Docs**: `doc/connectors/opcua-connector-spec.md` (§3.1, §3.2, a new section on subscription
  timing), `doc/reducing-data-volume.md` (pointer from the report policy to sampling).
- **Tests**: config unit tests in both SDKs (resolution, fallback, `"0"`, invalid durations, same
  messages), OPC UA unit tests for the requested parameters, and an e2e case run with `IMPL=rust`
  and `IMPL=c` showing a fast `sampling_interval` beating a slow `poll_interval`.
- No new dependencies. No MQTT topic or payload changes.
