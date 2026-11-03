### Config workbench

The config builder and the report-policy playground are now one page, the
[config workbench](https://thin-edge.github.io/tedge-dot/tools/workbench/) (the old links
redirect). Build a connector configuration and try it before you deploy it, in three steps that
share one state:

1. **Signals**: up to four simulated signals, each with a behaviour and an optional outage, or
   one of the examples.
2. **Configuration**: the config builder, for every protocol, with a point for each signal. Set
   `report` and `batch` on `[connector]` or on a point, and a point's `poll_interval`,
   `sampling_interval` or `subscribe`; a point inherits what it does not set, as on the gateway.
   A demo config runs as well: its point library (`points_from`) is resolved as the connector
   does. Points and less used sections fold away.
3. **Outcome**: which package runs the configuration, what each point's effective `report`
   policy publishes, and how [measurement output](../doc/measurement-output.md) batches it: the
   three `batch` policies (`time`, `frame` and `sample`, with or without aggregates) side by
   side, which signals each measurement is missing, which samples are not sent, and the
   measurement Cumulocity stores.

Its report and batching engines pass the same test vectors as the Rust and C SDKs.

The config builder can now set `output`, `max_inflight`, `max_inflight_total`, `batch` (with
`aggregate`), and per point `poll_interval`, `sampling_interval`, `subscribe`, `report` and
`batch`.
