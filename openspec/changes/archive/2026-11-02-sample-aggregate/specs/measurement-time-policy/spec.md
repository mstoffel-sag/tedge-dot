## MODIFIED Requirements

### Requirement: Grouping policy setting
The `batch` table SHALL be accepted at connector, device and point level and in point
libraries, merged key by key like `report`. It SHALL accept:

- `group_by`: `"time"` (default), `"frame"` or `"sample"`
- `window`: a duration from 1 ms to 60 s, required with `"frame"`
- `every`: a duration from 100 ms to 1 h, required with `"sample"`
- `aggregate`: one of `"mean"`, `"min"`, `"max"` or `"last"`, or a non-empty list of them without
  repeats, used only with `"sample"`

A table that sets `aggregate` without `group_by` SHALL be treated as setting
`group_by = "sample"` at that level, so `batch = { aggregate = "mean", every = "1s" }` is a
complete policy. A table that sets `group_by` SHALL be refused when it also sets the other
policy's duration, with a message naming the key. A table that sets `group_by` to `"time"` or
`"frame"` SHALL be refused when it also sets `aggregate`. A duration or an `aggregate` inherited from another level
that the effective `group_by` does not use SHALL be ignored, so a point can choose `"time"` under
a device that chose `"frame"` or an aggregated `"sample"`. A point whose effective policy is
`"sample"` with an `aggregate` SHALL be refused when its effective `report` policy is not
passthrough, with a message naming the point and both keys. The policy SHALL apply only in
measurement mode. The capability descriptor SHALL list each device's effective `output` and
`batch`, including `aggregate` when it is set.

#### Scenario: Missing window
- **WHEN** a device sets `batch = { group_by = "frame" }`
- **THEN** the config is refused with a message naming the device and `window`

#### Scenario: Point overrides device
- **WHEN** a device sets `group_by = "frame"` and one point sets `batch = { group_by = "time" }`
- **THEN** that point's readings are grouped by exact time and the others by frame

#### Scenario: Aggregate without sample
- **WHEN** a device sets `batch = { group_by = "frame", window = "100ms", aggregate = "mean" }`
- **THEN** the config is refused with a message naming the device and `aggregate`

#### Scenario: Aggregate implies sample
- **WHEN** a device sets `batch = { group_by = "frame", window = "500ms" }` and one point sets `batch = { aggregate = "mean", every = "1s" }`
- **THEN** that point is sampled every second with its time-weighted mean, and the other points are grouped by frame

#### Scenario: Aggregate without a period
- **WHEN** a device sets `batch = { aggregate = "mean" }` and no level sets `every`
- **THEN** the config is refused with a message naming the device and `every`

#### Scenario: Aggregate with a report policy
- **WHEN** a device sets `batch = { group_by = "sample", every = "1s", aggregate = "mean" }` and one of its points sets `report = { deadband = 0.5 }`
- **THEN** the config is refused with a message naming the point, `aggregate` and `report`

### Requirement: Sample and hold
Under `group_by = "sample"`, the runtime SHALL publish every `every` one series for each
aggregate of each point of the device that had a good value during the period, stamped with the
sample time, whether or not it changed. Without `aggregate`, the aggregate SHALL be `"last"`.
Time while a point is stale SHALL NOT count: its device link is down, its pushed subscription
fails `check_subscription`, or its last reading was not good. A point with no good time in the
period SHALL be left out of that sample. Event points (`meta.event` with `every`) SHALL NOT be
sampled; each of their readings SHALL be published with its own time.

The aggregates SHALL be computed over the values the point held during the period, on the
device clock. The value current at the start of the period counts from that start. A boolean
SHALL count as 1 or 0.

- `"last"`: the value held at the sample time.
- `"mean"`: the time-weighted mean. Each value counts for the good time it held within the
  period.
- `"min"` and `"max"`: the smallest and largest value held for any good time within the period.

#### Scenario: Fixed rate
- **WHEN** `every = "1s"` and a device has 1,000 pushed points that change 20 times per second
- **THEN** about 1,000 series are published each second, all with the sample time

#### Scenario: Unhealthy subscription
- **WHEN** a pushed point's subscription check fails
- **THEN** the point is left out of the samples until the check passes again

#### Scenario: Events keep every occurrence
- **WHEN** an event point fires three times between two samples
- **THEN** three series are published, each with its own time

#### Scenario: Time-weighted mean of change-only data
- **WHEN** `every = "1s"`, `aggregate = "mean"`, a point holds 10 at the start of the period, and changes to 20 at 900 ms
- **THEN** the sample publishes 11 (10 for 900 ms and 20 for 100 ms), not 15

#### Scenario: Mean of evenly polled readings
- **WHEN** `every = "1s"`, `aggregate = "mean"`, and a point polled every 100 ms reads 1, 2, 3, … 10, the period starting just as it read 1
- **THEN** the sample publishes 5.5

#### Scenario: Min and max include the carried value
- **WHEN** `aggregate = ["min", "max"]`, a point holds 5 at the start of the period, and reads 7 and then 6 within it
- **THEN** the sample publishes `<series>_min` 5 and `<series>_max` 7

#### Scenario: Boolean duty cycle
- **WHEN** `aggregate = "mean"` and a boolean point is true for 250 ms of a 1 s period
- **THEN** the sample publishes 0.25

#### Scenario: Stale time is left out
- **WHEN** `aggregate = "mean"`, a point holds 10 for the first 500 ms of the period, and its link is down for the rest
- **THEN** the sample publishes 10

### Requirement: Report policy applies first
The reporting policy (contract §5.3) and value mapping (§4.3) SHALL be applied before a reading
enters the point table. A reading the policy withholds SHALL NOT change the slot and SHALL NOT
be published as a series or a sample. A held reading SHALL enter the table when the policy
releases it, with its own time. The heartbeat SHALL keep its fresh-read rule; under `"sample"`
a heartbeat reading refreshes the slot like any other reading. A point with an `aggregate` has
no report policy (refused when the configuration loads), so every good reading, after value
mapping, enters its slot.

#### Scenario: Deadband withholds a series
- **WHEN** a point has `report = { deadband = 0.5 }` and its value moves by 0.2
- **THEN** no series carries the new value; under `"time"` and `"frame"` nothing is published for that reading, and under `"sample"` the next sample repeats the last reported value

#### Scenario: Aggregate sees every reading
- **WHEN** a point under `aggregate = "mean"` reads 10, 10.2 and 9.9 within one period
- **THEN** all three readings count in the mean
