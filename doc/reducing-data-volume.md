# Reducing data volume: report by exception

By default a connector publishes every reading of every point: a Modbus register polled every
2 seconds produces 43,200 samples a day, and each of them becomes a measurement in the cloud,
even when the value never moves. Most signals do not need that. A tank level matters when it
changes, a status word matters when it flips, and a temperature matters when it moves by more
than the sensor's noise.

A point's **reporting policy**, the `report` table, tells the connector to publish a reading
only when it matters, and still to publish now and then when nothing changes, so the cloud
knows the device is alive. This is often called *report by exception*.

The normative rules are in [contract §5.3](contract/ot-connector-contract.md#53-reporting-policy-report-by-exception).
This guide explains how to use them.

## What it does

`report` is applied by the **SDK runtime**, in front of the sample topic, the same way for every
connector (Modbus, OPC UA, SNMP, CAN, CANopen, PROFIBUS) and in both the Rust and the C build.
Samples that the policy holds back never reach the broker, so every consumer sees the reduced
stream: the measurement, alarm, event and parameter flows, and any third-party tool that
subscribes to the samples.

| Key | Type | Publishes a reading... |
| --- | --- | --- |
| `on_change` | boolean | ...only when it differs from the last **published** one. |
| `deadband` | number, or `"<p>%"` | ...only when it moved at least this much from the last published one: an absolute amount in the point's units (after `transform`), or `p` percent of the last published value. Implies `on_change`. Numbers only. |
| `min_interval` | duration | ...at most once per interval. A change that arrives sooner is held and **published when the interval ends**, with its own timestamp. |
| `max_interval` | duration | ...even when unchanged, once this long has passed since the last publish (a heartbeat). The heartbeat is always a **fresh reading**, never an old value sent again. |
| `debounce` | duration | ...only once a changed value has stayed the same (within the deadband, if one is set) for this long. Implies `on_change`. |

Durations are strings such as `"500ms"`, `"10s"`, `"5m"` or `"1h"`.

Some readings are **always** published, whatever the policy says:

- the first reading of each point after the connector starts, after a configuration reload or
  management command, after the device reconnects, after a `write` to the device, and after the
  connection to the broker is restored. The policy's memory is not kept across these events, and publishing once is how a
  consumer that missed something catches up;
- any reading whose `quality` differs from the last published one. A read that starts failing
  is reported at once, and so is the recovery. An older good reading that was held back is then
  discarded, so it can never arrive after the newer bad one.

Without a `report` anywhere in the configuration, every reading is published, exactly as
before. The one-shot CLI `tedge-dot read` always prints what it reads and ignores the policy.

## Examples

### Publish on change

A status word, a mode or a coil: send it when it changes.

```toml
[[device.point]]
id       = "pump_running"
datatype = "bool"
address  = { table = "coil", address = 2, count = 1 }
report   = { on_change = true }
```

Booleans, strings, raw values and 64-bit integers are compared exactly. Numbers are compared
after `transform`, so a change is a change in the value the sample carries.

### Deadband: absolute and percent

An analog value with a little noise: send it when it moves by at least 0.5 °C.

```toml
report = { deadband = 0.5 }
```

The difference is always measured from the last **published** value, not from the previous
reading. A slow drift of 0.1 °C per reading is therefore reported once it adds up to 0.5 °C;
it is never hidden forever.

For a value whose scale varies a lot, such as a flow rate, a relative deadband often fits
better: send it when it moves by 2% of its last published value.

```toml
report = { deadband = "2%" }
```

A percent deadband shrinks as the value approaches zero. When the last published value is
exactly 0, any change is published.

### Rate limit, without losing the last change

Send a fast-changing value at most every 10 seconds:

```toml
report = { on_change = true, min_interval = "10s" }
```

A change that arrives within the 10 seconds is held, and a newer one replaces it. When the
interval ends, the held reading is published with its original timestamp, provided it still
differs from the last published value. The last change of a burst is therefore not lost, even
for a pushed point (an OPC UA subscription, an SNMP trap) that receives nothing afterwards.

### How often a pushed point is sampled

The report policy only filters the readings that arrive. For a pushed point, how often a reading
can arrive is up to the source: an OPC UA server samples a subscribed node at the point's
`sampling_interval` (falling back to its `poll_interval`) and sends changes at most that often.
Set `sampling_interval` to get changes faster, and `report` to publish fewer of them. See
[contract §3.1](contract/ot-connector-contract.md#31-common-protocol-neutral-point-fields) and the
[OPC UA spec §3.8](connectors/opcua-connector-spec.md#38-subscription-timing-sampling_interval).

### Heartbeat, and Cumulocity availability

Cumulocity marks a device **unavailable** when it has sent nothing within its required
interval (`c8y_RequiredAvailability`, often 60 minutes). A device whose points all publish on
change can go quiet for hours while it is perfectly healthy. `max_interval` prevents that:

```toml
[connector]
report = { max_interval = "30m" }
```

Once a point has published nothing for 30 minutes, its next **fresh reading** is published even
if it has not changed:

- a **polled** point publishes its next poll;
- a **pushed** point (an OPC UA monitored item) is read on demand, at most once per
  `max_interval`. The read also proves the source is still there: if it fails or times out, the
  runtime publishes a `bad` sample, and the device's link status reacts as it does to a failed
  poll. A heartbeat never hides a dead source behind a replayed value;
- a point that **cannot be read on demand** (a CAN frame, an SNMP trap) gets no heartbeat. It is
  published only when its frame or trap arrives.

The packaged configs for Modbus, OPC UA, CANopen and PROFIBUS set exactly this line, because 30
minutes stays inside the usual 60-minute required interval. A polled point without a change
filter already publishes every reading, so the line only matters for points you give a filter
and for subscribed OPC UA nodes whose value never changes.

A heartbeat is published at the earliest `max_interval` after the last publish and, for a
polled point, at the latest one poll interval after that.

### Debounce a flapping digital input

A float switch or a contact that chatters as it changes state: publish the new state only once
it has held for 2 seconds.

```toml
report = { debounce = "2s" }
```

A reading that changes again within the 2 seconds restarts the wait, and the short-lived value
is never published. When the value settles, the most recent reading of the stable run is
published, with its own timestamp. For a noisy analog value, combine it with a deadband: a
reading then counts as "the same" when it stays within the deadband.

```toml
report = { debounce = "2s", deadband = 0.5 }
```

### Defaults on the connector and the device, and switching them off

`report` can be set on the connector, on a device and on a point. The effective policy of a
point is merged **key by key** from these levels, the more specific level winning for each key
it sets:

1. `[connector]`: every point of every device;
2. `[[device]]`: every point of that device;
3. the point in a point library (see below);
4. the point in the site's configuration.

```toml
[connector]
report = { max_interval = "30m" }                   # heartbeat for everything

[[device]]
name   = "plc-1"
report = { on_change = true }                       # plc-1: only on change

  [[device.point]]
  id     = "boiler_temp"
  report = { deadband = 0.5, min_interval = "10s" } # effective: on_change, deadband 0.5,
                                                    # min_interval 10s, max_interval 30m

  [[device.point]]
  id     = "alarm_count"
  report = { max_interval = "0" }                   # on change, but no heartbeat

  [[device.point]]
  id     = "line_speed"
  report = { on_change = false, deadband = 0 }      # every reading again; heartbeat kept
```

A duration of `"0"` switches off the setting it would inherit, and `on_change = false` together
with `deadband = 0` switches off change detection.

A single `report` table whose `max_interval` is not greater than its `min_interval` is a
configuration error. When the conflict only comes from inheritance, for instance a point with
`min_interval = "1h"` under the connector's `max_interval = "30m"`, the configuration is
accepted, the point's heartbeat is raised to twice its `min_interval`, and a warning names the
point.

### Point libraries

A point library ([RFC 0004](rfc/0004-point-libraries.md)) can declare a sensible policy for a
device type, once for every instance:

```toml
# /usr/share/tedge-dot/points.d/modbus/acme-meter-v2.toml
[[point]]
id        = "flow_rate"
datatype  = "float32"
address   = { table = "input", address = 10, count = 2 }
report    = { deadband = "1%", min_interval = "10s" }
```

A site adjusts one key without repeating the others, because `report` merges key by key with
the library's table:

```toml
[[device]]
name        = "meter-3"
points_from = ["acme-meter-v2"]

  [[device.point]]
  id     = "flow_rate"
  report = { deadband = "0.5%" }        # effective: deadband 0.5%, min_interval 10s
```

## Per-connector notes

What the heartbeat can do depends on whether a point can be read on demand:

| Connector | Points | Heartbeat (`max_interval`) |
| --- | --- | --- |
| Modbus | polled | yes, the next poll |
| OPC UA | polled nodes | yes, the next poll |
| OPC UA | subscribed nodes (monitored items) | yes, a read of the node on demand; a failed read publishes `bad` |
| CANopen | polled over SDO | yes, the next poll |
| PROFIBUS | polled | yes, the next poll |
| SNMP | polled objects | yes, the next poll |
| SNMP | trap and varbind points | no: they cannot be read on demand |
| CAN bus | frame signals | no: a signal is published when its frame arrives |

The change filters, `min_interval` and `debounce` work for every connector and every kind of
point. For a CAN signal sent periodically by the bus, `on_change` or a deadband is often the
biggest saving of all.

## Alarms and events

Because the policy is applied before the sample is published, the alarm and event flows see the
reduced stream too:

- **Keep a deadband smaller than the hysteresis of any alarm on the same signal.** The alarm
  only ever sees published values, which can lag the real value by up to the deadband. With a
  limit of 80 and a hysteresis of 5, a deadband of 10 can leave the alarm standing after the
  value has fallen to 74, because 74 is not far enough from the last published 81 to be sent.
  A deadband well below the hysteresis keeps the alarm's decisions within its own band.
- **Do not give a change filter or a debounce to a point whose every reading is an occurrence**
  (`meta.event = { ..., every = true }`, typically an SNMP trap), because two identical
  notifications are two events and a change filter would merge them. The runtime logs a
  warning when a point combines the two. For the same reason, a pushed point with
  `every = true` should switch off an inherited heartbeat with `report = { max_interval = "0" }`,
  or each heartbeat read raises an event.
- An event without `every` fires when the value changes, which is exactly what a filtered
  stream carries. A deadband also decides how small a change can raise the event.
- A **parameter** (a writable point) shown in the Cumulocity *Parameters* tab is updated from a
  successful write and then confirmed by the next sample. A device can reject or clamp a
  written value so that it reads back unchanged, which a change filter would withhold. So the
  runtime publishes the next reading of every point of a device after a `write` or
  `write-batch` to it, whatever the policy, and the twin is corrected by the next poll. A
  *pushed* parameter is only corrected once the value changes or its heartbeat reads it, so
  give pushed parameters a `max_interval`.

## Seeing the effective policy

The connector publishes the policies it applies in its retained capability descriptor on
`te/device/main/service/<service>/ot/capabilities`, under `reports`
([contract §7](contract/ot-connector-contract.md#7-capability-model)):

```sh
tedge mqtt sub 'te/device/main/service/tedge-dot-modbus/ot/capabilities'   # retained: shown at once
```

```json
"reports": {
  "default": { "max_interval": "30m" },
  "devices": [ { "device": "plc-1", "report": { "on_change": true } } ],
  "points": [
    { "device": "plc-1", "point": "boiler_temp",
      "report": { "on_change": true, "deadband": 0.5, "min_interval": "10s", "max_interval": "30m" } }
  ]
}
```

`default` is the `[connector]` table and `devices` the tables declared on devices. `points`
lists the **effective**, merged policy of each point whose policy differs from its device's. A
point without an entry uses its device's entry merged over `default`. When a point is quieter
than you expect, this is the place to look.

Each sample's `seq` counts published samples only, so a gap in `seq` still means a sample was
lost on the way, never that the policy held it back.

## Moving from `ot-measurement`'s settings

The `ot-measurement` flow used to be the only place to filter: its `on_change`, `deadband`,
`min_interval` and `debounce` parameters, or the same keys in a point's `meta` table. These
settings are **deprecated**. They still work, but they only reduce the measurements, they cannot
send a heartbeat, and their `min_interval` drops a change that arrives too early instead of
sending it later.

To move, rename the keys from `meta` to `report` and keep the other `meta` keys where they are:

```toml
# before
meta   = { on_change = true, deadband = 0.5, min_interval = "10s", measurement = { group = "Boiler" } }

# after
report = { deadband = 0.5, min_interval = "10s" }
meta   = { measurement = { group = "Boiler" } }
```

A flow-wide `on_change = "true"` in the flow's `params.toml` becomes
`[connector] report = { on_change = true }`, or the same on a device. Do not configure both: the
flow would filter again a stream that the runtime has already filtered.

See also the [migration guide](migration/migration-guide.md#8-upgrading-tedge-dot-report-by-exception-report)
and the [flows README](../flows/README.md).
