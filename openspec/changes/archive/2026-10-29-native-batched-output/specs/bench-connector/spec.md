## MODIFIED Requirements

### Requirement: Source timestamps on pushed samples
In both builds, the device `protocol_address` SHALL accept `ts_jitter` (a duration, default
0), `ts_resolution` (a duration of at least 1 ms, default 1 ms) and `publish_interval` (a
duration, default 0). A pushed sample SHALL carry a source time: its generation time minus a
uniform draw from [0, `ts_jitter`], floored to `ts_resolution`. The draws SHALL come from a
per-device stream seeded with FNV-1a 64 over `<device>\0#ts`, so setting these keys does not
change any point's values. With `publish_interval` above 0, samples SHALL be delivered together
once per interval, each keeping its own source time. Polled samples SHALL carry the read time.

#### Scenario: Timestamps precede delivery
- **WHEN** a device sets `ts_jitter = "50ms"` and `publish_interval = "100ms"`
- **THEN** every pushed sample's `ts` is at most 50 ms before its generation time and at most 150 ms before it is published, and samples are published in groups about 100 ms apart, in both builds

#### Scenario: Resolution makes timestamps coincide
- **WHEN** `ts_resolution = "10ms"` is set
- **THEN** every pushed sample's `ts` is a multiple of 10 ms
