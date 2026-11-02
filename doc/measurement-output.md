# Measurement output: batched measurements from the connector

By default a connector publishes one sample per reading, and the `ot-measurement` flow in the
thin-edge.io mapper turns each sample into a measurement. That works for a few thousand readings
per second. Every sample is a message, and every message runs JavaScript in the mapper: about
0.5 ms of mapper CPU per reading with the `ot-*` flows installed.

With **measurement output** the connector publishes the thin-edge measurements itself, hundreds
of readings per message, and publishes samples only for the points a flow still needs. No
JavaScript runs for a measurement, and the mapper only converts it. In the performance harness
(`doc/performance.md`) this took the mapper from about 145 % of a core at 4,000 readings/s to
about 10 % at 100,000 readings/s.

The normative rules are in [contract §5.5](contract/ot-connector-contract.md#55-measurement-output).
This guide explains how to use them.

## Switching it on

Set `output` in the `[connector]` table and leave the flows installed:

```toml
[connector]
protocol = "opcua"
output   = "measurements"
```

Both builds support it, for every protocol, and nothing else in the configuration changes. To
switch back, remove the key or set `output = "samples"`. The flows still handle samples.

What changes on the broker:

| | `output = "samples"` (default) | `output = "measurements"` |
| --- | --- | --- |
| Measurements on `te/device/<device>///m/<group>` | One per reading, from `ot-measurement` | Batched, from the connector |
| Units on `te/device/<device>///m/<group>/meta` | From `ot-measurement` | From the connector |
| Samples on `te/device/<device>/ot/<protocol>/sample/<point>` | Every reading | Only for points that flows read, or `sample = true` |
| Link status, capability descriptor, commands | Unchanged | Unchanged |

## Naming

A reading becomes the series `<group>.<series>` with the rules the `ot-measurement` flow uses, so
the cloud sees the same names in both modes:

1. the point's `meta.measurement.group` and `meta.measurement.series`
2. the `[connector]` table's `measurement.group` and `measurement.series`
3. the point id split at `measurement.point_separator`, when one is set and the id contains it
4. the protocol id as the group and the point id as the series

```toml
[connector]
protocol    = "modbus"
output      = "measurements"
measurement = { point_separator = "." }   # "Environment.Temperature" -> Environment / Temperature

[[device]]
name = "plc1"
protocol_address = { transport = "tcp", host = "10.0.0.5", port = 502, unit_id = 1 }

  [[device.point]]
  id       = "boiler_temp"
  datatype = "float32"
  unit     = "°C"
  address  = { table = "holding", address = 7, count = 2 }
  meta     = { measurement = { group = "Boiler", series = "Temperature" } }
```

Only a `good` reading of a number or a boolean becomes a series. A boolean is published as 1 or 0
unless `measurement.include_boolean = false`. Strings, 64-bit integers outside the JavaScript safe
range, `raw` points and points with `meta.measurement = false` never become series.

The `[connector] measurement` keys are the `ot-measurement` flow's parameters of the same names.
The flow's own `params.toml` does not apply to a connector in measurement output, so copy any
`group`, `series`, `point_separator` or `include_boolean` you set there into the connector's
configuration.

## Timestamps and grouping

A thin-edge measurement has one `time`, so readings share a message only when they share a time.
Which time a reading has, and how readings are grouped, is the point's `batch` policy. Set it on
`[connector]`, a device or a point; it is merged key by key like `report`.

| `group_by` | Message time | Loss | Use it for |
| --- | --- | --- | --- |
| `"time"` (default) | The reading's own time | None, and no time is moved | Polled points. A polled pass shares one read time, so it is one message per device and group. |
| `"frame"`, `window = "100ms"` | The first reading's time in the window | None. A time moves by less than `window` | Pushed points whose source times differ by a few milliseconds |
| `"sample"`, `every = "1s"` | The sample time | Readings superseded between two samples | A fixed rate per point, whatever the source sends |

### `time`: exact times

Readings with the same timestamp, at millisecond precision, go into one message. A polled read
returns all its points with the completion time of the read call, so a pass over a device is one
message per group. Pushed values carry the time the source reports (an OPC UA source timestamp),
so pushed points batch only as far as their times coincide. No reading is lost and no time is
changed.

### `frame`: a window of the device clock

```toml
[[device]]
name  = "line1"
batch = { group_by = "frame", window = "100ms" }
```

Readings that arrive within `window` share a message stamped with the first reading's time. A
point that changes twice within a window starts a new message, so both values reach the cloud.
The price is the time: a reading can be published with a time up to `window` earlier than its
own. Choose a window shorter than the time resolution you care about.

### `sample`: sample and hold

```toml
[[device]]
name  = "line1"
batch = { group_by = "sample", every = "1s" }
```

Every `every`, the connector publishes the latest good value of every point, stamped with the
sample time, whether the value changed or not. Readings between two samples are not published.
A point is left out while its value cannot be trusted: its device link is not `connected`, its
subscription failed its check, or its last reading was not `good`. Points that raise an event
for every reading (`meta.event.every`) are still published one by one, with their own times.

### Combining with `report`

The reporting policy (`report`, see [reducing data volume](reducing-data-volume.md)) runs first.
A reading it withholds never reaches the measurements. Under `"sample"` the last reported value
is sampled again, so `report` and `"sample"` together give a fixed rate of values that changed
meaningfully.

## Samples and the flows

In measurement output a point still publishes samples when a flow reads them:

- `meta.alarm` (the `ot-alarm` flow) or `meta.event` (`ot-event`)
- `meta.parameter`, or `write` or `read_write` access (`ot-parameter-state`)

Such a sample carries `"measured": true` when the reading also became a series, and
`ot-measurement` skips it, so the cloud receives each value once.

Another consumer of the samples, such as your own flow or a tool subscribed to the sample
topics, sets `sample = true` on the points it reads. `sample = false` stops a point's samples
even when a flow would read them.

```toml
  [[device.point]]
  id     = "spindle_load"
  datatype = "float32"
  address  = { node_id = "ns=2;s=Spindle.Load" }
  sample   = true          # read by a custom flow
```

### Measurement-series alarms and events

`ot-alarm` and `ot-event` used to subscribe to every measurement, for their measurement-series
mode: one series watched with a threshold, or an event on every change of one series. With the
connector publishing batches, that would run both flows on every batch. That mode is now the
separate flows `ot-alarm-series` and `ot-event-series`, with the same parameters:

- The package installs them from `/usr/share/tedge-dot/flows/` into the mapper only when an
  existing `ot-alarm` or `ot-event` `params.toml` sets `series`, and copies that params file over.
- To use one on a new installation, copy its directory into
  `/etc/tedge/mappers/c8y/flows/` and write its `params.toml` from the template.
- Declaring the alarm on the point (`meta.alarm`) is the cheaper option: it works in both output
  modes and needs no flow on the measurements.

## Checking it

The capability descriptor on `te/device/main/service/<service>/ot/capabilities` lists each
device's output and grouping policy:

```json
"outputs": [
  { "device": "line1", "output": "measurements", "batch": { "group_by": "frame", "window": "100ms" } }
]
```

To see the measurements and their units:

```sh
tedge mqtt sub 'te/device/+///m/#'
```

A message is split before its Cumulocity size passes about 15,000 bytes, so a device with
thousands of points in one group sends several messages per pass, each with the same `time`.
The mapper rejects nothing: its limit is 16,184 bytes.

## Limits

- The `ot-measurement` flow's deprecated filters (`on_change`, `deadband`, `min_interval`,
  `debounce`) and its `combine` mode do not apply to a connector in measurement output. Use the
  point's `report` table and `batch` instead.
- `tedge-dot run --output stdout` prints samples in both modes.
- Units are published per measurement topic from the connector's configuration. Two connectors
  that publish the same device and group overwrite each other's units message; give their points
  different groups.
