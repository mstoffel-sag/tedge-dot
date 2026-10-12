## Why

The OPC UA connector can read, subscribe to and write variables, but it cannot call methods.
Methods are how many OPC UA servers expose actions: resetting a counter, starting a batch,
acknowledging a fault, loading a recipe. Without method calls those actions are unreachable from
thin-edge.io.

A method is not a value, so it does not fit the current point model. Every configured point is
read: the Rust runtime polls or subscribes to every point whatever its `access`. A method node
declared as an ordinary point would therefore be read on every cycle. Reading a method node's
`Value` attribute fails with `BadAttributeIdInvalid`, so the connector would publish a bad sample
each time.

That polling behaviour is also a bug for write-only points today. The contract says a write-only
point "never samples" (§5.2, §7 `parameter_keys`). The C runtime already skips points it cannot
read, but the Rust runtime polls them too. OPC UA defines write-only the same way: a node with
`CurrentWrite` but without `CurrentRead` answers a Read with `BadNotReadable`.

## What Changes

- Add a fourth point **`access`** value, **`call_method`**, to the contract and to both SDKs. A
  `call_method` point is never read, subscribed or sampled, cannot be written, and is never a
  parameter. Declaring value-related fields on it (`datatype`, `mode`, `transform`, `map`,
  `poll_interval`, `sampling_interval`, `subscribe`, `report`, `endianness`, `word_order`,
  `meta.parameter`) is a configuration error.
- Make **`access = "write"` mean never read** in the Rust runtime too: no polling, no
  subscription, no heartbeat read. This matches the contract and the C runtime. The CLI `read`
  wildcard already skips write-only points; it now also skips `call_method` points.
  **BREAKING** for the Rust build only: a configuration that relied on Rust polling a
  write-only point no longer gets those samples. Such a point should be declared `read_write`.
- Add a **`call-method`** command verb on `te/device/<device>/ot/<protocol>/cmd/call-method/<id>`.
  The request carries `point` and an `arguments` array; a successful result carries an `outputs`
  array. The SDK runtimes route the verb, refuse it for any point whose access is not
  `call_method`, and reset the device's reporting state afterwards as they do after a write.
  `write` and `write-batch` refuse `call_method` points.
- A configuration with `call_method` points is rejected at load when the connector does not
  support the `call-method` verb, so only the OPC UA connector accepts such points for now.
- **OPC UA**, in both builds: a `call_method` point is addressed as
  `address = { object_id = "...", method_id = "...", inputs = ["uint32", "string"] }`. The
  connector converts the arguments to the declared input types, calls the method with the OPC UA
  `Call` service, and returns the output arguments as JSON. Both node ids are translated through
  the device's namespace table like any other node id. A method call is never retried.
- The C build needs no open62541 change: its client Call service is compiled in already.
- The capability descriptor gains **`methods`**: every `call_method` point with its address, so
  a consumer learns which methods a device has. The `ot-registration` flow turns it into the
  thin-edge `ot_call_method` command capability of each device with methods, so the cloud
  mapper can route method calls to it; the existing command flows already forward the command.
- The OPC UA simulator gains methods to call, and the e2e suite covers method calls and
  write-only points in both builds.

## Capabilities

### New Capabilities

- `point-access`: what each point `access` value allows (read, write, method call), that points
  which cannot be read are never read, and the protocol-neutral `call-method` command verb.
- `opcua-method-calls`: how the OPC UA connector addresses, calls and reports the result of an
  OPC UA method.

### Modified Capabilities

None.

## Impact

- **Contract**: `doc/contract/ot-connector-contract.md` §3.1, §3.3, §5, §6 (new `call-method`
  verb section), §7; JSON schemas `config.schema.json`, `point-library.schema.json`,
  `sample.schema.json` (no change to the sample's `access` enum, since such points never sample),
  `command.schema.json`.
- **Rust SDK** (`impl/rust/crates/sdk`): `Access`, point validation, poll schedule and push point
  selection, command routing.
- **Rust CLI** (`impl/rust/src/main.rs`): `read` wildcard filter.
- **C SDK** (`impl/c/sdk`): access bits, point validation, a new optional `call_method` hook on
  `tdot_connector_t`, command dispatch.
- **OPC UA connector**: `impl/rust/crates/connector-opcua`, `impl/c/connectors/opcua`,
  `impl/c/CMakeLists.txt`.
- **Docs**: `doc/connectors/opcua-connector-spec.md`.
- **Tests**: `connectors/opcua/sim/server.py`, `connectors/opcua/connector.toml`,
  `connectors/opcua/tests/opcua_e2e.robot`, unit tests in both SDKs and both OPC UA connectors.
- No new dependencies.
