## MODIFIED Requirements

### Requirement: Polled reads with modelled latency
`read_points` SHALL return one good-quality sample per requested point. The device
`protocol_address` SHALL accept `latency`, a duration (default 0), and `batch`, a point count
(default unlimited). A call SHALL take `latency` for each block of up to `batch` points before
returning. In the C build the wait SHALL block the caller, as a blocking protocol call would.

The module SHALL support concurrent reads of different devices in both builds. Within one call,
up to the call's allowance of blocks SHALL wait their `latency` at the same time, as pipelined
requests would. `protocol_address` SHALL also accept `endpoint`, a string naming the device's
endpoint (default: the device name), so that devices can share one, and `server_inflight`, a
positive integer (default unlimited): the most blocks the endpoint serves at the same time,
counting every device on it. A block beyond it SHALL be refused as busy, after `latency`, as a
server answering `BadServerTooBusy` would.

#### Scenario: Latency per block
- **WHEN** a device sets `latency = "2ms"` and `batch = 500` and 2,000 points are read in one call with an allowance of 1
- **THEN** the call takes at least 8 ms in either build

#### Scenario: No latency by default
- **WHEN** a device sets neither key
- **THEN** reading 10,000 points returns without waiting

#### Scenario: Pipelined blocks
- **WHEN** a device sets `latency = "20ms"` and `batch = 500` and 2,000 points are read in one call with an allowance of 4
- **THEN** the call takes at least 20 ms and less than 40 ms in either build

#### Scenario: A busy endpoint
- **WHEN** two devices share `endpoint = "plc"` with `server_inflight = 2` and both are read with an allowance of 2
- **THEN** at most two blocks are served at once, and the blocks beyond are refused as busy
