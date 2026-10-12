# RFC 0004: Point libraries — decoupling data point lists from the connector target

Status: proposed — implemented alongside this RFC (contract §3.4, both SDKs, packaging, e2e)

## Problem

A connector configuration today mixes two things that have entirely different lifetimes:

```toml
[[device]]
name             = "plc-1"
protocol_address = { transport = "tcp", host = "192.168.0.10", port = 502, unit_id = 1 }  # per instance

  [[device.point]]     # per device TYPE — identical on every instance
  id       = "boiler_temp"
  datatype = "float32"
  address  = { table = "holding", address = 7, count = 2 }
  # ...and 200 more
```

The point list belongs to the *device type*: every ACME meter of the same firmware has the
same registers at the same addresses. What differs between instances is the address you reach
it at. Because the two live in one file, a site with twenty identical meters has the same 200
point definitions copy-pasted twenty times. That makes:

* **onboarding manual.** A device found on the network cannot be added without also producing
  its point list, so a discovery mechanism (mDNS, a subnet scan, an asset inventory) has
  nothing useful to publish — even though the operator already knows *what kind of thing* it
  found.
* **point lists unshippable.** A vendor, or this project, cannot package "the point list for
  an ACME meter" as an artefact, because the only place a point list can live is inside a
  specific gateway's configuration.
* **fixes unscalable.** A wrong scaling factor on one register is a twenty-file edit, and
  there is nothing to diff against to know the twenty agree.

## Decision

Introduce a **point library**: the point list of one device type, in its own TOML file, with
no connection information in it. A device instance references one or more and declares only
what is genuinely per-instance.

```toml
# /usr/share/tedge-dot/points.d/modbus/acme-meter-v2.toml
[library]
protocol = "modbus"
type     = "acme-meter-v2"   # added by RFC 0005: the device type these points describe

[[point]]
id       = "boiler_temp"
datatype = "float32"
address  = { table = "holding", address = 7, count = 2 }
```

```toml
[[device]]
name             = "plc-1"
protocol_address = { transport = "tcp", host = "192.168.0.10", port = 502, unit_id = 1 }
points_from      = ["acme-meter-v2"]
```

Naming the device type is [RFC 0005](0005-device-types-and-parameter-sets.md)'s addition to this
design: this RFC established that a library *is* one device type's point list, and that RFC
gives the type a name so the cloud-facing identifiers derived from it stop colliding.

The full rules are normative in [contract §3.4](../contract/ot-connector-contract.md#34-point-libraries)
and machine-readable in [point-library.schema.json](../contract/schemas/point-library.schema.json).
The decisions worth arguing about are below.

### 1. Libraries are protocol-scoped, and a name is a file

A bare name resolves as `<dir>/<protocol>/<name>.toml` along a search path: the site's
`/etc/tedge/plugins/ot/points.d` first, then the packaged `/usr/share/tedge-dot/points.d`.

The protocol subdirectory is not decoration. A point's `address` is one of the three
protocol-specific objects the contract leaves opaque (§3.2), so a point list is only ever
meaningful for one protocol; putting the protocol in the path means the same device type can
have a Modbus library *and* an OPC UA one under the same name, and that a file copied into the
wrong place fails with "is for protocol 'opcua', not 'modbus'" instead of a hundred address
parse errors.

`/etc` before `/usr/share` is the familiar override: dropping a file of the same name into the
site directory shadows a packaged library without touching anything a package upgrade
replaces. A reference containing `/` or ending in `.toml` is a path instead of a name
(relative to the referencing config's directory), which is what makes a self-contained
checkout, a demo, or a one-off list next to its config work.

**Alternative rejected:** a registry file mapping names to paths. It is one more file to keep
in step, and it would have to be writable by whatever creates libraries — which is exactly
the coupling this RFC removes.

### 2. Several libraries per device, applied in order, last definition wins

`points_from` is a list, and the device's own inline points are applied after all of them. A
definition whose `id` already exists **patches** the one collected so far: `meta` and
`transform` merge key by key, every other field replaces.

This is what lets a site extend a list it does not own. A packaged library stays untouched
while the device adds a few points of its own, retitles one parameter, or slows a single
point's `poll_interval` — with no need to restate that point's datatype and address:

```toml
points_from = ["acme-meter-v2", "site-extras"]

  [[device.point]]
  id   = "boiler_temp"      # patch: keeps the library's datatype and address
  unit = "K"
```

`address` replaces rather than merges because a partly-inherited protocol address is not a
meaningful object: inheriting `table` and `count` while overriding `address` would produce a
plausible-looking register that nobody declared. `meta` merges (recursively) because its whole
purpose is a bag of independent per-signal facts, and the common override is adding *one* of
them — a `meta.parameter.title` — to a point whose `meta.alarm` should stay as the library set
it. `transform` merges for the same reason, one factor at a time, and so does the reporting
policy `report` (contract §5.3), one key at a time.

A repeated `id` *within a single library* is an error, not an override: there is no order in
which to apply it, and the second definition would silently win.

**Alternative rejected:** replacing a repeated point wholesale. It forces a one-field tweak to
restate the address, which is the copy-paste this RFC exists to remove — and a restated
address is exactly the thing that silently drifts from the library's.

### 3. Resolution happens at load; the persisted document keeps the reference

The loader expands references into the typed configuration and nothing else. Two consequences,
both load-bearing:

* **Nothing downstream changes.** A protocol module, a flow, the capability descriptor and the
  sample envelopes cannot tell an inherited point from an inline one. This feature added no
  field to the contract's sample, command or status envelopes, and no protocol module was
  touched.
* **The management verbs keep the reference.** The runtime patches and persists the raw
  document (§6.3), which still says `points_from = ["acme-meter-v2"]`. A `set-config` therefore
  never rewrites a user's file with 200 expanded points, and `define-device` can define a
  device from connection information plus a reference alone:

```sh
tedge mqtt pub -r te/device/main/service/tedge-dot-modbus/ot/cmd/define-device/d1 '{
  "status": "init",
  "device": {
    "name": "plc-7",
    "protocol_address": { "transport": "tcp", "host": "192.168.0.17", "port": 502, "unit_id": 1 },
    "points_from": ["acme-meter-v2"]
  }
}'
```

That is the discovery hook, and it is the reason the expansion boundary sits where it does.
Whatever finds instances on the network — mDNS, a scan, an inventory export — publishes one
small command per instance and the device type's point list is already on the gateway. The
discovery mechanism itself is deliberately **out of scope**: it is site-specific, and every
site already has an answer. What was missing was something for it to point at.

### 4. A shared list documents itself

A point carries an optional `name` (short label) and `description` (§3.1). The `id` cannot do
that job: it is a topic segment and a parameter-set key, so it stays a plain identifier — which
is why a list of two hundred points reads like `temp_u16`, `count_u32`, `status_word` and
nobody downstream can tell what they are.

Putting the labels on the point means a library declares them **once** and every instance that
references it inherits them, which is the same argument as the addresses themselves. A site can
still relabel one point without restating anything else, because labels are ordinary scalars
under the patch rule above.

They surface in the two places that are free: a parameter's Cumulocity DTM title and
description (rendered by `describe` from the same TOML, with `meta.parameter.title` /
`.description` still winning so a parameter can read differently from the signal), and the
connector's retained capability descriptor as `point_labels` (§7) — which is also the only
place a *read-only* point's label can appear, since it has no parameter definition.

They are deliberately **not** echoed in the sample envelope. A label is static and a sample is
a time series: echoing a description on every read of every point would put the same bytes on
the wire at the poll interval, forever, for information that changes when someone edits the
config. One retained message per connector says it once.

A protocol's addresses can also depend on the server they were written against. OPC UA node ids
name their namespace by index, and two instances of one device type may number their namespaces
differently. Such a library carries what it was written against, OPC UA's `[library] namespaces`
(RFC 0006). Each point keeps the `[library]` table its address came from, so the setting
applies exactly to the library's addresses.

### 5. Errors are loud, and a command may only name a library

An unresolvable reference fails the load, naming the reference and every path it looked in. A
library for the wrong protocol, a file that is really a connector configuration, a library with
no points, a patch with no base — each is a distinct message. The failure mode to design
against is a device resolving to *zero* points: the connector would come up healthy, the link
would report connected, and nothing would ever be published. So an empty `point` list is
rejected exactly like a missing one, and an explicit `point_library_path` must name at least
one directory rather than quietly reverting to the default.

Two rules exist only because `points_from` is reachable from the broker, not just from a file:

* a reference *introduced* by a management command **must be a name**, refused before the path
  is opened. A config file is written by whoever administers the device; a command is not. A
  path taken from the broker would otherwise let anything able to publish learn whether an
  arbitrary file exists and, through a TOML parse error, see a line of it in the retained
  command result. Names are all discovery needs — the `define-device` example above uses one.
  The check compares the patched document against the one held, not the result as a whole:
  judging the whole candidate made *every* verb — an unrelated `set-config`, even
  `remove-device` — fail on a config that legitimately used the path form.
* the two implementations must agree *exactly* on resolution, including the odd cases. Twice
  they did not. An empty `point_library_path` first made the Rust loader fail and the C loader
  fall back to the default path; fixing that left Rust validating the field eagerly and C only
  when some device referenced a library, so a config with a typo'd path and no `points_from`
  still parted them. Both are now validated at load in both, and the mirrored loader tests
  cover the empty, scalar and non-string forms with and without a reference present.

## What this does not do

Deliberately left for when there is a real need, rather than guessed at now:

* **Point selection.** Referencing a 200-point library when 20 are wanted still means copying
  the file. An `include`/`exclude` glob pair on the device is the obvious answer and would fit
  the resolver as it stands; nothing in this design has to change to add it.
* **Library-level defaults.** A library cannot set a `default_mode` or a poll interval for its
  own points; each point states what it needs, and the device's `default_mode` applies as
  before. Adding one means materialising it per point during expansion, which is small but
  needs the interaction with the device default pinned down first.
* **Nested libraries.** A library cannot reference another. The device-level list covers
  composition, and keeping libraries leaf files means the resolver has no cycles to detect.
* **Generated libraries.** The natural next step is a connector-specific `browse` that produces
  a library from a device that can describe itself (an OPC UA address space, a CANopen object
  dictionary, a DBC file). That is a per-protocol feature; this RFC gives it its output format.

## Implementation

| Where | What |
| --- | --- |
| [contract §3.4](../contract/ot-connector-contract.md#34-point-libraries) | normative rules; §3 skeleton, §3.3 validation |
| [schemas](../contract/schemas/) | `point-library.schema.json`; `config.schema.json` gains `points_from` and `point_library_path`, and moves the point *completeness* rules into `$defs/complete_point` so a patch validates |
| [impl/rust/crates/sdk/src/library.rs](../../impl/rust/crates/sdk/src/library.rs) | the resolver; `load`/`resolve` are now the only way a config is read |
| [impl/c/sdk/src/config.c](../../impl/c/sdk/src/config.c) | the same resolution and merge rules; the parsed library documents are owned by the config, since a point's `address` is borrowed from the document that declared it |
| [connectors/modbus/](../../connectors/modbus/) | the e2e harness gets half its points from a library, so the existing suite also checks that an inherited point behaves identically |
| [demo/](../../demo/) | a library per simulator in `points.d/<protocol>/demo-sim.toml`, which the demo configs in `config/` reference instead of inlining their points — also what the packages ship, so a user can point a device at an address and be reading it |

Both implementations resolve the same references to the same points in the same order; the
mirrored unit tests (`library.rs` and `impl/c/tests/config.c`) exist because a divergence here
would mean the same configuration sampling different signals depending on which package is
installed.

## Breaking changes

None. `points_from` is optional, and a configuration that inlines all of its points is parsed
exactly as before.
