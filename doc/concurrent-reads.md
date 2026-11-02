# Concurrent reads: reading devices in parallel

A connector spends most of a poll waiting for answers. A PLC answers a 500-point read in tens
of milliseconds, so a connector that reads one device after another, one request at a time,
stops at a few tens of thousands of readings per second however fast the gateway is. With 20 ms
per 500-point read, one connector used to reach about 23,000 readings/s while using 5 % of a
CPU core.

So a connector reads its devices in parallel, and sends several requests to one OPC UA server
at once. Both builds do this by default. A per-endpoint budget keeps any one PLC from being sent
more than it can take.

The normative rules are in [contract §5.6](contract/ot-connector-contract.md#56-concurrent-reads).
This guide explains how to use them.

## What runs in parallel

| Protocol | Devices in parallel | Several requests to one device |
| --- | --- | --- |
| OPC UA | Yes | Yes: up to the budget, Reads of 500 nodes (or the server's `MaxNodesPerRead`) |
| Modbus TCP | Yes | No: one request at a time per device |
| Modbus RTU | No: one serial bus, one request at a time | No |
| SNMP | Yes | No: one request at a time per agent |
| CAN, CANopen, PROFIBUS | No: one bus | No |

Whatever runs in parallel, a device is never read twice at once, its readings keep their order,
and every point of one read has one read time, so measurement output still publishes one
measurement per device and pass ([measurement output](measurement-output.md)). Writes, commands
and method calls are not concurrent: they wait for the device's read in flight.

## Endpoints and their budget

Every device has an **endpoint**: the host and port it talks to (OPC UA, Modbus TCP, SNMP), its
serial port (Modbus RTU), its CAN interface or its bus. Devices on the same endpoint share one
**budget** of requests in flight:

```toml
[connector]
protocol           = "opcua"
max_inflight       = 4    # requests in flight to one endpoint (the default)
max_inflight_total = 32   # requests in flight across all endpoints (the default)

[[device]]
name             = "line1"
protocol_address = { endpoint = "opc.tcp://10.0.0.5:4840/" }

[[device]]
name             = "line2"
protocol_address = { endpoint = "opc.tcp://10.0.0.5:4840/" }   # the same server: shares its 4

[[device]]
name             = "press"
protocol_address = { endpoint = "opc.tcp://10.0.0.9:4840/" }
max_inflight     = 1      # a small PLC: one request at a time
```

- `line1` and `line2` are one endpoint, `10.0.0.5:4840`: together they have at most 4 requests
  in flight, and each of them always gets at least one.
- `press` is its own endpoint with a budget of 1, but it is still read in parallel with the
  other two.
- Devices sharing an endpoint use the smallest `max_inflight` among them, so the most fragile
  device on a server sets its limit.
- A serial or CAN bus always carries one request at a time, whatever `max_inflight` says.

The capability descriptor lists each endpoint with its devices and its budget:

```sh
tedge mqtt sub 'te/device/main/service/+/ot/capabilities'
```

```json
"endpoints": [
  { "endpoint": "10.0.0.5:4840", "devices": ["line1", "line2"], "max_inflight": 4 },
  { "endpoint": "10.0.0.9:4840", "devices": ["press"], "max_inflight": 1 }
]
```

## Choosing a budget

The default of 4 was measured against two OPC UA servers, 20,000 nodes, 20 ms round trip:

| Requests in flight | A server that handles requests in parallel | A server that handles one at a time |
| --- | --- | --- |
| 1 | 19,500 readings/s | 14,700 readings/s |
| 4 | 77,300 | 52,200 (its limit) |
| 8 | 161,700 | 53,700 |

- A small PLC that handles one request at a time gains most of what it can from 4; more only
  queue at the PLC, and each queued request waits for the ones before it.
- A large server that handles requests in parallel keeps gaining. Raise `max_inflight` for it
  if you need more readings per second.
- The gateway's CPU is not what limits this: a connector waiting for answers is idle, so the
  budget does not depend on how many cores the gateway has.

## When a server is busy

A server that cannot keep up answers `BadServerTooBusy` or `BadResourceUnavailable`, or a request
times out behind others. The connector then halves the endpoint's budget, down to 1, and sends
the refused request again. After 20 requests answered in a row it raises the budget by one, up
to `max_inflight`. Each change is logged:

```text
info  endpoint 10.0.0.5:4840: endpoint busy: requests in flight lowered to 2
info  endpoint 10.0.0.5:4840: requests in flight raised to 3
```

A busy server slows a pass down; it does not produce `bad` readings, unless a request is still
refused after 8 attempts. If you see the budget lowered often, set `max_inflight` to the budget
it settles at.

## Going back to one request at a time

```toml
[connector]
max_inflight = 1
```

sends one request at a time to every endpoint, as before this release. Devices on different
endpoints are still read in parallel. Set it on a single device to apply it to that device's
endpoint only.
