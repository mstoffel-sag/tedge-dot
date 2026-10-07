## 1. Contract and docs first

- [x] 1.1 Add `sampling_interval` to `doc/contract/ot-connector-contract.md`: the `[connector]`/`[[device]]` examples and keys (§3.1), the point field table (§3.2), the resolution order, "pushed points only", and `"0"` = fastest supported rate
- [x] 1.2 Add a "Subscription timing" section to `doc/connectors/opcua-connector-spec.md`: sampling vs publishing, worst-case latency ≈ sampling + publishing, a worked example with defaults (2 s + 2 s), the `"0"` warning, the revised-value log, and that the publishing interval is the fastest sampling interval of the device's subscribed points. Point to `report.min_interval` for rate limiting
- [x] 1.3 Add a short pointer in `doc/reducing-data-volume.md`: the report policy filters what arrives, and `sampling_interval` decides how often a pushed point is sampled

## 2. Rust SDK

- [x] 2.1 Add `sampling_interval: Option<String>` to the connector, device and point config structs (`impl/rust/crates/sdk/src/config.rs`), including the point-library and device-type merge paths
- [x] 2.2 Add `sampling_interval` to `CONNECTOR_KEYS`, `DEVICE_KEYS` and `POINT_KEYS` and `check_duration` it at every level (`impl/rust/crates/sdk/src/library.rs`)
- [x] 2.3 Add a resolver `effective_sampling_interval(config, device, point) -> Duration` implementing design §2. Use it in `push_points` (`runtime.rs`) for `PointRef::interval`
- [x] 2.4 Update the `PointRef::interval` doc comment (`connector.rs`): the sampling interval for a pushed point, the poll interval for a polled one
- [x] 2.5 Unit tests: the resolution table (fallback, connector beats point poll, point beats device, `"0"`), invalid duration message, "did you mean" for `sample_interval`, and a polled point (`subscribe = false`) keeping its poll interval in `build_schedule`
- [x] 2.6 Check that `set-config` / `define-device` accept `sampling_interval` (it is not local-only) and that a reload with a changed value reconnects the device (`needs_restart` stays false). Add a test if not covered

## 3. C SDK

- [x] 3.1 Add `sampling_interval_s` to the connector, device and point structs in `impl/c/sdk/include/tedge_dot/config.h`, with a negative sentinel for "unset"
- [x] 3.2 Add the key to the strict-key lists in `impl/c/sdk/src/config.c`, `check_duration` it at every level (inline, library, device type) with the Rust messages, and resolve the point's effective value at load time per design §2
- [x] 3.3 C unit tests (`impl/c/tests/config.c`) mirroring 2.5, with the same inputs and expected values as the Rust table

## 4. OPC UA connector

- [x] 4.1 Verify that async-opcua and open62541 accept a requested `samplingInterval = 0` and a publishing interval of `0`. Record the result in design §Risks. If either rejects `0`, map it to that library's minimum in both builds and update the spec
- [x] 4.2 Rust (`connector-opcua/src/lib.rs`): keep the publishing interval as min(sampling), including `0`. Keep `DEFAULT_SAMPLING_INTERVAL` only as an unreachable fallback, or remove it
- [x] 4.3 C (`connector_opcua.c`): `sampling_interval_ms` returns `pt->sampling_interval_s * 1000`, and the publishing interval matches 4.2. Update the parity comment
- [x] 4.4 Both builds: log `info` once per subscription/monitored item when `revisedPublishingInterval`/`revisedSamplingInterval` differs from the requested value (device, point, requested, revised)
- [x] 4.5 Unit tests for the requested parameters: per-point sampling interval and the derived publishing interval (min, zero, fallback to poll interval), the same in both builds
- [x] 4.6 Add a commented-out `sampling_interval` example to the packaged and demo OPC UA configs

## 5. End-to-end and parity

- [x] 5.1 Add an e2e case to `connectors/opcua/tests/opcua_e2e.robot`: device `poll_interval = "1h"`, `sampling_interval = "200ms"`, and a node the simulator changes every 500 ms (`OPCUA_SIM_DYNAMIC`). Assert that several distinct samples arrive within a few seconds. opc3 talks to its own `sampling-simulator` service: on the shared simulator, its 4 s keep-alive window made it the first device to notice a frozen server, and the C runtime's blocking teardown and reconnects then pushed `opc2` past the deadline of "Push Delivery Recovers From A Silent Server"
- [x] 5.2 Add an e2e case for the fallback: no `sampling_interval`, so behaviour is unchanged (the existing subscribe tests still pass)
- [x] 5.3 Run `just test`, `just test-e2e opcua` and `just test-e2e-c opcua`, and the OPC UA conformance suites for both IMPLs. Rebuild images first (see the e2e Docker cache gotcha)
- [x] 5.4 Run `openspec validate opcua-sampling-interval` and fix any findings
