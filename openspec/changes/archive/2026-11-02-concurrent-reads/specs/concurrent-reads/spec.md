## ADDED Requirements

### Requirement: Devices are read concurrently
Both runtimes SHALL read different devices at the same time when their module supports it, within
the budgets below. A device SHALL never have more than one `read_points` or `read_point` call in
flight, and its writes, commands and reconnects SHALL wait for its read in flight. The report
policy, the measurement batcher, the link status and publishing SHALL run on the runtime's own
loop, in the order each device's results arrive. A module that does not support concurrent reads
SHALL be read one call at a time, as before.

#### Scenario: Latency-bound devices read in parallel
- **WHEN** 64 bench devices on separate endpoints are polled every second with 20 ms per 500-point block and the default budgets
- **THEN** one connector instance publishes at least 58,000 readings/s in either build

#### Scenario: A frozen device does not delay the others
- **WHEN** one device stops answering until its read times out
- **THEN** the other devices' reads keep their poll interval

#### Scenario: One read per device
- **WHEN** a device's read takes longer than its poll interval
- **THEN** its next read starts only after the previous one returned

### Requirement: Pipelined reads within a device
The OPC UA module SHALL send up to the call's allowance of Read requests at once, in both builds;
another module whose protocol allows several requests in flight on one connection MAY do the
same. The results of a call
SHALL be returned in the order of the points requested, a request that fails SHALL fail only its
own points, and a lost link SHALL end the call after the requests in flight and report the link as
lost. The call's read time SHALL be stamped when the call completes (contract §5.4).

#### Scenario: OPC UA blocks in flight
- **WHEN** one OPC UA device has 20,000 polled nodes, the server is `ot-conformance serve opcua` behind a 20 ms round trip, and `max_inflight = 4`
- **THEN** one connector instance reads at least 70,000 readings/s, and each pass is one measurement under `group_by = "time"`

#### Scenario: A failed block
- **WHEN** one of four blocks in flight is refused by the server
- **THEN** only that block's points are published `bad`, and the other blocks' points are good

### Requirement: Per-endpoint budget
Each device SHALL have an endpoint, named by its module: the OPC UA endpoint's host and port,
Modbus TCP's host and port, the serial port for Modbus RTU, the SNMP agent's host and port, the CAN
interface for CAN and CANopen, and the PROFIBUS bus. A module that names none SHALL use the device
name. `max_inflight` (a positive integer, default 4) SHALL be accepted on `[connector]` and on a
device, and SHALL bound the requests in flight to one endpoint, counting every device on it and
every pipelined request. Devices sharing an endpoint SHALL use the smallest `max_inflight` among
them. A module's cap for an endpoint SHALL always apply: 1 for a serial or CAN bus.
`max_inflight_total` (a positive integer, default 32) on `[connector]` SHALL bound the requests in
flight across all endpoints. A read SHALL take one request of its endpoint's budget, waiting if
none is free, and then as many more as are free, up to what the module asks for; every device on
an endpoint SHALL therefore make progress. The capability descriptor SHALL list each endpoint with
its devices and its budget (`max_inflight`, after the module's cap).

#### Scenario: Two devices share a PLC
- **WHEN** two devices name the same OPC UA endpoint, one with `max_inflight = 2` and the other with the default
- **THEN** at most two requests are in flight to that endpoint at any time, and both devices are read every pass

#### Scenario: A serial bus
- **WHEN** three Modbus RTU devices use `/dev/ttyUSB0` and `max_inflight = 4`
- **THEN** they are read one request at a time, as before

#### Scenario: Restoring the old behavior
- **WHEN** `[connector] max_inflight = 1`
- **THEN** each endpoint has at most one request in flight, and devices on different endpoints are still read in parallel

#### Scenario: Invalid values
- **WHEN** `max_inflight = 0` or `max_inflight_total = "many"`
- **THEN** the configuration is refused with a message naming the key and the place

### Requirement: Backoff on a busy endpoint
An endpoint's effective budget SHALL halve, to at least 1, when a request is answered with OPC UA
`BadServerTooBusy` or `BadResourceUnavailable`, or times out while another request to the same
endpoint is in flight. `BadTooManyOperations` SHALL NOT count as busy. After 20 successful requests at a reduced budget it
SHALL grow by one, up to the configured budget. Each change SHALL be logged at `info` with the
endpoint and the new budget. A request refused as busy SHALL be sent again within the same call
once the effective budget allows, at most 8 times and within the call's `operation_timeout`; only
then SHALL its points fail. A timeout with no other request in flight SHALL be handled as a link failure, as before.

#### Scenario: A server that serves two at a time
- **WHEN** a bench endpoint with `server_inflight = 2` is read with `max_inflight = 4`
- **THEN** the effective budget stays between 1 and 3 after the first refusal, no point is published `bad` for it, and each change is logged with the endpoint and the new budget
