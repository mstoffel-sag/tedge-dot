## ADDED Requirements

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

A table that sets `group_by` SHALL be refused when it also sets the other policy's duration,
with a message naming the key. A duration inherited from another level that the effective
`group_by` does not use SHALL be ignored, so a point can choose `"time"` under a device that
chose `"frame"`. The policy SHALL apply only in measurement mode. The capability descriptor SHALL list each
device's effective `output` and `batch`.

#### Scenario: Missing window
- **WHEN** a device sets `batch = { group_by = "frame" }`
- **THEN** the config is refused with a message naming the device and `window`

#### Scenario: Point overrides device
- **WHEN** a device sets `group_by = "frame"` and one point sets `batch = { group_by = "time" }`
- **THEN** that point's readings are grouped by exact time and the others by frame

### Requirement: Group by exact time
Under `group_by = "time"`, readings SHALL share a message only when their timestamps are equal
at the contract's millisecond precision. No reading SHALL be dropped and no timestamp moved. An
open message SHALL be published when a reading with a different time arrives for its group, or
at the end of the runtime pass, whichever comes first.

#### Scenario: Pushed times differ
- **WHEN** three pushed readings of one group have times `.913`, `.913` and `.914`
- **THEN** two messages are published, with two series at `.913` and one at `.914`

### Requirement: Group by frame
Under `group_by = "frame"`, readings that arrive within one window of the device clock SHALL
share a message stamped with the first reading's time. A reading of a point already in the
message SHALL publish that message at once and start a new one in the same window. No reading
SHALL be dropped. Every other message SHALL be published when its window ends, and a reading
that arrives after the window ended SHALL open a new window even when the ended one is not
published yet. A message larger than the size budget SHALL be split, each part keeping the
message's time. A reading's published time SHALL differ from its own timestamp by less than the
window.

#### Scenario: Frame groups pushed readings
- **WHEN** `window = "100ms"` and 400 pushed readings of distinct points arrive within one window
- **THEN** they are published together, stamped with the first reading's time, within 100 ms after the window opened plus one runtime pass

#### Scenario: Nothing overwritten
- **WHEN** a point changes three times within one window
- **THEN** all three values are published, in three messages

### Requirement: Sample and hold
Under `group_by = "sample"`, the runtime SHALL publish every `every` one series for each slot
of the device that holds a good value, stamped with the sample time, whether or not it changed.
A slot SHALL be left out while it is stale: its device link is down, its pushed subscription
fails `check_subscription`, or its last reading was not good. Readings superseded between two
samples SHALL NOT be published. Event points (`meta.event` with `every`) SHALL NOT be sampled;
each of their readings SHALL be published with its own time.

#### Scenario: Fixed rate
- **WHEN** `every = "1s"` and a device has 1,000 pushed points that change 20 times per second
- **THEN** about 1,000 series are published each second, all with the sample time

#### Scenario: Unhealthy subscription
- **WHEN** a pushed point's subscription check fails
- **THEN** the point is left out of the samples until the check passes again

#### Scenario: Events keep every occurrence
- **WHEN** an event point fires three times between two samples
- **THEN** three series are published, each with its own time

### Requirement: Report policy applies first
The reporting policy (contract §5.3) and value mapping (§4.3) SHALL be applied before a reading
enters the point table. A reading the policy withholds SHALL NOT change the slot and SHALL NOT
be published as a series or a sample. A held reading SHALL enter the table when the policy
releases it, with its own time. The heartbeat SHALL keep its fresh-read rule; under `"sample"`
a heartbeat reading refreshes the slot like any other reading.

#### Scenario: Deadband withholds a series
- **WHEN** a point has `report = { deadband = 0.5 }` and its value moves by 0.2
- **THEN** no series carries the new value; under `"time"` and `"frame"` nothing is published for that reading, and under `"sample"` the next sample repeats the last reported value
