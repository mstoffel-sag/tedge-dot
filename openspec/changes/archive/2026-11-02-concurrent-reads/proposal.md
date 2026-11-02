## Why

Both runtimes read one block at a time: the Rust runtime calls one `read_points` at a time across
every device (the `Connector` trait takes `&mut self`), the C runtime runs one blocking loop, and
the OPC UA module sends a device's 500-node blocks one after another. A connector's throughput is
therefore capped by round-trip time, not CPU. Measured on 2026-11-02 (M1 Max, details in
design.md):

- 64 bench devices at 20 ms per 500-point block: one connector reaches 22,917 readings/s (Rust) and
  19,667 (C) while using 4–7 % of a core; four connector processes reach 58,417 and 59,667.
- One OPC UA device with 20,000 nodes, 20 ms round trip: 19,484 readings/s with one Read in flight,
  77,278 with four, against the bench's OPC UA server.

Real PLCs answer a 500-node read in tens of milliseconds, so the 100,000 readings/s goal is out
of reach for a single connector today however cheap the rest of the pipeline is. The serial C loop
also lets one frozen device delay every other device's reads and outage detection.

## What Changes

- **Concurrent reads across devices** in both runtimes. Devices are read in parallel, each device
  still one read at a time and in order. All runtime state (report policy, batcher, link status,
  MQTT) stays on the runtime's own loop; only the module's read call runs concurrently.
- **Pipelined reads within a device** for modules whose protocol allows several requests in
  flight: OPC UA in both builds, and SNMP where its library allows. A `read_points` call may send
  up to the allowance the runtime gives it, and its results stay in order, with one read time.
- **A per-endpoint budget of requests in flight**, `max_inflight` (default 4), shared by every
  device on the same endpoint and by the blocks of a pipelined read. A module names each device's
  endpoint (host and port, serial port, CAN interface) and caps it: a serial bus or a CAN bus is
  always 1. Devices sharing an endpoint use the smallest `max_inflight` among them.
- **Backoff**: an endpoint that answers "too busy" (OPC UA `BadServerTooBusy` or
  `BadResourceUnavailable`) or times out while other requests are in flight has its
  budget halved, and regains one step after a run of successful reads.
- **A total cap** on reads in flight across all endpoints, `max_inflight_total` (default 32), which
  bounds the C build's worker threads.
- New `[connector]` and `[[device]]` keys; contract 0.4.0. The capability descriptor lists each
  endpoint's budget.
- Bench: the bench module gains endpoints, pipelined blocks and a "busy" server limit, and a new
  `polled-latency` scenario sets the target.
- **Behavior change**: with the defaults, devices on different endpoints are read in parallel and
  an OPC UA device sends up to four Reads at once. `max_inflight = 1` restores the old behavior
  for an endpoint.

## Capabilities

### New Capabilities

- `concurrent-reads`: concurrent reads across devices, pipelined reads within a device, the
  per-endpoint budget, the total cap, endpoint identity per module, and the backoff.

### Modified Capabilities

- `bench-connector`: "Polled reads with modelled latency" gains endpoints, pipelining within the
  allowance, and a server limit that answers "too busy".
- `performance-harness`: "Named scenarios and baseline" adds the `polled-latency` scenario.

## Impact

- Rust SDK: `runtime.rs` poll path (per-device read tasks with results returned to the loop), a new
  optional `Connector` method that hands out a per-device reader so reads can run concurrently, an
  `endpoint` method, the budget and backoff, and the config keys in `config.rs`/`library.rs`.
- Rust modules: OPC UA (reader over the shared `Arc<Session>`, pipelined `read_in_blocks`),
  Modbus, SNMP (readers; SNMP pipelining if `snmp2` allows), bench; CAN, CANopen and PROFIBUS
  only name their endpoint (bus, capped at 1).
- C SDK: worker threads for module reads with a result queue drained by the main loop, a module
  flag for reads safe on different devices at once, an `endpoint` hook, the budget and backoff, and
  the config keys.
- C modules: OPC UA (`UA_Client_sendAsyncReadRequest` for pipelining), Modbus, SNMP, bench.
- Writes and commands for a device wait for its read in flight, so a device is never read and
  written at once.
- Contract 0.4.0, schemas, docs (`doc/performance.md`, a guide section on concurrency), a
  `changes/` highlight, vectors or unit tests for the budget and backoff in both builds, e2e.
