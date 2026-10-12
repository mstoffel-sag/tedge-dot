## Context

See proposal.md for the motivation. The state of the code this design builds on:

- **Access parsing and validation.** Both loaders already reject an `access` outside
  `read | write | read_write` (`ACCESSES` in `impl/rust/crates/sdk/src/library.rs` and
  `impl/c/sdk/src/config.c`), inline and in point libraries. The lenient fallbacks to `read`
  (`Access::parse` in Rust, the `else` branch in `config.c`) are therefore unreachable for loaded
  configs.
- **Which points are read.** The C runtime's poll loop skips a point without
  `TDOT_ACCESS_READ`, and the C OPC UA module subscribes only readable points. The Rust runtime
  filters on nothing but `subscribe`: `build_schedule` polls every point that is not pushed, and
  `push_points` offers every `subscribe` point to the module. So Rust reads write-only points,
  and the contract (§5.2, §7) says they never sample.
- **Commands.** Rust: `handle_command` sends every device verb other than `write-batch` and the
  management verbs through `execute_write`, which applies the inverse transform and map, then
  calls `Connector::execute(device, verb, request)`. A connector refuses verbs it does not
  implement. C: `runtime.c` dispatches by name (`write`, `write-coil`, `write-batch`, management)
  and answers anything else with `unsupported verb`. The C module interface has a `write_point`
  hook but no generic execute.
- **OPC UA node ids and namespace tables.** Rust keeps one `OpcuaPoint` per point with one
  `node_id`; `translate` maps each point id to the session's node id and `effective()` applies it.
  C keeps one `ua_point_t` with one `node_id` and per-session `resolved` / `reason` fields filled
  by `translate_device`.
- **C open62541 build.** `UA_ENABLE_METHODCALLS` is forced `OFF` in `impl/c/CMakeLists.txt`. In
  open62541 v1.5.5 that flag guards only server-side method nodes; the client's Call service is
  compiled in either way.

## Goals / Non-Goals

**Goals:**
- One meaning of `access` across protocols and both builds, with the OPC UA method call as the
  first user of `call_method`.
- Method calls that behave the same in Rust and C: same configuration, same command and result
  shapes, same failure reasons where the libraries allow it.

**Non-Goals:**
- Discovering a method's argument types from its `InputArguments` property. The types are
  declared in the configuration (D5).
- Checking `AccessLevel`, `UserAccessLevel` or `Executable` against the configured `access` at
  connect. The server's answer (`BadNotReadable`, `BadNotWritable`, `BadNotExecutable`,
  `BadUserAccessDenied`) reaches the command result instead.
- Structured (`ExtensionObject`) input arguments, and decoding structured output arguments.
- A `call-method-batch` verb, a CLI subcommand for method calls, and a Cumulocity operation that
  maps onto `call-method`. Each can follow without changing what this change defines.
- Method calls for any protocol other than OPC UA.

## Decisions

### D1: `call_method` is an access value, not a separate `kind`

`access` already states which operations a point allows. A method allows exactly one, so it is a
fourth value. A separate `kind = "method"` field would allow contradictory combinations
(`kind = "method"` with `access = "read_write"`) that both loaders would then have to reject.

Rust: `Access::CallMethod`, with `can_read()` true for `Read | ReadWrite` and `can_write()`
unchanged (`Write | ReadWrite`). C: a third bit, `TDOT_ACCESS_CALL 0x4`, never combined with
`READ` or `WRITE`. Every existing check (`can_write()`, `access & TDOT_ACCESS_WRITE`,
`access & TDOT_ACCESS_READ`) then does the right thing for method points without edits: they are
neither written, read nor treated as parameters.

The name is `call_method` (config values use underscores, like `read_write`) and the verb is
`call-method` (verbs use hyphens, like `write-batch`).

### D2: The runtime, not the module, decides what is read

The Rust runtime filters on `access.can_read()` in `build_schedule` and `push_points`, matching
C. The heartbeat reads of `report.max_interval` come from the same sets, so they follow. The CLI
`read` wildcard's existing `access != Write` test becomes `can_read()`.

Alternative: leave it to each module's `read_points`. Rejected: six Rust modules would each need
the check, and C already does it in the runtime.

### D3: Method points carry no value fields

After a library point and the site's patch are merged, both loaders reject `datatype`, `mode`,
`endianness`, `word_order`, `transform`, `map`, `poll_interval`, `sampling_interval`,
`subscribe`, `report` and `meta.parameter` on a `call_method` point. They would load and do
nothing, which §3.3 forbids. `meta` otherwise stays free-form. The check runs on the resolved
point, where the existing `map`-against-`datatype` check already runs.

`report` and `poll_interval` inherited from `[connector]` or the device are not the point's own
declaration and are not refused.

### D4: Support is checked against the connector at configure time

Only the configuration and the connector together know whether a `call_method` point is usable.
Each SDK gets one helper that runs before the module's `configure` and fails when the
configuration has a `call_method` point and the connector cannot call methods:

- Rust: `sdk::configure(connector, config)` checks `capabilities().command_verbs` for
  `call-method`, then calls `connector.configure(config)`. It replaces the direct calls in the
  runtime (startup, stdout mode, management commands, file reload) and the CLI.
- C: `tdot_connector_configure(conn, cfg, err, errlen)` checks `conn->call_method != NULL`, then
  calls `conn->configure`. It replaces the direct calls in `runtime.c` and `src/main.c`.

Alternative: check in each module's `configure`. Rejected for the same reason as D2.

### D5: Input types are declared in the address

```toml
[[device.point]]
id      = "multiply"
access  = "call_method"
address = { object_id = "ns=2;s=Machine", method_id = "ns=2;s=Machine.Multiply", inputs = ["int32", "int32"] }
```

`inputs` lives in `address` because it is OPC UA-specific, and `address` is delegated to the
connector's schema (§3.3), so no new protocol-neutral key is needed. The types reuse the
contract's scalar datatypes and the existing write encoders (`build_variant` in Rust, the write
switch in `connector_opcua.c`), so arguments are converted exactly as writes are.

Alternative: read the method's `InputArguments` property at connect. That needs no
configuration, but costs a browse and a read per method on every connect, fails on servers that
do not expose the property, and makes a configuration's meaning depend on the server. It can be
added later as a check that logs a mismatch.

`object_id` and `method_id` are textual node ids only. The structured `namespace` + `identifier`
form would need two of each.

### D6: Command shape: `arguments` in, `outputs` out

Request: `{"status":"init","point":"multiply","arguments":[6,7]}`; result:
`{"status":"successful","point":"multiply","outputs":[42]}`. Separate names make clear that
neither is a point value and that both are positional. `value` would suggest the inverse
transform and map apply, which they do not.

- **Rust.** `handle_command` gets a `call-method` branch beside `write-batch`. It checks that the
  point exists and has `Access::CallMethod`, validates that `arguments` is an array, publishes
  `executing`, and calls `connector.execute(device, "call-method", request)` with the arguments
  in `CommandRequest.value`. It does not go through `execute_write`. The module returns the
  outputs array in `CommandResult.value`, and the runtime publishes it as `outputs`. No change
  to the `Connector` trait.
- **C.** A new optional hook,
  `int (*call_method)(self, dev, pt, const cJSON *arguments, cJSON **outputs, char *err, size_t errlen)`.
  `runtime.c` dispatches `call-method` to a `handle_call_method` that mirrors `handle_write`.
  A connector without the hook answers `unsupported verb` as today.

In both, `write` and `write-batch` already refuse a point that `can_write()` / `TDOT_ACCESS_WRITE`
rejects, so a method point is refused with the existing "not writable" reason. Both runtimes call
the existing reporting reset after a `call-method` command, as after a write.

### D7: OPC UA keeps one model per point, with an optional method part

Rust: `OpcuaPoint` gains `method: Option<MethodTarget { object: NodeId, inputs: Vec<DataType> }>`.
For a method point, `node_id` holds the method id, so `addr`, `translate` and `effective()` keep
working on it unchanged. The object id is translated as a second mapped entry keyed
`"<point>\u{0}object"`, which no point id can collide with.

C: `ua_point_t` gains `is_method`, `object_id`, `object_ns`, `object_resolved`, `inputs[]` and
`ninputs`. `translate_device`'s per-node body moves into a helper used for both ids.

Neither build subscribes, reads or resolves types for a method point. The runtime already never
offers one (D2). The module skips them when it walks its own points (`resolve_types`,
`subscribe`'s grouping, the C `subscribe_device` loop).

### D8: Calling and reporting

Rust uses `Session::call` with one `CallMethodRequest`. C uses `UA_Client_Service_call` with one
`UA_CallRequest` (`UA_Client_call` returns only the overall status, so it cannot name a rejected
argument). Both are bounded
by `request_timeout_s`. Failure reasons, in order:

- `device <name> is not connected`
- the namespace translation reason
- `expected N arguments, got M`, and `argument K: <conversion error>` (1-based), before any call
- `call failed: <service status>`, with a timeout reported as `call failed: BadTimeout` in both
  builds (open62541 reports its own timeout that way)
- `call rejected: argument K: <status>` when an input argument result is not good
- `call rejected: <status>` for the call's own status

Outputs: scalar variants use the decoder samples use (`variant_to_value` in Rust, the read
decoder in C), with 64-bit integers outside ±2^53−1 as strings. Arrays of such scalars become
JSON arrays. Everything else is `null`. A method may already have taken effect when its outputs
come back, so an output that cannot be rendered never turns a successful call into a failure.

### D9: open62541 method calls

`UA_ENABLE_METHODCALLS` stays `OFF`. It guards only the server side (method nodes and their
callbacks); the client's Call service builds and links without it, so turning it on would only
add server code the connector never runs. Measured on macOS arm64, the stripped binary is the
same size (1,085,192 bytes) with the flag on and off. The CMake comment now says so.

### D10: Tests

- The simulator adds a `Machine` object with `Multiply(Int32, Int32) -> Int32`,
  `Ping() -> String` and `Fail()`, which answers `BadInvalidState`.
- `connectors/opcua/connector.toml` adds method points for all three, plus `setpoint_wo` with
  `access = "write"` on the `Setpoint` node.
- `opcua_e2e.robot` adds cases for the descriptor, a successful call, a call without inputs or
  outputs, wrong argument count, wrong argument type, a bad method status, `call-method` on a
  variable, `write` on a method point, and no samples from either the method points or the
  write-only point. Every case runs in both builds, so nothing is added to
  `C_MISSING_CAPABILITIES`.
- Unit tests cover access parsing and validation, the read-set filter, the support check, address
  parsing, argument conversion, output rendering and namespace translation of both ids.

## Risks / Trade-offs

- [A Rust user relied on samples from a write-only point] → It is listed as **BREAKING** in the
  proposal and the PR. The fix is `access = "read_write"`. The C build has always behaved this
  way.
- [A method call times out after the server executed it] → The command reports `failed`, but the
  action may have happened. This is inherent in a request/response call. It is documented, and
  the call is never retried (spec "Method calls are not retried").
- [Declared `inputs` drift from the server's method signature] → The server rejects the call with
  `BadTypeMismatch` or `BadArgumentsMissing`, and the reason names the argument.
- [`call-method` is retained like every command topic] → A retained `init` would be replayed by a
  connector that restarts before answering it. This is the same exposure `write` has today.
  Requesters should clear the topic after the terminal state, as the contract already says.

## Migration Plan

No migration step for existing configurations. Rust users with `access = "write"` points whose
samples they used switch them to `read_write`. Rollback is reverting the change; configurations
using `call_method` then fail to load with the existing "unknown access" error.
