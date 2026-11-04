## ADDED Requirements

### Requirement: Replayed flag on pushed samples
Both SDKs SHALL let a module mark a pushed sample as replayed (`Sample::replayed` in Rust,
`replayed` in `tdot_sample_t` in C, false by default). A replayed sample SHALL carry a source time;
the runtime SHALL treat a replayed sample without one as not replayed and log a `warn` once per
point. The flag SHALL NOT appear in the sample envelope.

#### Scenario: Default is live
- **WHEN** a module pushes a sample without setting the flag
- **THEN** the runtime handles it exactly as before this change

#### Scenario: Replayed without source time
- **WHEN** a module pushes a replayed sample with no source time
- **THEN** the runtime handles it as a live sample and logs one `warn` for the point

### Requirement: Reporting policy runs on source time for replayed samples
For a replayed sample, the runtime SHALL measure `min_interval` and `debounce` (contract §5.3)
against the sample's source time instead of the runtime clock, so the published samples are the
ones the policy would have published had the events arrived live. A reading held or debounced
within a replay SHALL be published at the point of the replay where its interval ends, with its
own `ts`. The replay of a point SHALL end at its first sample that is not replayed, or 2 seconds
of runtime clock after its last replayed sample; a reading still held or debouncing SHALL then
keep waiting with its timer restarted on the runtime clock. `on_change`, `deadband`, the
quality rule and the always-published rules SHALL apply unchanged. Both SDKs SHALL pass the same
replay test vectors in `doc/contract/test-vectors/report/`.

#### Scenario: Min interval over a replay
- **WHEN** 600 replayed samples one second apart in source time, each with a new value, are offered within one runtime-clock second to a point with `min_interval = "10s"`
- **THEN** about 60 samples are published, each at least 10 seconds of source time after the previous one

#### Scenario: Debounce over a replay
- **WHEN** a replayed value changes and stays unchanged for 30 seconds of source time, on a point with `debounce = "20s"`
- **THEN** the settled value is published during the replay

#### Scenario: Hand-over to live
- **WHEN** a replay ends with a reading held by `min_interval` and no live sample follows
- **THEN** the held reading is published `min_interval` after the replay ended on the runtime clock

#### Scenario: Replay pauses between pages
- **WHEN** a replay pauses for more than 2 seconds between two pages of history
- **THEN** the next replayed sample resumes on source time from the last one published in the replay

### Requirement: No heartbeat reads during a replay
The runtime SHALL NOT start a heartbeat read (`max_interval`, contract §5.3 rule 4) for a pushed
point while that point replays.

#### Scenario: Heartbeat waits for live data
- **WHEN** a point replays history for longer than its `max_interval`
- **THEN** no on-demand read of that point is made until its replay ends
