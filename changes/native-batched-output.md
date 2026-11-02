### Measurement output: batched measurements from the connector

A connector can now publish thin-edge measurements itself, hundreds of readings per message,
instead of one sample per reading for the `ot-measurement` flow to convert. Set
`output = "measurements"` in the `[connector]` table. No flow runs for a measurement, which takes
most of the work off the mapper at high reading rates. See
[doc/measurement-output.md](../doc/measurement-output.md).

- Opt-in: without `output`, a connector publishes samples exactly as before.
- `batch = { group_by = ... }` chooses how readings with different timestamps are grouped:
  `"time"` (default, nothing moved or lost), `"frame"` (a window) or `"sample"` (a fixed rate).
- Samples are still published for points with alarms, events or parameters, and for points with
  `sample = true`.
- The OT Connector Contract is now version 0.2.0 (§5.4, §5.5, `measured`, `outputs`). The
  additions are optional and do not appear in the default output.

**Behavior and API changes.** These apply without `output = "measurements"`:

- **Action needed if you use the measurement-series alarm or event** (a `series` in the
  `ot-alarm` or `ot-event` `params.toml`): that mode moved to the new `ot-alarm-series` and
  `ot-event-series` flows, and `ot-alarm` and `ot-event` no longer subscribe to measurements.
  The package installs the new flow with your params file on upgrade; check that
  `/etc/tedge/mappers/c8y/flows/ot-alarm-series/` (or `ot-event-series/`) exists. If you
  install the flows by hand, copy the new flow from `/usr/share/tedge-dot/flows/`.
- Every sample of one read call now has the same `ts`, the time the call completed. In the C
  build, a module without `read_points` (Modbus, for example) uses one time per device pass.
  Points that were read together used to have times a few milliseconds apart.
- Pushed OPC UA values in the C build now carry the server's source timestamp instead of the
  time they arrived, as in the Rust build.
- For module authors: the Rust `Sample` has a new public field, `source_ts`. A module that builds
  a `Sample` with a struct literal must add `source_ts: None` (or the protocol's own time). The
  C `tdot_sample_t` has a new last member, `source_ts_ms`, so modules must be recompiled against
  the new header; modules that call `tdot_sample_init` need no source change.
