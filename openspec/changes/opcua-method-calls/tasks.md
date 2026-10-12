## 1. Contract and schemas

- [ ] 1.1 Contract §3.1 and §3.3: add `call_method` to the `access` values, state that `write` and `call_method` points are never read, and list the value fields a `call_method` point must not declare plus the connector-support rule. Verify by reading the rendered section against the `point-access` spec.
- [ ] 1.2 Contract §6: add a "`call-method` verb" section (request `arguments`, result `outputs`, access checks, no retries, reporting reset), and mention it under "Other verbs" and in §7 `command_verbs`. Verify the examples match the spec scenarios.
- [ ] 1.3 Add `call_method` to the `access` enums of `config.schema.json` and `point-library.schema.json`; add the `call-method` request and result shapes to `command.schema.json`. Verify the schemas are valid JSON and that the conformance tool's schema tests pass (`just test -p ot-conformance`).

## 2. Rust SDK

- [ ] 2.1 `Access::CallMethod` with `parse`/`access_str` support and `can_read()`; add `call_method` to `ACCESSES` in `library.rs`. Unit tests for parsing and `can_read()`/`can_write()` of all four values.
- [ ] 2.2 Reject value fields on a resolved `call_method` point (design D3) in `library.rs`. Unit tests: each refused field inline and via a library point, and inherited `poll_interval`/`report` still accepted.
- [ ] 2.3 Filter `build_schedule` and `push_points` on `can_read()`. Unit test: a `write` and a `call_method` point are absent from the schedule and the push set; `read_write` is present.
- [ ] 2.4 Add `sdk::configure(connector, config)` that refuses `call_method` points unless `command_verbs` lists `call-method`, and use it at every `configure` call site in `runtime.rs` and `impl/rust/src/main.rs`. Unit test with a stub connector with and without the verb.
- [ ] 2.5 `call-method` branch in `handle_command`: point lookup, access check, `arguments` validation, `executing`, `execute(device, "call-method", …)` bounded like a write, `outputs` in the result, `origin` echo, reporting reset. Unit tests with a recording connector for success, a non-method point, a non-array `arguments`, and a connector error.
- [ ] 2.6 CLI `read` wildcard uses `can_read()`. Update the existing `resolve_targets` test to include a `call_method` point.
- [ ] 2.7 `just test` and `just lint` pass.

## 3. C SDK

- [ ] 3.1 `TDOT_ACCESS_CALL`, parsing of `call_method` in `config.c`, `call_method` in `ACCESSES`, and `call_method` in the envelope/descriptor access strings. Unit test in `impl/c/tests/config.c`.
- [ ] 3.2 Reject value fields on a resolved `call_method` point, mirroring 2.2. Unit tests mirroring 2.2's cases.
- [ ] 3.3 Optional `call_method` hook in `connector.h`; `tdot_connector_configure()` that refuses `call_method` points when the hook is NULL; use it in `runtime.c` and `src/main.c`. Unit test with a stub connector.
- [ ] 3.4 `handle_call_method` in `runtime.c` (mirrors `handle_write`, resets reporting state) and the `call-method` dispatch. Verify the CLI wildcard read in `src/main.c` already skips non-readable points.
- [ ] 3.5 `just c-test` passes.

## 4. OPC UA connector — Rust

- [ ] 4.1 Address parsing: `object_id`, `method_id`, `inputs` on `NodeAddress`; validation for method and variable points both ways (spec "Method addressing"); no datatype required for method points. Unit tests for each scenario.
- [ ] 4.2 `MethodTarget` on `OpcuaPoint`; translate the object id as a second mapped entry; skip method points in `resolve_types` and subscription grouping. Unit test: both ids translated through a moved namespace, and a missing namespace reported.
- [ ] 4.3 `execute("call-method")`: argument count/type checks, `Session::call` with timeout, status and input-argument-result handling, output rendering (design D8). Unit tests for argument conversion and output rendering, including 64-bit strings, arrays and unsupported types as `null`.
- [ ] 4.4 Add `call-method` to `command_verbs`. `cargo test -p connector-opcua` and `just lint` pass.

## 5. OPC UA connector — C

- [ ] 5.1 `UA_ENABLE_METHODCALLS ON` in `impl/c/CMakeLists.txt`; record the binary size before and after for the PR.
- [ ] 5.2 Address parsing and validation mirroring 4.1, in `configure`.
- [ ] 5.3 Method fields on `ua_point_t`; factor the per-node translation into a helper and translate both ids; skip method points in `subscribe_device`.
- [ ] 5.4 `call_method` hook with `UA_Client_call`, the failure reasons of design D8, and output rendering; add `call-method` to the descriptor's `command_verbs`. `just c-test` passes.

## 6. Simulator, end-to-end tests and docs

- [ ] 6.1 Simulator: `Machine` object with `Multiply`, `Ping` and `Fail` (design D10). Verify with a local asyncua client call or the e2e suite.
- [ ] 6.2 `connectors/opcua/connector.toml`: method points and the write-only `setpoint_wo` point; check that the conformance configs still load.
- [ ] 6.3 `opcua_e2e.robot`: the cases listed in design D10. `just test-e2e opcua` and `just test-e2e-c opcua` pass.
- [ ] 6.4 `doc/connectors/opcua-connector-spec.md`: a "Method calls" section (addressing, arguments, outputs, failure reasons, namespace tables) and `call-method` in §2 Capability descriptor. Verify the TOML example in the doc loads with `tedge-dot describe`.

## 7. Integration

- [ ] 7.1 `just test`, `just lint`, `just c-test`, `just c-describe-parity`, and both OPC UA e2e suites pass on the branch; `openspec validate opcua-method-calls --strict` passes.
