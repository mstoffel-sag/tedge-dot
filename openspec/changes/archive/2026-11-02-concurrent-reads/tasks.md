## 1. Bench and baseline

- [x] 1.1 Bench module, both builds: `endpoint` and `server_inflight` in `protocol_address`, pipelined blocks within the call's allowance, busy refusals beyond `server_inflight`; update its tests (latency, pipelining, busy)
- [x] 1.2 Add the `polled-latency` (64 bench devices, 20 ms per 500-point block) and `opcua-latency` (one 20,000-node OPC UA device, the simulator's new `--latency 20ms`) scenarios to `bench/scenarios.toml`, and record `max_inflight` and `max_inflight_total` in the result line
- [x] 1.3 Record the before lines for `polled-latency` and for `opcua-polled` behind a 20 ms round trip (both builds, three runs each), and fill the "Today" column of design.md's Performance table

## 2. Configuration and contract

- [x] 2.1 Rust: accept `max_inflight` on `[connector]` and devices and `max_inflight_total` on `[connector]` (positive integers), in the key lists and point libraries, with messages naming the key and the place
- [x] 2.2 C: the same keys and messages in `config.c`; loader cases in both builds' tests
- [x] 2.3 Contract §3 and a new §5.6 (concurrency, endpoints, budgets, backoff), `config.schema.json` and `status.schema.json` (the descriptor's endpoints), version 0.4.0 and the connector specs that state it

## 3. Budget and backoff (shared logic)

- [x] 3.1 Rust: an endpoint table built from the modules' endpoint names and caps, the smallest `max_inflight` per endpoint, per-endpoint and total semaphores, the take-one-then-more rule, and the backoff state (halve on busy or on a timeout with others in flight, +1 after 20 successes, logged)
- [x] 3.2 C: the same table, counters and backoff, used by the worker pool
- [x] 3.3 Unit tests in both builds for the shared rules: the smallest budget wins, the module cap wins, every device on an endpoint gets a permit, halving and regrowth, a lone timeout is not a backoff
- [x] 3.4 The capability descriptor lists each endpoint with its devices, configured and effective budget, in both builds

## 4. Rust runtime and modules

- [x] 4.1 `Connector`: optional `endpoint(&device) -> (String, Option<usize>)` and `reader(&device) -> Option<Box<dyn DeviceReader>>`; `DeviceReader::read_points(&self, points, allowance)`
- [x] 4.2 Runtime poll path: start each due device's read as a task within the budgets, collect results on the loop in arrival order, keep one read per device, make writes, commands and reconnects wait for the device's read, re-fetch readers after connect and reconnect
- [x] 4.3 OPC UA: a reader over the device's `Arc<Session>`; `read_in_blocks` sends up to the allowance at once and reassembles by index, with today's failure and link-lost rules; busy status codes reported to the backoff; endpoint = the endpoint URL's host and port
- [x] 4.4 Modbus: a reader per device (TCP endpoint = host and port; RTU endpoint = serial port, cap 1); SNMP: a reader per agent, pipelining if `snmp2` allows several requests in flight (resolve the open question in design.md); CAN, CANopen, PROFIBUS: endpoint = interface or bus, cap 1
- [x] 4.5 Runtime tests: a frozen device does not delay others and one read per device (`ot-conformance/tests/concurrent_reads.rs`); results keep their order and read time (OPC UA `pipelined_blocks_overlap_and_retry_when_busy`); a write waiting for a device's read is the drain before every command request, covered by the e2e suites

## 5. C runtime and modules

- [x] 5.1 A worker pool of at most `max_inflight_total` threads, created on demand; a result queue drained by the main loop, woken through a pipe so `mosquitto_loop` returns at once; a per-device lock taken by reads, writes, commands and reconnects
- [x] 5.2 Module API: a `concurrent_reads` flag, an `endpoint` hook returning the name and cap, and the allowance passed to `read_points`
- [x] 5.3 OPC UA: pipelined Reads with `UA_Client_sendAsyncReadRequest` driven by `UA_Client_run_iterate`, reassembled by index; the module flagged after review (one `UA_Client` per device)
- [x] 5.4 Modbus and SNMP flagged after review (`modbus_t` per device; `snmp_sess_*`, and `tsnmp_parse_session` made safe); CAN, CANopen and PROFIBUS name their bus with cap 1
- [x] 5.5 Runtime tests as in 4.5, and the C tests under AddressSanitizer and ThreadSanitizer

## 6. E2E and docs

- [x] 6.1 E2E: two Modbus or OPC UA devices on one simulator with `max_inflight = 1` and the default; a frozen-device case (pause one simulator, the other device keeps its rate); run every protocol's suite on both builds
- [x] 6.2 Docs: a concurrency section in the configuration guide (endpoints, `max_inflight`, the defaults, when to lower them), `doc/performance.md`, and a `changes/concurrent-reads.md` highlight with the behavior change

## 7. Performance

- [x] 7.1 Run `polled-latency` and the 20 ms `opcua-polled` variant after the change, three runs each in both builds, plus a busy endpoint (`server_inflight = 2`), and fill design.md's Performance table with before and after lines from the same host
- [x] 7.2 State whether each target was met, and explain any miss from the measured numbers
- [x] 7.3 Run `just lint`, the Rust workspace tests, `ctest`, the flow tests, conformance and `openspec validate concurrent-reads --strict`
