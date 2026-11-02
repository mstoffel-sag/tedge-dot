## Context

A connector reads every device and every block one after another:

```
one loop, one module object
 pass:  plc1 [blk][blk]  plc2 [blk][blk]  ...  plc50 [blk]       each [blk] = one round trip
 ceiling = points per block / (round trip + server time), whatever the CPU
```

- Rust: `Connector::read_points(&mut self, ...)`, so the runtime calls one read at a time for all
  devices (`runtime.rs`, the poll path). The OPC UA module's `read_in_blocks` awaits each 500-node
  Read before sending the next, although `async_opcua::Session::read` takes `&self` on an
  `Arc<Session>` and can have several Reads in flight.
- C: one serial loop with blocking module calls; MQTT runs in the same thread. A frozen device
  delays every other device (memory: C loop device starvation). The vendored open62541 offers
  `UA_Client_sendAsyncReadRequest`, driven by `UA_Client_run_iterate`, which the module already
  calls for subscriptions. The SNMP module already uses `snmp_sess_async_send`.

### Measurements (2026-11-02, M1 Max, scratch tooling, `--no-record`)

**Across devices.** The bench module with `latency = "20ms"` per `batch = 500` points, 64 devices
of 1,000 points, `poll_interval = "1s"`, measurement output, target 64,000 readings/s. Separate
connector processes stand in for concurrent reads:

| Instances | Rust | C | Connector CPU |
| --- | --- | --- | --- |
| 1 | 22,917/s | 19,667/s | 4–7 % |
| 2 | 46,167 | 39,042 | 7–14 % |
| 4 | 58,417 | 59,667 | 7–19 % |
| 8 | 57,667 | 62,375 | 7–19 % |

Throughput scales almost linearly until the pass fits the poll interval (64 devices × 2 blocks ×
20 ms needs about three reads in parallel). The connector is idle, waiting. Rust's remaining 9 %
shortfall at 4 and 8 instances is the late-poll issue, not this one.

**Within a device.** A scratch `async-opcua` client read 500-node blocks with K Reads in flight,
through a proxy that delays each direction by half the round trip.

The bench's server (`ot-conformance serve opcua`, async-opcua, multi-threaded), 20,000 nodes:

| K | 0 ms round trip | 20 ms | 100 ms |
| --- | --- | --- | --- |
| 1 | 130,351/s | 19,484/s | 4,735/s |
| 4 | 535,867 | 77,278 | 18,519 |
| 8 | 911,868 | 161,698 | 37,536 |
| 64 | 2,926,775 | 664,399 | 185,791 |

The e2e simulator (Python `asyncua` in its container, single-threaded, about 5 ms per 500-node
Read), 5,000 nodes, standing in for a small PLC that serves one request at a time:

| K | 0 ms round trip | 20 ms |
| --- | --- | --- |
| 1 | 57,627/s | 14,704/s |
| 2 | 84,829 | 32,025 |
| 4 | 94,048 | 52,239 |
| 8 | 95,046 | 53,746 |
| 16 | 77,699 | 65,152 |

- Against a serial server, four Reads in flight reach its ceiling; more only queue, and 16 was
  slower than 4 with no network delay. Against a parallel server, more keeps paying.
- Neither server refused a request, even at K = 64. The backoff (D6) is therefore not validated by
  these servers; the bench module's server limit (D8) is how it gets tested.
- Connector CPU stayed at 4–19 % in every run: the work is waiting, so the gateway's CPU count is
  not what limits it (D4).

## Goals / Non-Goals

**Goals:**
- One connector reaches 50,000 readings/s or more with 20 ms per block, against about 23,000 today.
- A device is never read concurrently with itself, and its results keep their order and read time.
- No endpoint receives more requests at once than its budget, and an endpoint that says it is busy
  gets fewer.
- One frozen device no longer delays the others.
- The same behavior in both builds.

**Non-Goals:**
- Pipelining for Modbus (libmodbus is synchronous; most devices serve one transaction at a time),
  CANopen SDO (one at a time per node), PROFIBUS and CAN (cyclic or pushed).
- Concurrent writes or method calls. They keep their current order and wait for a device's read.
- The late-poll shortfall (a separate change), although shorter passes may reduce it.
- Concurrency inside the runtime's state (report policy, batcher, link status, MQTT).

## Decisions

### D1. Concurrency only around the module's read call
The runtime's state stays on one loop. A read is started on a worker (a task in Rust, a thread in
C), and its result comes back to the loop, which applies the report policy, the batcher, the link
status and publishing exactly as today.
- Nothing else becomes concurrent, so the existing tests and vectors keep their meaning.
- *Rejected: running the whole per-device path concurrently.* Every piece of runtime state would
  need locking.

### D2. Rust: a per-device reader handed out by the module
A new optional `Connector` method returns, for a device, a `DeviceReader` (`Send`, owning what the
read needs, such as the OPC UA module's `Arc<Session>`) whose `read_points` can be moved into a
task. The runtime asks for a reader after each connect or reconnect and drops it when the device's
link is lost. A module without the method is read as today, one call at a time through the module
object.
- Modules opt in one at a time: OPC UA, Modbus, SNMP and bench first.
- *Rejected: changing every `Connector` method to `&self`.* That refactors all seven modules at
  once for the same result.
- *Rejected: one module instance per device or per shard.* Modules share per-connector resources,
  such as SNMP's trap listener and the CAN socket of an interface, which would conflict.

### D3. C: worker threads, with a module flag
A module sets `concurrent_reads` when its `read_points` (and `read_point`) are safe to call at the
same time for different devices. The runtime then runs reads on a pool of worker threads, at most
`max_inflight_total`, and the main loop collects results from a queue. A pipe wakes the main loop,
so `mosquitto_loop` doesn't wait out its timeout. Without the flag, reads stay on the main loop.
- OPC UA (one `UA_Client` per device), Modbus (one `modbus_t` per device) and SNMP (net-snmp's
  single-session API, `snmp_sess_*`) are the candidates. SNMP's file-level
  `tsnmp_parse_session` is used for parsing only and must be checked or made per-call.
- A device's writes, commands and reconnects take the device's lock, so they wait for its read.
- *Rejected: non-blocking I/O in every module.* A rewrite of each module.

### D4. A per-endpoint budget, not a CPU-based default
- Every device has an **endpoint**, named by its module: an OPC UA endpoint's host and port,
  Modbus TCP's host and port, the serial port for Modbus RTU, an SNMP agent's host and port, the
  CAN interface for CAN and CANopen, the PROFIBUS bus. A module without the hook uses the device
  name, so each device is its own endpoint.
- The module also gives a **cap** for an endpoint: 1 for a serial or CAN bus, unlimited otherwise.
- `max_inflight` (default 4), on `[connector]` or a device, is the number of requests in flight
  to one endpoint. Devices that share an endpoint use the smallest of their values; the module's
  cap always wins.
- `max_inflight_total` (default 32), on `[connector]`, caps requests in flight across all
  endpoints. It bounds the C build's worker threads.
- Four is where the serial server above stops gaining, and a parallel server still gains three to
  four times over one. Connector CPU stayed under 20 % in every measurement, so the gateway's CPU
  count is not used: the budget protects the remote device, which more gateway cores do not make
  faster.

### D5. Sharing the budget between devices and blocks
A read of a device takes one permit of its endpoint, waiting if none is free. It then takes up to
`allowance - 1` more without waiting, where the allowance is what the module says the call can use
(its blocks, at most the budget). The module sends at most that many requests at once, and
returns all permits when the call ends.
- Every device on an endpoint makes progress: none waits for another's pipelined read to finish
  before getting its first permit.
- A module that cannot pipeline always asks for 1.
- The total cap is a second semaphore, taken the same way.

### D6. Backoff on a busy endpoint
An endpoint's effective budget halves (to at least 1) when a request is answered with OPC UA
`BadServerTooBusy` or `BadResourceUnavailable`, or times out while another request to the
endpoint is in flight. `BadTooManyOperations` is not a busy answer: a server returns it for a
Read larger than it accepts, which sending again cannot fix. After 20 successful requests at the reduced budget, it grows
by one, up to `max_inflight`. Each change is logged at `info`.
- A request refused as busy is sent again within the same call once the budget allows, at most
  8 times and within the call's `operation_timeout`. Only then do its points fail, so a busy
  server slows a pass down but does not produce `bad` readings.
- With a server that serves exactly N at a time, the budget oscillates between about N/2 and N
  (halve on a refusal, one step back every 20 successes), like a congestion window.
- A timeout with nothing else in flight is an ordinary link failure, handled as today.
- The descriptor lists each endpoint's configured budget. The budget in effect changes with each
  backoff and is logged, not republished in the retained descriptor.

### D7. Results keep their order and read time
A pipelined call reassembles block results by index. A block that fails fails only its own points.
A lost link stops sending, ends the call after the blocks in flight, and reports
`ConnectorError::LinkLost`, as `read_in_blocks` does today. The runtime stamps the call's read time
when the whole call completes (contract §5.4), so a device's pass is still one measurement under
`group_by = "time"`.

### D8. The bench models endpoints, pipelining and a busy server
The bench module's `protocol_address` gains:
- `endpoint`: devices with the same value share an endpoint.
- Pipelining: blocks within the call's allowance overlap their latency.
- `server_inflight`: the most blocks the simulated endpoint serves at once. A block beyond it is
  answered "busy", which tests D6.

Two scenarios set the targets, since the harness builds uniform device sets:
- `polled-latency`: 64 bench devices on separate endpoints, 20 ms per 500-point block, measurement
  output.
- `opcua-latency`: one OPC UA device with 20,000 nodes, the standalone simulator started with
  `--latency 20ms`, a proxy in front of it that delays each direction by half the round trip
  without limiting throughput.

### D9. Contract 0.4.0
New keys (`max_inflight`, `max_inflight_total`) and a capability-descriptor field, so the minor
version goes up.

## Performance

All lines are in `bench/results/Reubens-MacBook-Pro-2-2026-11-02.jsonl`, from one Apple M1 Max
(thin-edge.io 2.0.2~423, mosquitto 2.1.2), measurement output, three runs per build unless noted.

- `polled-latency`: 64 bench devices × 1,000 points, 20 ms per 500-point block, 1 s polls, a
  target of 64,000 readings/s. Before: `main` at 0c4d61f, built in a separate worktree with the
  scenario added there. After: this change, defaults (`max_inflight = 4`, `max_inflight_total = 32`).
- `opcua-latency`: one OPC UA device with 20,000 polled nodes, the standalone simulator behind a
  20 ms round trip (`--latency 20ms`), a target of 80,000 readings/s (0.25 s polls). `main` has
  no `--latency`, so the before line is this change with `max_inflight = 1`: one Read at a time,
  as before.

Medians of three runs; 0 missing values and 0 `te/errors` in every run.

| Scenario | Build | Before | After (defaults) | Connector CPU before → after |
| --- | --- | --- | --- | --- |
| `polled-latency` | Rust | 21,941 readings/s (65.7 % short) | **64,000** (0.0 % short) | 6.8 % → 9.2 % |
| `polled-latency` | C | 16,471 (74.3 % short) | **64,000** (0.0 % short) | 11.4 % → 18.8 % |
| `opcua-latency` | Rust | 17,662 (`max_inflight = 1`) | **63,377** (20.8 % short) | 6.3 % → 18.1 % |
| `opcua-latency` | C | 17,662 (`max_inflight = 1`) | **64,416** (19.5 % short) | 5.1 % → 13.1 % |
| `opcua-latency`, `max_inflight = 8` (one run) | Rust / C | | **80,000 / 80,000** (0.0 % short) | 19.0 % / 13.3 % |

| Target | Measured | Met |
| --- | --- | --- |
| One instance reaches ≥ 58,000 readings/s on `polled-latency` | 64,000 in both builds | Yes |
| One OPC UA device reaches ≥ 70,000 readings/s at a 20 ms round trip with K = 4 | 63,377 (Rust), 64,416 (C) | No |
| Connector CPU ≤ 25 % | 9.2–18.8 % | Yes |
| No lost readings, no errors | 0 / 0 | Yes |

- `polled-latency` went from 22,000 (Rust) and 16,500 (C) to the full 64,000 readings/s: about
  three and four times. The connector stays mostly idle, and the C build now delivers what the
  Rust build does.
- `opcua-latency` with the default budget of 4 reached 3.6 times the one-Read rate but missed the
  70,000 target. With four Reads in flight at a 20 ms round trip, the scratch client (no decoding,
  no publishing) reached 77,278 readings/s; the connector reaches about 83 % of that, and a pass
  of 40 blocks takes about 315 ms against a 250 ms poll interval. The miss comes from the budget,
  not the connector: with `max_inflight = 8` both builds read the full 80,000 readings/s at under
  20 % of a core. The default stays at 4 because the serial server measured in Context gains
  nothing beyond it; a site with a server that handles requests in parallel raises it, and the
  guide says so.
- Building the C pool exposed two costs that were not concurrency itself: the loop scanned every
  point of every idle device on each pass (31 % → 19 % of a core once a device's next due time is
  kept), and `UA_Client_run_iterate` runs for its whole timeout (10 ms → 1 ms: 51,930 → 70,175
  readings/s in a single check run).
- Grid scheduling (polls one interval after they were due, not after the loop reached them) took
  Rust's single-instance `polled-latency` from 53,333 (16.7 % short) to 64,000 in a single check
  run, and removes the 11 % late-poll shortfall noted in `doc/performance.md`.

## Risks / Trade-offs

- **[A small PLC is overloaded by default]** → Four in flight reached the serial server's
  ceiling without errors. The backoff halves on "busy" or a timeout with other requests in flight,
  and `max_inflight = 1` restores today's behavior per endpoint.
- **[Timeouts count queueing time]** → With K requests queued at a serial server, the last waits
  for the K - 1 before it. `operation_timeout` (5 s) leaves ample room at K = 4 even at 100 ms per
  request. A timeout with others in flight backs off rather than marking the link lost.
- **[Module thread safety in C]** → Opt-in per module (D3). A module is flagged only after review,
  and the CI runs its e2e suite with concurrency on and under AddressSanitizer in the unit tests.
- **[Two logical devices on one PLC]** → They share the endpoint's budget (D4), so the PLC sees
  the same at most 4 as with one device.
- **[Session limits]** → Concurrency is per device's existing session; no new sessions are opened.
- **[The C build's threads]** → At most `max_inflight_total` workers, created on demand.

## Migration Plan

With the defaults, devices on different endpoints are read in parallel and OPC UA devices send up
to four Reads at once. A site that needs today's behavior sets `max_inflight = 1` on
`[connector]`. Downgrading restores the serial loop. Nothing is stored.

### D10. SNMP is not pipelined in the Rust build
`snmp2`'s `AsyncSession::get` takes `&mut self` and answers into the session's own buffer, so one
session carries one request at a time. Pipelining would mean sharing one socket and matching
responses to request ids ourselves. The Rust SNMP module therefore gets concurrency across agents
only: its reader shares the device's session behind a lock. Answered in task 4.4.

## Open Questions

None.
