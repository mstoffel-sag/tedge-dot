# measurement-time-policy Specification

## Purpose
Which time a reading carries and how a connector in measurement output groups readings with different times into one measurement: exact time, a frame window, or sample and hold.
## Requirements
### Requirement: Source time in samples
A module SHALL be able to give a sample a source time in both builds. The envelope `ts` and a
measurement's `time` SHALL be the source time when the sample has one, else its read time. The
C OPC UA module SHALL set the source time from a data value's source timestamp, else its server
timestamp, as the Rust module does. A C module that sets no source time SHALL behave as it does
today.

#### Scenario: OPC UA builds agree
- **WHEN** an OPC UA subscription delivers a value with source timestamp `10:00:00.913Z`
- **THEN** both builds publish it with `ts` `2026-…T10:00:00.913Z`

#### Scenario: Modbus unchanged
- **WHEN** a Modbus point is read
- **THEN** its `ts` is the read time, as before

### Requirement: One read time per read call
All samples returned by one `read_points` call without a source time SHALL share one read
time: the time the call completed. For a module that reads one point per call, the poll pass
over a device SHALL count as one call, timed at its first read's completion. A module SHALL NOT
set the read time itself.

#### Scenario: Polled pass shares a time
- **WHEN** one `read_points` call returns 500 samples over 30 ms of block reads
- **THEN** all 500 carry the same `ts`

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

### Requirement: Group by exact time
Under `group_by = "time"`, readings SHALL share a message only when their timestamps are equal
at the contract's millisecond precision. No reading SHALL be dropped and no timestamp moved. An
open message SHALL be published when a reading with a different time arrives for its group, or
at the end of the runtime pass, whichever comes first.

#### Scenario: Pushed times differ
- **WHEN** three pushed readings of one group have times `.913`, `.913` and `.914`
- **THEN** two messages are published, with two series at `.913` and one at `.914`

### Requirement: Group by frame
Under `group_by = "frame"`, the readings that arrive within one window of the device clock SHALL
be published as the frame's **layers**. A reading SHALL go into the first layer that does not
hold its point yet: a point's first value in the window goes into layer 0, its second into
layer 1, and so on. No reading SHALL be dropped. The layers SHALL be published when the window
ends, in layer order, and a reading that arrives after the window ended SHALL open a new window
even when the ended one is not published yet. A reading that would open a 65th layer SHALL
publish the frame's layers at once and start a new frame in the same window. A layer larger than
the size budget SHALL be split, each part keeping the layer's time.

A layer's time SHALL be its first reading's time, unless that is not later than the time of the
previous layer of the same topic and grouping policy, in this frame or an earlier one. In that
case it SHALL be 1 ms after that previous time, so layer times of one topic and policy strictly
increase; the parts of a split layer share its time. Apart from that move, a reading's published
time SHALL differ from its own timestamp by less than the window.

#### Scenario: Frame groups pushed readings
- **WHEN** `window = "100ms"` and 400 pushed readings of distinct points arrive within one window
- **THEN** they are published together, stamped with the first reading's time, within 100 ms after the window opened plus one runtime pass

#### Scenario: A repeat goes into the next layer
- **WHEN** readings of points A, B, A and C arrive in that order within one window
- **THEN** at the end of the window the connector publishes one message holding A's first value, B and C, then one message holding A's second value

#### Scenario: Nothing overwritten
- **WHEN** a point changes three times within one window
- **THEN** all three values are published, in three messages, in the order they arrived

#### Scenario: Times stay in order when source times jitter
- **WHEN** layer 0 of a frame is stamped 12:00:00.140 and the first reading of layer 1 carries the source time 12:00:00.125
- **THEN** layer 1 is stamped 12:00:00.141, and every point's values reach the cloud in the order they arrived

#### Scenario: Polled passes keep their read times
- **WHEN** a device is polled every 200 ms under `window = "500ms"`, each pass sharing one read time
- **THEN** each pass after the first in the window goes into its own layer, stamped with its own read time

#### Scenario: Layer cap
- **WHEN** one point changes 65 times within one window
- **THEN** the 65th reading publishes the frame's 64 layers at once and opens a new frame in the same window, and no value is lost

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
