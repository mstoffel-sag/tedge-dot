# Design

## Context

- The connector already publishes `unit` in every sample (contract §5), in both the Rust and the C
  build. `ot-measurement` resolves group and series (`resolveNaming`) and builds a bare-number body
  (`shapeBody`). Its comment explains why the unit is left out of the body.
- thin-edge.io 2.x ships a `units` flow (`/etc/tedge/mappers/c8y/flows/units.toml`). It subscribes
  to `te/+/+/+/+/m/+/meta`, keeps the retained metadata in the mapper's context, and the c8y
  measurement conversion adds `unit` to each series it finds there. Verified on 2.0.1: retained
  `{"flow.flow":{"unit":"l/m"}}` on `te/device/Pump01///m/flow/meta` turns the next
  `{"flow":{"flow":7.39}}` into `{"flow":{"flow":{"value":7.39,"unit":"l/m"}}}` in Cumulocity.
- The mapper drops a flow's output to its own input topics, to prevent loops. `ot-alarm` therefore
  reads its retained alarms back through a companion flow (`alarm-state.toml`) that fills the
  mapper-wide store. The same constraint applies here.

## Goals / Non-Goals

**Goals:**
- Units reach Cumulocity with no configuration beyond the point's `unit`.
- No extra message per sample: metadata only when it changes.
- Correct after a mapper restart, including for points that rarely publish (static subscribed
  nodes, long heartbeats).

**Non-Goals:**
- Units for measurements that ot-measurement does not produce (other flows, `ot-alarm`'s
  measurement mode).
- Clearing metadata for points removed from the config: a stale unit for a series that no longer
  appears changes nothing that is shown. This can follow later from the link status, as in
  `ot-alarm`.
- Any change to the connector, the contract, or the measurement body.

## Decisions

### D1: Retained metadata topic, not the unit in the body
Publish `<measurement topic>/meta` retained, and leave series values as bare numbers.
*Alternative:* `{value, unit}` in the body. That is the original reason for the omission: the
mapper of the time dropped such series, which stranded the measurement. The metadata channel is
the one thin-edge.io provides for units. A mapper without it ignores the retained message, so
nothing degrades.

### D2: One message per measurement topic, keyed `"<group>.<series>"`
The metadata topic is per measurement type, and a retained publish replaces the whole message. The
flow therefore keeps, per meta topic, the map `series key → unit` and always publishes the full
map. The key is `<group>.<series>`, the dotted path to the series in the body
(`{group:{series:v}}`). That is the form the units flow resolves, verified above.

### D3: Change detection against the mapper-wide store, seeded from the retained messages
`ot-measurement` compares each mapped sample's unit with the stored map for its meta topic and
outputs a new metadata message only when the map changes. The store lives in `context.mapper`
under `ot-measurement-units:<meta topic>`. A companion flow `ot-measurement/units-state.toml`
subscribes to `te/+/+/+/+/m/+/meta` and records every retained metadata message there. After a
mapper restart the broker replays the retained messages, the store is refilled, and a sample
merges into the units already standing instead of replacing them with only the series seen
since the restart.
*Alternative:* `context.script` only. After a restart the first sample would publish a map with
just its own series and drop the others' units until each of them sends a sample. That can take
up to the heartbeat interval (30 min by default) for a static node.
*Alternative:* read the units flow's context. That is an internal of thin-edge.io's builtin flow,
with no stable key.

### D4: Units are evaluated before the deprecated filters
The unit is checked right after the quality, value-type and opt-out checks, before debounce,
on_change, deadband and min_interval. Those filters suppress values, not units, and a unit edit
must not wait for the value to change. Combine mode works the same way; the meta topic is derived
from the combined target topic.

### D5: Custom `target_topic`
Metadata is published only if `target_topic` matches `te/<s1>/<s2>/<s3>/<s4>/m/<type>`, on
`<target_topic>/meta`. For any other topic there is no metadata channel to use, so none is
published.

## Risks / Trade-offs

- [The first measurement after a unit appears may arrive without it] The c8y mapper learns the
  unit through the broker, so the measurement published in the same step can be converted first.
  → Accepted: every later measurement carries the unit. The metadata is published before the
  measurement in the same output batch, which makes the race rare.
- [The companion flow is a second subscription on a new topic family] It only writes to the
  mapper store and outputs nothing. → Same pattern as `alarm-state`, covered by flow tests.
- [Metadata published by someone else on the same topic is overwritten] For example, by a
  hand-written workaround such as the blueprint's script. → The flow merges into what
  `units-state` read, so a foreign key stays until the flow changes the message. Documented.

## Migration Plan

Flow-only. A package upgrade replaces the flow files and the mapper hot-reloads them. Rolling
back leaves the retained metadata, which is harmless and still correct. Devices that published
units by hand (the OPC-UA solution blueprint) can drop that once they run a release with this
change.
