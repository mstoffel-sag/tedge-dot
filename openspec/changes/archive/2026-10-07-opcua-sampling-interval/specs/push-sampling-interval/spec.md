## ADDED Requirements

### Requirement: Declared sampling interval
A connector configuration SHALL accept an optional `sampling_interval` key, a duration string,
on `[connector]`, on `[[device]]` and on points. The point key SHALL be accepted inline, in a
point library and in a device type. Both the Rust and the C loader SHALL validate it like
`poll_interval`. A value that is not a duration SHALL be refused with
`sampling_interval must be <duration> ...`, with the same message and place prefix in both
builds. A misspelt key SHALL receive the existing "did you mean" suggestion.

#### Scenario: Accepted at every level
- **WHEN** a configuration sets `sampling_interval = "500ms"` on `[connector]`, `"1s"` on a device and `"200ms"` on one point
- **THEN** both loaders accept it

#### Scenario: Invalid duration
- **WHEN** a point sets `sampling_interval = "fast"`
- **THEN** both loaders refuse the file with the same message naming the point and `sampling_interval`

#### Scenario: Misspelt key
- **WHEN** a device sets `sample_interval = "1s"`
- **THEN** the file is refused with `unknown key 'sample_interval' ... (did you mean 'sampling_interval'?)`

### Requirement: Effective sampling interval
The SDK runtime SHALL resolve each pushed point's effective sampling interval as the first one
set among: the point's `sampling_interval`, the device's `sampling_interval`, the connector's
`sampling_interval`, and the point's effective `poll_interval` (point, then device, then
connector, then `2s`). An explicit `sampling_interval` at any level SHALL take precedence over
every `poll_interval`. The Rust and C runtimes SHALL resolve the same value for the same file.

#### Scenario: Fallback to poll_interval
- **WHEN** no level sets `sampling_interval` and a pushed point's effective `poll_interval` is `5s`
- **THEN** its effective sampling interval is `5s`, as before this change

#### Scenario: Connector sampling beats point poll
- **WHEN** `[connector] sampling_interval = "1s"` and a pushed point sets `poll_interval = "250ms"` and no `sampling_interval`
- **THEN** the point's effective sampling interval is `1s`

#### Scenario: Point sampling beats device sampling
- **WHEN** a device sets `sampling_interval = "1s"` and one of its pushed points sets `sampling_interval = "100ms"`
- **THEN** that point's effective sampling interval is `100ms` and the device's other pushed points use `1s`

### Requirement: Sampling interval applies only to pushed points
The effective sampling interval SHALL be handed to the protocol module only for points delivered
by push. It SHALL NOT change the polling schedule. A polled point, including one with
`subscribe = false` and one the module does not push, SHALL be read at its effective
`poll_interval` whatever its `sampling_interval`.

#### Scenario: Polled point ignores sampling_interval
- **WHEN** a point sets `subscribe = false`, `poll_interval = "10s"` and `sampling_interval = "100ms"`
- **THEN** it is read every 10 s

### Requirement: Fastest-rate request
`sampling_interval = "0"` SHALL be passed to the module as a zero interval. For a module whose
protocol defines zero as "fastest supported rate" (OPC UA), that is what it SHALL request. Zero
SHALL never be a default.

#### Scenario: OPC UA zero
- **WHEN** a subscribed OPC UA point resolves to `sampling_interval = "0"`
- **THEN** its monitored item is created with requested `samplingInterval = 0`

### Requirement: OPC UA monitored-item sampling
The OPC UA connector SHALL create each subscribed point's monitored item with
`samplingInterval` equal to the point's effective sampling interval in milliseconds.
`queueSize` SHALL stay `1` and `discardOldest` SHALL stay `true`. Both builds SHALL request the
same values for the same configuration.

#### Scenario: Fast sampling with slow polling
- **WHEN** an OPC UA device sets `poll_interval = "1h"` and `sampling_interval = "200ms"`, and a subscribed node changes every 500 ms
- **THEN** both builds publish a sample for each change within about one second of it, not once an hour

### Requirement: OPC UA publishing interval is derived
The OPC UA connector SHALL request each device subscription's publishing interval as the minimum
effective sampling interval of that device's subscribed points. A minimum of zero SHALL be
requested as zero. The connector SHALL NOT accept a `publishing_interval` setting. Both builds
SHALL request the same value for the same configuration.

#### Scenario: Fastest point sets the publishing interval
- **WHEN** a device's subscribed points resolve to sampling intervals of `1s` and `250ms`
- **THEN** the subscription is requested with a publishing interval of `250ms`

#### Scenario: Unchanged without sampling_interval
- **WHEN** no level sets `sampling_interval` and a device's subscribed points resolve to poll intervals of `2s` and `5s`
- **THEN** the subscription is requested with a publishing interval of `2s`, as before this change

### Requirement: Revised intervals are logged
The OPC UA connector SHALL log a server's revision of a requested sampling or publishing
interval. When the revised value differs from the requested one, it SHALL log one `info` message per subscription (publishing) or monitored item
(sampling) naming the device, the point where applicable, the requested value and the revised
value. It SHALL keep the subscription as granted.

#### Scenario: Server raises sampling interval
- **WHEN** a point requests `sampling_interval = "50ms"` and the server revises it to `1000` ms
- **THEN** an `info` log names the device, the point, `50ms` requested and `1s` revised, and the point is delivered at the server's rate

### Requirement: Reload applies new intervals
A reload that changes `sampling_interval` SHALL re-create the affected subscriptions with the
new sampling and derived publishing intervals, without restarting the process.

#### Scenario: Reload changes sampling
- **WHEN** a running OPC UA connector's file changes a device's `sampling_interval` from `2s` to `200ms` and is reloaded
- **THEN** the device's monitored items are re-created with `samplingInterval = 200`
