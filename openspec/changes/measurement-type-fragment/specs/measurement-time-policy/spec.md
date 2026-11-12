## MODIFIED Requirements

### Requirement: Grouping policy setting
The `grouping` table SHALL be accepted at connector, device and point level and in point
libraries, merged key by key like `report`. It SHALL accept:

- `by`: `"time"` (default), `"frame"` or `"sample"`
- `window`: a duration from 1 ms to 60 s, required with `"frame"`
- `every`: a duration from 100 ms to 1 h, required with `"sample"`
- `aggregate`: one of `"mean"`, `"min"`, `"max"` or `"last"`, or a non-empty list of them without
  repeats, used only with `"sample"`

The `batch` table SHALL be accepted as a deprecated alias of `grouping`, with `group_by` in place
of `by`, and SHALL log a warning at load. A level that sets both `batch` and `grouping` SHALL be
refused with a message naming both.

A table that sets `aggregate` without `by` SHALL be treated as setting `by = "sample"` at that
level, so `grouping = { aggregate = "mean", every = "1s" }` is a complete policy. A table that
sets `by` SHALL be refused when it also sets the other policy's duration, with a message naming
the key. A table that sets `by` to `"time"` or `"frame"` SHALL be refused when it also sets
`aggregate`. A duration or an `aggregate` inherited from another level that the effective `by`
does not use SHALL be ignored, so a point can choose `"time"` under a device that chose `"frame"`
or an aggregated `"sample"`. A point whose effective policy is `"sample"` with an `aggregate`
SHALL be refused when its effective `report` policy is not passthrough, with a message naming the
point and both keys. The policy SHALL apply only in grouped output. The capability descriptor
SHALL list each device's effective `output` and `grouping`, including `aggregate` when it is set.

#### Scenario: Missing window
- **WHEN** a device sets `grouping = { by = "frame" }`
- **THEN** the config is refused with a message naming the device and `window`

#### Scenario: Point overrides device
- **WHEN** a device sets `by = "frame"` and one point sets `grouping = { by = "time" }`
- **THEN** that point's readings are grouped by exact time and the others by frame

#### Scenario: Deprecated batch table
- **WHEN** a device sets `batch = { group_by = "frame", window = "500ms" }`
- **THEN** its points are grouped by frame as with `grouping = { by = "frame", window = "500ms" }`, and a deprecation warning is logged

#### Scenario: Both tables at one level
- **WHEN** a device sets both `batch` and `grouping`
- **THEN** the config is refused with a message naming the device, `batch` and `grouping`

#### Scenario: Aggregate without sample
- **WHEN** a device sets `grouping = { by = "frame", window = "100ms", aggregate = "mean" }`
- **THEN** the config is refused with a message naming the device and `aggregate`

#### Scenario: Aggregate implies sample
- **WHEN** a device sets `grouping = { by = "frame", window = "500ms" }` and one point sets `grouping = { aggregate = "mean", every = "1s" }`
- **THEN** that point is sampled every second with its time-weighted mean, and the other points are grouped by frame

#### Scenario: Aggregate without a period
- **WHEN** a device sets `grouping = { aggregate = "mean" }` and no level sets `every`
- **THEN** the config is refused with a message naming the device and `every`

#### Scenario: Aggregate with a report policy
- **WHEN** a device sets `grouping = { by = "sample", every = "1s", aggregate = "mean" }` and one of its points sets `report = { deadband = 0.5 }`
- **THEN** the config is refused with a message naming the point, `aggregate` and `report`
