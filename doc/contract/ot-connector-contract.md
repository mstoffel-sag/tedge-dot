# The OT Connector Contract

| Field | Value |
| --- | --- |
| Status | **Normative draft** |
| Version | 0.1.0 |
| Date | 2026-05-30 |
| Schemas | [schemas/](schemas/) · [asyncapi.yaml](asyncapi.yaml) |

This document defines the **OT Connector Contract**: the protocol-neutral interface every
connector exposes to the rest of thin-edge.io. It is the single source of truth that the
[SDK](../sdk/connector-sdk.md), the [connector specs](../connectors/), the
[flows](../flows/), and the [conformance suite](../conformance/conformance-suite.md) all
build on.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are used as in
RFC 2119.

> **This contract is protocol-agnostic.** Nothing in it is specific to Modbus, CAN, BACnet,
> OPC-UA, or any other protocol. Exactly three configuration objects are left opaque for a
> protocol to define — `connection`, `device.protocol_address`, and `point.address` (§3.2) —
> and everything else (topics, sample envelope, datatypes, commands, capabilities, status) is
> identical across protocols. Throughout this document **Modbus is used only as a running
> example** to make the abstract structure concrete; wherever you see Modbus terms such as
> *register*, *coil*, or *unit id*, read them as "this protocol's equivalent."

---

## 1. Concepts and terminology

| Term | Meaning |
| --- | --- |
| **Connector** | A running instance of `tedge-dot` using one protocol module. |
| **Protocol module** | The protocol-specific code (e.g. Modbus) implementing the `Connector` trait. |
| **Device** | A physical/logical field device the connector talks to (a PLC, a meter, an OPC-UA server). Maps to a thin-edge entity. |
| **Point** | A single readable/writable datum on a device (a register, a coil, an OPC-UA node, a CAN signal). |
| **Sample** | The result of reading a point once, published as a *sample envelope*. |
| **Mode** | Per-point output selection: `raw` (bytes) or `typed` (decoded primitive). |
| **Capability** | A declared feature a connector supports (a point kind, a mode, a command verb). |

A connector manages **one or more devices**; each device has **one or more points**. The
connector reads points (by polling and/or subscription), publishes **samples**, accepts
**commands** (e.g. writes), and reports **status**.

## 2. Topic conventions

All topics live under the thin-edge.io entity tree so the local mapper and flows pick them
up naturally. `<device>` is the thin-edge entity id segment for the device
(e.g. `plc-1`); `<protocol>` is the protocol module id (e.g. `modbus`).

| Purpose | Direction | Topic | Retained |
| --- | --- | --- | --- |
| Sample (read result) | connector → broker | `te/device/<device>/ot/<protocol>/sample/<point>` | no |
| Connector status | connector → broker | `te/device/main/service/<service>/status/health` | yes |
| Device link status | connector → broker | `te/device/<device>/ot/<protocol>/status/link` | yes |
| Capability descriptor | connector → broker | `te/device/main/service/<service>/ot/capabilities` | yes |
| Command request | requester → broker | `te/device/<device>/ot/<protocol>/cmd/<verb>/<id>` | yes |
| Command result | connector → broker | `te/device/<device>/ot/<protocol>/cmd/<verb>/<id>` | yes |
| Management command request | requester → broker | `te/device/main/service/<service>/ot/cmd/<verb>/<id>` | yes |
| Management command result | connector → broker | `te/device/main/service/<service>/ot/cmd/<verb>/<id>` | yes |

Notes:

- `<service>` is the connector service name (`[connector] service_name`, default
  `tedge-dot-<protocol>`). It addresses the connector's management commands (§6.3), so it
  must be unique on the broker and cannot be changed by `set-config`.
- Samples MUST NOT be retained; they are time series.
- Status, capability, and command messages MUST be retained so late subscribers and the
  command state machine observe the latest state.
- Command request and result share a topic; the **payload `status` field** carries the
  state-machine transitions defined in §6.

> Flows are responsible for re-publishing samples into the standard
> `te/device/<device>///m|e|a/...` topics. The connector itself MUST NOT publish to the
> `m/`, `e/`, or `a/` channels.

## 3. Configuration model

Configuration is TOML. The top level is split into a **connector** section, a **connection**
section (protocol-specific), and a list of **devices**, each with a list of **points**. The
structure below is **protocol-neutral**: only the three objects marked *protocol-specific*
(`connection`, `device.protocol_address`, `point.address`) change shape from one protocol to
the next (see §3.2). Everything else is identical for Modbus, CAN, BACnet, OPC-UA, and any
future protocol.

```toml
# /etc/tedge/plugins/ot/<protocol>.toml   (protocol-neutral skeleton)

[connector]
protocol      = "<protocol>"    # protocol module id (MUST match a compiled-in module)
service_name  = "tedge-dot-modbus"
poll_interval = "2s"            # default poll interval (duration string); per-point override allowed
log_level     = "info"
operation_timeout = "30s"       # optional: upper bound on one protocol-module call (§8.1)
stall_timeout     = "120s"      # optional: restart the connector if its loop stops moving (§8.1)
# optional: where bare point-library names are looked up (§3.4); shown with its default
point_library_path = ["/etc/tedge/plugins/ot/points.d", "/usr/share/tedge-dot/points.d"]
report        = { max_interval = "30m" }  # optional: default reporting policy for every point (§5.3)

[mqtt]
host = "127.0.0.1"
port = 1883

# Protocol-specific shared connection defaults. Shape defined by each connector spec.
[connection]
# ...

[[device]]
name     = "<device-name>"      # -> te/device/<device-name>
type     = "<device-type>"      # optional; what this device IS (§3.1), else from its library
protocol_address = { } # protocol-specific: how to reach this device. Shape per connector spec.
poll_interval = "2s"            # optional per-device override
default_mode  = "typed"         # optional; default output mode for this device's points
points_from   = []              # optional; point libraries to inherit points from, in order (§3.4)
enabled       = true            # optional; false keeps the definition but leaves the device out (§3.3)
report        = { on_change = true }  # optional: default reporting policy for this device's points (§5.3)

  [[device.point]]
  id       = "<point-id>"       # unique within the device; appears in topics
  mode     = "typed"            # "raw" | "typed" (inherits device.default_mode if omitted)
  datatype = "float32"          # required when mode = "typed"; see §4
  endianness    = "big"         # byte order: "big" | "little" (typed only)
  word_order    = "big"         # multi-word order: "big" | "little" (typed only)
  poll_interval = "1s"          # optional per-point override
  address  = { } # protocol-specific: how to address this point. Shape per connector spec.
  access   = "read"             # "read" | "write" | "read_write" (default "read")
  unit     = "raw"              # optional free-form hint passed through in the sample
  name     = "<short label>"    # optional human-readable label (§3.1); the id stays an identifier
  description = "<what this signal is>"  # optional longer explanation (§3.1)
  transform = { multiplier = 1, divisor = 1, decimal_shift = 0, offset = 0 } # optional linear scale
  # map = { cases = [{ eq = 0, to = "stopped" }, { eq = 1, to = "running" }], default = "unknown" } # optional (§4.3)
  report   = { deadband = 0.5, min_interval = "10s" }  # optional reporting policy (§5.3)
  enabled  = true               # optional; false keeps the definition but leaves the point out (§3.3)
```

> **Example (Modbus).** To make the skeleton concrete, here are the same fields populated for
> a Modbus device. Only the three protocol-specific objects differ from the skeleton; a CAN or
> OPC-UA example would fill in those same three slots differently while keeping every other
> field identical.
>
> ```toml
> [connector]
> protocol      = "modbus"
> poll_interval = "2s"
>
> [connection]
> serial = { baudrate = 9600, parity = "N", stopbits = 2, databits = 8 }  # RTU defaults
>
> [[device]]
> name     = "plc-1"
> protocol_address = { transport = "tcp", host = "192.168.0.10", port = 502, unit_id = 1 }
> default_mode = "typed"
>
>   [[device.point]]
>   id       = "boiler_temp"
>   mode     = "typed"
>   datatype = "float32"
>   address  = { table = "holding", address = 7, count = 2 }
>
>   [[device.point]]
>   id       = "run_command"
>   mode     = "raw"
>   access   = "read_write"
>   address  = { table = "coil", address = 0, count = 1 }
> ```

### 3.1 Common (protocol-neutral) point fields

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `id` | string | yes | Unique within the device; used in `sample/<point>` and `cmd` topics. |
| `mode` | `"raw"` \| `"typed"` | no | Inherits `device.default_mode`, else `"typed"`. |
| `datatype` | string | when `typed` | One of §4's primitive types. |
| `endianness` | `"big"` \| `"little"` | no | Byte order for `typed`; default `"big"`. |
| `word_order` | `"big"` \| `"little"` | no | Word order for multi-word `typed`; default `"big"`. |
| `poll_interval` | duration string | no | Overrides device/connector default. |
| `access` | `"read"` \| `"write"` \| `"read_write"` | no | Default `"read"`. |
| `unit` | string | no | Opaque hint echoed into the sample for flows. |
| `name` | string | no | Short human-readable label, for wherever a name is displayed instead of the `id` — which is a topic segment and a parameter-set key, so it stays a plain identifier. Feeds a parameter's DTM title (§5.2) and the capability descriptor's `point_labels` (§7). |
| `description` | string | no | Longer human-readable explanation of the signal. Feeds a parameter's DTM description and `point_labels` (§7). |
| `transform` | object | no | Per-point linear scale `(value*multiplier*10^decimal_shift/divisor)+offset`; see §4.2. |
| `map` | object | no | The point's **value map**: state codes to labels, numeric ranges to bands with a catch-all, numeric text to numbers. Applied by the SDK runtime after `transform` on reads and reversed on writes; a later definition replaces it whole. See §4.3. |
| `report` | object | no | The point's **reporting policy** (report by exception): publish only on change or beyond a deadband, rate-limit, debounce, and a heartbeat. Applied by the SDK runtime, not the module; merged key by key over the device's and `[connector]`'s `report`. See §5.3. |
| `meta` | object | no | Free-form signal metadata echoed verbatim as `meta` in every sample envelope. Never interpreted by the connector; flows and tooling read it for per-signal behaviour, for naming the point's measurement or, with `meta.measurement = false`, keeping it out of the measurements, for declaring the signal's alarms and events (`meta.alarm`, `meta.event`, read by the `ot-alarm` / `ot-event` flows), and for exposing the point as an operator-editable *parameter* (`meta.parameter`, see §5.2). |
| `subscribe` | boolean | no | Default `true`. `false` keeps the point on the polling schedule even when the connector supports push delivery. |
| `enabled` | boolean | no | Default `true`. `false` keeps the definition but leaves the point out of the device (§3.3) — how a site switches off one point of a library it does not own (§3.4). |
| `address` | object | yes | **Protocol-specific**; shape defined by the connector spec. |

`name` and `description` are **not** echoed in the sample envelope: they are static per point,
so the connector publishes them once in its retained capability descriptor (§7) instead of on
every read. `meta.parameter.title` / `meta.parameter.description` override them for a
parameter's cloud-facing labels, so a point can carry a general-purpose label and still say
something different in the parameter UI.

A device MAY inherit these same point fields from a **point library** instead of declaring
them inline; see §3.4.

#### The device `type`

`[[device]] type` names the **device type** the instance is one of: what its point list
describes, as opposed to `protocol_address`, which says where to reach it. It is optional, and
a device that does not declare one inherits the `type` of the first point library it references
(§3.4) — a library *is* the point list of one device type, so that is where it is usually
declared, once, for every instance.

It matters because it names things that outlive the device: the device's **parameter sets**
(§5.2), whose names are tenant-wide identifiers in the cloud, and the thin-edge entity type a
registration flow assigns. A connector MUST therefore echo it where a consumer needs it without
the configuration file: in every sample (§5) and on the link status (§8). Nothing else in the
runtime interprets it.

### 3.2 Protocol-specific fields

`device.protocol_address`, `connection`, and `point.address` are **opaque to the contract**:
their shape is defined by each [connector spec](../connectors/). The contract only requires
that they are objects and that each connector documents and schema-validates them.

### 3.3 Validation rules

- A point with `mode = "typed"` MUST declare a `datatype`.
- A point with `mode = "raw"` MUST NOT be rejected for missing `datatype`; decoding fields
  are ignored.
- `id` MUST be unique within a device; `name` MUST be unique within a connector, and a
  connector MUST reject a repeated device name rather than let two definitions publish over
  each other on one entity's topics. Across the sources a device collects its points from
  (§3.4) a repeated `id` is an override, not a duplicate; within one of those sources it is an
  error.
- The rules above apply to the **resolved** point, after every `points_from` reference has
  been merged (§3.4).
- A device `type`, where present, MUST be a non-empty string; a connector MUST reject a blank
  one rather than treat it as absent, so the same configuration is accepted by every
  implementation.
- A device with `enabled = false` is **not loaded**: the connector MUST NOT connect to, poll,
  describe or answer commands for it, and MUST NOT read anything else about it — its type, its
  address or its point libraries; only the names of its keys are checked, as for every device
  (below) — so a configuration can carry a ready-made device switched
  off, even one whose library is not installed. `enabled` MUST be a boolean when present
  (default `true`), and a disabled device's `name` still counts towards uniqueness. The
  definition stays in the file, so `set-config` (§6.3, target `device:<name>`) can switch it
  on again. A device switched off while the connector runs — by a reload or a restart — is left
  like a removed one (`remove-device`): it is no longer polled or answered for, but its retained
  link status (§8) is not cleared.
- A point with `enabled = false` is **left out of its device**, exactly as if it were not
  configured: the connector MUST NOT read, subscribe to, write or describe it, and it appears in
  nothing the connector publishes — samples, the capability descriptor (§7), the link status
  (§8). The rule applies to the **resolved** point (§3.4): `enabled` is replaced like any other
  field, so a later definition can switch a point back on, and a bare
  `{ id = "<point-id>", enabled = false }` switches off a point a library supplies. A disabled
  point is exempt from the completeness rules (an `address`, a `datatype` when typed), but what
  it does declare MUST still be valid, and `enabled` MUST be a boolean in every definition that
  carries it (default `true`).
- Duration strings follow the thin-edge convention (`"500ms"`, `"2s"`, `"5m"`): a decimal
  number and an optional unit — `ms` (whole milliseconds), `s`, `m` or `h`; no unit means
  seconds — optionally surrounded by whitespace. Signs, exponents and `inf`/`nan` are not
  durations.
- A field's value MUST be valid wherever the definition is written — inline, as a patch of a
  library point, in a point library, on a disabled point — and an invalid one MUST be rejected
  at load, naming the point (or device, or `[connector]`) and the field: an enumerated field
  (`mode`, `datatype`, `endianness`, `word_order`, `access`, `default_mode`) spelt exactly as
  listed, a `poll_interval` that is a duration, `unit`/`name`/`description` strings, `address`,
  `transform` and `meta` tables, `transform` numbers (`decimal_shift` a 32-bit integer),
  `subscribe`/`enabled` booleans, and a `map` table whose shape and output types are valid
  (§4.3). The checks of a `map` against the point's `mode` and `datatype` apply to the
  resolved point, since a library may declare the map and the site the datatype. A value read leniently instead — an unknown `access` as
  `"read"`, an unparseable `poll_interval` as the device's — would load and quietly do the
  wrong thing.
- A key the contract does not define MUST be rejected — at the top level, in `[connector]` and
  `[mqtt]`, in a `[[device]]` (a disabled one included), in a point, inline or in a point
  library (§3.4), in its `transform`, its `map` and each of the map's `cases`, and in a
  library's `[library]` — naming the key and the
  table it is in, and the known key it most resembles when one is close. A misspelt setting
  (`polling_interval` for `poll_interval`) would otherwise be accepted and do nothing. The
  protocol-specific objects (`connection`, `protocol_address`, `address`) are delegated to the
  connector's own schema, and `meta` is free-form.

### 3.4 Point libraries

A device type has the same data points on every instance; what differs from one instance to
the next is the connection information. A **point library** is that point list in its own
file, carrying no connection information at all:

```toml
# /usr/share/tedge-dot/points.d/modbus/acme-meter-v2.toml
[library]
protocol    = "modbus"          # MUST match the referencing connector's protocol
type        = "acme-meter-v2"   # optional; the device type these points describe (§3.1)
description = "ACME meter, firmware 2.x"   # optional, informational
version     = "2.1"                        # optional, informational

[[point]]                       # exactly the point fields of §3.1/§3.2
id       = "boiler_temp"
datatype = "float32"
address  = { table = "holding", address = 7, count = 2 }
```

A device references one or more, in order, and declares only what is per-instance:

```toml
[[device]]
name             = "plc-1"
protocol_address = { transport = "tcp", host = "192.168.0.10", port = 502, unit_id = 1 }
points_from      = ["acme-meter-v2", "site-extras"]
```

A library holds **only** `[library]` and `[[point]]`. A file that also carries `connector`,
`mqtt`, `connection` or `device` is a connector configuration and MUST be rejected as such.

#### Resolution

A `points_from` entry is either a **name** or a **path**; an entry containing `/`, or ending
in `.toml`, is a path, and a relative path resolves against the referencing configuration
file's own directory. A name is resolved as `<dir>/<protocol>/<name>.toml` in each directory
of the **library search path**, first match winning:

| Precedence | Directory | For |
| --- | --- | --- |
| 1 | `/etc/tedge/plugins/ot/points.d` | a site's own libraries; shadow packaged ones of the same name |
| 2 | `/usr/share/tedge-dot/points.d` | libraries shipped by a package (a connector's, or a vendor's device pack) |

`[connector] point_library_path` (an array of directories, relative ones resolved against the
configuration file's directory) **replaces** that default path, and MUST therefore name at
least one directory — an empty or unusable list is a configuration error, not a way to say
"no libraries", and MUST NOT be read as if the key were absent. Where the key *is* absent, the
`TEDGE_DOT_POINT_LIBRARY_PATH` environment variable (colon-separated) replaces the default
path instead, which is how a source checkout points at its own libraries. Names are protocol-scoped because a point's
`address` is protocol-specific (§3.2): the same device type can have a library per protocol,
and only the connector's own is ever loaded.

#### Ordering and overrides

Points are collected in order — each library in the order listed, then the device's own
inline `[[device.point]]` entries — and a definition whose `id` already exists **patches** the
one collected so far rather than adding a second point:

- `meta` and `transform` are merged key by key (recursively for `meta`), so one field can be
  adjusted without restating the rest;
- `map` (§4.3) is **replaced** whole: its cases are an ordered list, so merging two would mean
  nothing. `map = {}` removes an inherited map;
- every other field, `address` included, is **replaced** when the overriding definition
  declares it (a partly-inherited protocol address is not a meaningful thing). `name` and
  `description` (§3.1) are ordinary scalars under this rule, which is what lets a site relabel
  an inherited point — one label at a time — without restating its address. `enabled` is one
  too, so `{ id = "<point-id>", enabled = false }` switches off an inherited point (§3.3);
- a field the override does not mention keeps its inherited value.

Inline points are applied last, so a device always wins over the libraries it references.
That ordering is what lets a site extend or adjust a packaged list — add a few points, retitle
a parameter, slow one point's `poll_interval` — without editing the file the package owns and
an upgrade replaces.

The validation rules of §3.3 apply to the **resolved** point: a definition that only ever
appears as a patch, with no library supplying the rest, is rejected for the fields it is
missing. Within a single library a repeated `id` is an error, not an override: there is no
order in which to apply it.

#### The device type a library declares

`[library] type` names the device type the points describe (§3.1). A device that does not
declare its own `type` inherits it from the **first** library in `points_from` that declares
one; later references extend that type rather than redefine it (`["acme-meter-v2",
"site-extras"]`), and a `type` on the device itself wins over every library.

A library that declares no `type` leaves the device without one: the file name is deliberately
not used instead, because the type ends up as a tenant-wide identifier in the cloud (§5.2) and
is worth declaring on purpose.

#### Relationship to the rest of the contract

Resolution happens when the configuration is **loaded**, so a protocol module, a flow, the
capability descriptor and the `sample` envelopes see no difference between a point that was
inherited and one that was written inline. What is *not* expanded is the configuration
document the runtime keeps for the management verbs (§6.3): it retains the `points_from`
reference, so persisting a patched configuration never bakes a library's points into the
operator's file, and `define-device` MAY define a device from connection information plus a
reference alone. That is the hook for discovery: a mechanism that finds instances on the
network (mDNS, a scan, an asset inventory) publishes one small `define-device` per instance
naming a device type it already knows.

A connector MUST report an unresolvable reference as a configuration error naming what it
looked for; it MUST NOT silently resolve a device to zero points. A library that declares an
empty `point` list is therefore rejected exactly like one that declares none at all: left to
resolve, the device would come up healthy and publish nothing, which is also what a generated
library that found nothing would produce.

A reference *introduced* by a **management command** (§6.3) MUST be a name, never a path, and
MUST be refused before the path is opened. A configuration file is written by whoever
administers the device, but a command is a different trust boundary: a reference taken from the
broker could otherwise name an arbitrary path and have the connector report what it found
there — whether the file exists, and, through a parse error, part of its contents — in the
retained command result. Names are all a discovery mechanism needs.

Only what the command changes is judged: the path references a device already had remain
valid, so a configuration that legitimately uses the path form still accepts every management
verb. A connector MUST therefore compare the patched document against the one it held, not
scan the result as a whole.

The same boundary applies to **local-only settings**: protocol settings that name files on the
gateway (a password file, a PKI directory, a client certificate) or relax security (accepting
any server certificate, allowing plaintext passwords). A module declares them by key; a
management command that *adds or changes* one, anywhere in `[connection]` or in a device's
`protocol_address`, MUST be refused before anything is written, with a reason naming the key but
not its value. As above, values the configuration already has stay valid. Removing one is a
change too (a device-level `false` may be what overrides a `[connection]` opt-in); removing a
whole device is not. The OPC UA and SNMP connectors declare theirs in their specifications.

## 4. Datatypes (typed mode)

In `typed` mode the driver applies **only** primitive decoding. The contract defines this
closed set of primitive datatypes:

| `datatype` | Meaning | JSON `value` type |
| --- | --- | --- |
| `bool` | single boolean (e.g. a Modbus coil/discrete input, a digital signal) | boolean |
| `int8` / `uint8` | 8-bit integer | number |
| `int16` / `uint16` | 16-bit integer | number |
| `int32` / `uint32` | 32-bit integer | number |
| `int64` / `uint64` | 64-bit integer | number (see §4.1) |
| `float32` | IEEE-754 single | number |
| `float64` | IEEE-754 double | number |
| `string` | fixed-length text (encoding declared per connector) | string |
| `bytes` | opaque byte run (always emitted as hex) | string (hex) |

Decoding semantics:

- **Endianness** (`endianness`) selects byte order within the smallest addressable unit.
- **Word order** (`word_order`) selects the order of multi-word reads when a value spans more
  than one of the protocol's native words (for example, two Modbus 16-bit registers forming a
  32-bit value). Protocols whose values are not word-addressed simply ignore this field.
- The driver MUST NOT apply renaming, unit conversion, thresholding, or thin-edge JSON
  shaping. Those are flow responsibilities. (The declared reporting policy, §5.3, is not
  driver thresholding: like `transform`, it is a contract-level point field the **SDK
  runtime** applies to every connector alike. A module MUST NOT filter samples itself.) The one numeric transform the driver MAY apply is
  the **declared per-point linear transform** (`point.transform`, §4.2): because scaling is an
  intrinsic property of a signal rather than flow logic, it is a contract-level point field whose
  math is owned by the SDK. The driver only invokes the SDK helper; it MUST NOT invent any other
  scaling, offset, or rounding. The declared value map (`point.map`, §4.3) is the non-linear
  counterpart; the **SDK runtime** applies it, never the driver.
- Bit-field extraction (start bit / bit count within a word) MAY be supported by a connector
  as a `typed` refinement and, if so, MUST be declared in that connector's spec. It is the
  one decoding refinement allowed beyond whole-primitive decode, because doing it in JS is
  error-prone.

### 4.1 64-bit integers

`int64`/`uint64` values that exceed JavaScript's safe integer range (`2^53 - 1`) MUST be
emitted as a JSON **string** in `value`, and the connector MUST set `value_repr: "string"`
in the sample (see §5). Flows can then parse with `BigInt`. Values within the safe range
MAY be emitted as numbers with `value_repr: "number"`.

### 4.2 Per-point linear transform

A point MAY declare a `transform` object. The SDK applies it to the decoded **numeric** value
immediately after primitive decode (and after bit-field extraction):

```
out = (value * multiplier * 10^decimal_shift / divisor) + offset
```

| Field | Default | Notes |
| --- | --- | --- |
| `multiplier` | `1` | |
| `divisor` | `1` | A `0` divisor is treated as `1`. |
| `decimal_shift` | `0` | Power-of-ten shift (e.g. `-3` divides by 1000); mirrors the legacy `decimalshiftright`. |
| `offset` | `0` | Added last. |

Rules:

- The transform applies **only** to `number` values. `bool`, `string`, and `bytes` values pass
  through unchanged, and it is a no-op in `raw` mode.
- The scaled value is what the sample's `value`/`value_repr` carry; `raw` always remains the
  unmodified wire bytes.
- The math is owned by the SDK so every connector scales identically. Connectors invoke the SDK
  helper rather than re-implementing it.

**Writes** run the transform in reverse. A write request carries the value in the same
engineering units as the sample's `value` (§6.2), so before the connector encodes it the SDK maps
it back to the raw value:

```
raw = (value - offset) * divisor / (multiplier * 10^decimal_shift)
```

- The same `0` divisor rule applies. Only `number` values of `typed` points are inverted; `bool`
  and `string` values, and `raw` writes, are passed to the connector unchanged.
- For an integer `datatype` (`int8` … `uint64`, including bit-field points) the result is rounded
  to the nearest integer, ties away from zero, so `21.5` with `multiplier = 0.1` writes `215`
  rather than a truncated `214`. Float datatypes keep the unrounded value, so a writable point
  with a transform whose device value is an integer must declare that integer `datatype` (the
  raw type), not a float — e.g. an SNMP `integer`/`timeticks` object scaled by `divisor = 100`
  is `datatype = "int32"`/`"uint32"`, otherwise `2.3` inverts to `229.99999999999997` and the
  connector rejects the fractional value.
- The raw value must fit the `datatype`: one outside its range (e.g. `-45` with `offset = -40`
  on a `uint16`, which inverts to `-5`) fails the write rather than wrapping.
- A transform whose scale `multiplier * 10^decimal_shift` is `0` has no inverse, and a value
  whose raw result is not finite cannot be written: the write is `failed` with a reason and
  nothing is sent to the device.
- The SDK applies the inverse once, on every write path (`write`, each entry of `write-batch`,
  and the CLI `write`); connectors never apply the transform on write. A point with a value
  map (§4.3) has the written value mapped back to the device value first.

### 4.3 Value mapping

A point MAY declare a `map` that converts its value between the device's representation and
the one its samples carry: an operating state code to a label, ranges of codes to a band with
a catch-all, a number stored as text to a number (so it can become a measurement). It is the
non-linear, type-changing counterpart of `transform` and, like it, a contract-level point field
whose rules the SDK owns: the **SDK runtime** applies it to every connector alike, and a
connector never sees it.

```toml
[[device.point]]
id       = "op_state"
datatype = "uint16"
access   = "read_write"
map.cases = [
  { eq = 0,             to = "stopped" },
  { eq = [1, 5],        to = "running" },              # several codes, one label
  { min = 10, max = 19, to = "warning" },              # inclusive range: read-only
  { min = 20,           to = "fault", write = 20 },    # open range with a write value
]
map.default = "unknown"                                 # catch-all

[[device.point]]
id       = "level_text"
datatype = "string"
map      = { as = "number" }                            # "21.5" <-> 21.5
```

| Key | Type | Meaning |
| --- | --- | --- |
| `cases` | array of tables | Evaluated in order; the **first** matching case gives the output. |
| `cases[].eq` | number, string, bool, or a non-empty list of them | Match a value equal to this one (or to any entry of the list). |
| `cases[].min` / `cases[].max` | number | A numeric range, both bounds **inclusive**, either one omitted for an open range. A case has `eq` or a range, not both. |
| `cases[].to` | number, string or bool | The output. |
| `cases[].write` | number | Range cases only: the device value a write of `to` sends. It MUST lie within the range. |
| `default` | number, string or bool | The output for a value no case matches. |
| `as` | `"number"`, `"string"` or `"bool"` | Convert a value no case matches to this type. Cannot be combined with `default`. |

**Reads.** The map applies after primitive decode and `transform`, so range bounds are in the
point's engineering units, and before the reporting policy (§5.3), which therefore compares the
mapped value. Matching:

- Numbers compare numerically; a 64-bit integer carried as a string (§4.1) compares as that
  integer, exactly. Strings compare exactly and case-sensitively; a bool matches only a bool.
  There is no coercion: a numeric `eq` never matches the text `"1"` (use `as`). `NaN` matches
  no case.
- A value no case matches takes `default`, else the `as` conversion, else the sample is
  published `quality = "bad"` with no `value` and
  `error = "point <id>: no mapping for value <v>"`. That is a mapping failure, not a failed
  read: it does not affect the link status (§8), reconnects or liveness.
- `as` conversions: **number** — a string is parsed as a decimal
  (`[+-]?(digits[.digits?] | .digits)([eE][+-]?digits)?`, surrounding whitespace ignored, finite;
  no hexadecimal, `inf` or `nan`), a bool is `1`/`0`; **string** — a number becomes the shortest
  decimal digits that read back as the same number, in positional notation (`0.1`, `12`,
  `1000000000000000000000`, never an exponent; `-0` is `0`), a bool `"true"`/`"false"`;
  **bool** — a number is `true` unless it is `0`, a string is `true` for `true`/`1`/`on`/`yes`
  and `false` for `false`/`0`/`off`/`no` (any case, whitespace trimmed). A value already of the
  type is unchanged; one that cannot be converted makes the sample bad, as above.
- A mapped sample carries `source_value` (and `source_value_repr`): the value before the map,
  after `transform` — the code behind the label (§5). `bad` samples of failed reads carry no
  value and are untouched.

**Writes.** A write carries the **mapped** value (§6.2), so the SDK maps it back to the device
value before the inverse transform (§4.2) and the encoding: it takes the first case whose `to`
equals the written value (compared as above) and writes that case's `eq` (the first entry of a
list) or its range's `write`. A range without `write` is read-only, so a later case with the
same `to` may still supply the write. With no such case and an `as`, it writes the reverse
conversion into the point's own type (`21.5` → `"21.5"` for a `string` point). The write is
`failed`, and nothing is sent to the device, when the written value's type is not the map's
output type, when it matches only `default` (never writable) or nothing, or when the reverse
conversion fails; the reason lists the accepted values. A `raw` write (§6.2) bypasses the map.

**Validation** (at load and on reload, naming the device, the point and the key or case):
`default` and `as` together; outputs (`to`, `default`, the `as` type) of more than one type —
every map publishes one `value_repr`; a case without `to`, with both `eq` and a range, with
neither, with an empty `eq` list or with `min > max`; `write` outside its range or on an `eq`
case; a range on a `string` or `bool` point; an `eq` of a type the point's value can never be;
a map on a `raw`-mode or `bytes` point. A map with none of `cases`, `default` and `as` is
refused, except the empty table, which means no map.

Test vectors both SDKs run are in [test-vectors/map/](test-vectors/map/).

## 5. The sample envelope

Every successful or failed read produces exactly one **sample** message on
`te/device/<device>/ot/<protocol>/sample/<point>`. JSON Schema:
[schemas/sample.schema.json](schemas/sample.schema.json).

The envelope is protocol-neutral; only the `addr` object is protocol-specific (it echoes the
native address so flows can route or debug). The example below uses Modbus to make it concrete:

```json
{
  "ts": "2026-05-30T10:00:00.000Z",
  "ts_ms": 1780221600000.0,
  "device": "plc-1",
  "type": "acme-meter-v2",
  "protocol": "modbus",
  "point": "boiler_temp",
  "mode": "typed",
  "datatype": "float32",
  "value": 42.5,
  "value_repr": "number",
  "raw": "422a 0000",
  "quality": "good",
  "unit": "raw",
  "addr": { "table": "holding", "address": 7, "unit_id": 1 },
  "seq": 12407
}
```

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `ts` | string (RFC 3339, ms, UTC `Z`) | yes | Read completion time. |
| `ts_ms` | number | no | The same instant as Unix epoch milliseconds (float); the numeric companion to `ts` for consumers doing time arithmetic. |
| `device` | string | yes | thin-edge device entity id segment. |
| `type` | string | no | Echo of the device's declared `type` (§3.1), when it has one. Lets consumers name the point's parameter set (§5.2) without the configuration file. |
| `protocol` | string | yes | Protocol module id. |
| `point` | string | yes | Point `id`. |
| `mode` | `"raw"` \| `"typed"` | yes | Echoes the point mode. |
| `datatype` | string | when `typed` | The primitive type decoded. |
| `value` | number \| boolean \| string | when `quality = good` | Decoded value (`typed`) — absent for `raw`. |
| `value_repr` | `"number"` \| `"boolean"` \| `"string"` | when `value` present | Tells flows how to interpret `value`. For a mapped point (§4.3) it is the map's output type, while `datatype` still names the device primitive. |
| `raw` | string (hex, space-grouped per word) | yes | The bytes read; always present in both modes. |
| `quality` | `"good"` \| `"bad"` \| `"stale"` | yes | See §5.1. |
| `unit` | string | no | Echo of the point's `unit` hint. |
| `access` | `"read"` \| `"write"` \| `"read_write"` | no | Echo of the point's declared `access` (SDK runtimes always set it). Lets consumers tell writable points apart without the configuration file (§5.2). |
| `addr` | object | yes | Protocol-specific address echo (for flow routing/debug). |
| `seq` | integer | no | Monotonic per-point counter of **published** samples; helps detect drops. Readings withheld by the reporting policy (§5.3) do not consume a number, so a gap always means a published sample was lost. |
| `error` | string | when `quality = bad` | Human-readable failure reason. |
| `source_value` | number \| boolean \| string | no | Mapped points only (§4.3): the value before the point's `map`, after `transform` — the code behind a label. Present whenever the read produced a value, including when the map has no output for it. |
| `source_value_repr` | `"number"` \| `"boolean"` \| `"string"` | with `source_value` | Tells flows how to interpret `source_value`. |
| `meta` | object | no | The point's `meta` table echoed verbatim by the runtime (§3.1); carries per-signal hints for flows. |

### 5.1 Quality semantics

| `quality` | Meaning | `value` present? |
| --- | --- | --- |
| `good` | Read succeeded; value is current. | yes (typed) / `raw` only (raw mode) |
| `bad` | Read failed (timeout, exception, CRC). `error` set. | no |
| `stale` | Last good value re-emitted because a refresh failed but cached data exists. | yes |

- In `raw` mode there is no `value`; the payload is the `raw` hex. `quality` still applies
  (a failed raw read is `bad` with no `raw`, or `raw` omitted).
- A connector MUST publish `bad` samples for failed reads rather than silently dropping
  them, so flows and operators can react. A connector MAY rate-limit repeated `bad` samples.

### 5.2 Parameters (writable points as device state)

A point whose `access` permits writes is, to an operator, a *parameter*: a setting with a
current value and a control to change it. The contract deliberately adds no mechanism for
this beyond echoing `access`, `meta` and the device `type` in samples: a flow
(`ot-parameter-state`) derives one
retained twin fragment per *parameter set* from the samples and acknowledged writes, and
cloud-specific tooling (`tedge-dot describe`) renders the same sets as cloud-side definitions.
A read-only point can opt in with `meta.parameter = true`, a writable point can opt out with
`meta.parameter = false`. A parameter's key inside its fragment is its point id, or
`meta.parameter.key` when the point names one — so a point can keep an id that is unique on the
device (`firmwareVersion`) and still be `version` in its `firmware` set with
`meta.parameter = { key = "firmware.version" }`: a key `<set>.<key>` names its set too (absolutely,
so it cannot be combined with `set` or `group`), while a key without a dot stays in the point's usual
set (and cannot be combined with `set`). Keys SHOULD be plain
identifiers (`[A-Za-z0-9_]`) and unique per set on a device. The capability descriptor lists
every point that names a key (`parameter_keys`, §7), so consumers know the keys before any sample:
after a restart, since samples are not retained, and for a write-only point, which never samples.

**Naming a set.** A set name is a tenant-wide identifier in the cloud, so it is qualified by the
*device type* (§3.1) — what decides which points exist — and never by the protocol alone, which
says nothing about them:

```text
<device type, else the protocol>_<group, default "control">_parameters
```

Every *run* of characters outside `[A-Za-z0-9]` folds to a single `_`, so `acme-meter-v2` with
the default group gives `acme_meter_v2_control_parameters`. Two knobs refine it, both under `meta.parameter`:

| Key | Meaning |
| --- | --- |
| `group` | A second set *of the same device type* (`commissioning` → `acme_meter_v2_commissioning_parameters`). |
| `set` | An absolute name, used verbatim — the escape hatch for an identifier that predates this rule, or for a set deliberately shared by several device types. A bare string (`meta.parameter = "pump"`) is this form. |

Either key MAY be a **list**, and the point then belongs to every set it names — operators group
signals by what they are *for*, and one setpoint can belong on the commissioning screen and the
daily-operation one:

```toml
meta.parameter = { group = ["control", "commissioning"] }
```

Such a point is a property of *each* of those definitions, and its value is published to each of
their fragments, so the groups never disagree about it. Empty lists and non-string entries are
ignored (a list that names nothing usable behaves like no list at all), names that fold to the
same set are not repeated, and an absolute `set` still wins over `group`.

A device with no declared type falls back to `<protocol>_control_parameters`, which every other
device type on that protocol also falls back to: fine for a fleet of one type, a collision for a
fleet of several, and the reason `tedge-dot describe` warns about it. Consumers derive the same
name from the device `type` echoed in samples and on the link status, so the connector, the
flows and the cloud-side definitions agree without sharing a configuration file.

**Keeping a set current.** A fragment must hold exactly the parameters the device has now:
Cumulocity sends the whole fragment back with an operator's edit, so a key for a point that is
gone fails every update of that set. The flow therefore drops a point from every set when the
link status (§8) no longer lists it, and from a set its latest sample no longer names (its
group, its type or its access changed); a set left with no points is cleared with an empty
retained message rather than published as `{}`. A fragment the flow no longer holds in memory —
published before the mapper restarted, for a set with no configured point left — is the one
case it cannot see, and is cleared by hand (see RFC 0005).

A mapped point (§4.3) is a parameter like any other: its fragment holds the mapped value (the
label), an operator's edit is written as that label and the SDK maps it back, and
`tedge-dot describe` renders the property with the map's output type and, when its writable
values form a closed set (no `as`), lists them as the property's `enum`, so the cloud offers a
choice. The datatype's range does not apply to a mapped property; `meta.parameter.enum`,
`min` and `max` still win.

See [RFC 0003](../rfc/0003-parameter-writes.md) and
[RFC 0005](../rfc/0005-device-types-and-parameter-sets.md).

### 5.3 Reporting policy (report by exception)

By default every reading is published. A point MAY declare a **reporting policy** that
publishes it only when it matters — on a change, on a change beyond a deadband, at most so
often, once a new value has settled — and a **heartbeat** that still publishes it now and then
when nothing changes. The policy is applied by the **SDK runtime** in front of the sample topic,
so every consumer (flows, alarms, the parameter twin, third-party tools) and the broker itself
see the reduced stream, and every connector behaves identically. It is configured, not coded:

```toml
[connector]
report = { max_interval = "30m" }                  # heartbeat for every point

[[device]]
name   = "plc-1"
report = { on_change = true }                      # this device's points: only on change

  [[device.point]]
  id     = "boiler_temp"
  report = { deadband = 0.5, min_interval = "10s" } # ...and this one beyond 0.5, at most every 10 s
```

| Key | Type | Meaning |
| --- | --- | --- |
| `on_change` | boolean | Publish only when the value differs from the **last published** one. |
| `deadband` | number, or string `"<p>%"` | Publish only when the value moved at least this much from the last published one: an absolute amount in the point's (transformed) units, or `p` percent of the last published value's magnitude. Implies `on_change`. Numeric values only. |
| `min_interval` | duration | Publish at most once per interval. A change arriving sooner is **held and published when the interval ends** (with its own `ts`), never dropped. |
| `max_interval` | duration | Heartbeat: publish the point's next **fresh reading** once this long has passed since its last publish, even if unchanged. |
| `debounce` | duration | Publish a changed value only once it has stayed the same (within the deadband, if one is set) for this long. Implies `on_change`. |

A duration of `"0"`, `on_change = false` and `deadband = 0` switch a setting off, which is how a
more specific level turns off one it inherits.

**Inheritance.** The effective policy of a point merges, key by key and each winning over the
ones before it: `[connector] report`, the device's `report`, the point's `report` in a point
library (§3.4), and the point's `report` in the configuration. A single `report` table that
sets `max_interval` not greater than `min_interval` is refused; when that only arises through
inheritance, the effective heartbeat is raised to twice `min_interval` and a warning names the
point.

**Rules** (per device and point):

1. **Always published**, whatever the policy: the first reading after the connector starts, after
   an applied reload or management command (§6.3), after the device reconnects, after a
   `write`, `write-batch` (or other write) command to the device (a rejected or clamped write reads back
   unchanged, while the parameter twin already shows the written value), and after the
   connection to the broker is restored; and any reading whose `quality` differs from the last
   published one. Publishing a quality change discards a held reading, so an older good value
   never follows a newer bad one.
2. **Change.** Numbers (`value_repr = "number"`) compare numerically after `transform`: any
   difference above 1e-9 with `on_change` alone, at least the deadband when one is set (with a
   percent deadband and a last value of 0, any change). `NaN` equals only `NaN`. Everything
   else — booleans, strings, 64-bit integers carried as strings (§4.1), raw points — compares by
   exact equality of `value_repr` and `value` (or `raw` when there is no value); the deadband
   does not apply to them. A mapped point (§4.3) is compared by its **mapped** value, so two
   codes with one label are no change, and a deadband applies only when the map's output is a
   number.
3. **Deadband drift.** Differences are measured from the last *published* value, so a slow
   drift is reported once it adds up to the deadband.
4. **Heartbeat.** A heartbeat never re-publishes an old reading. A polled point's next poll is
   published; a pushed (subscribed) point is **read on demand**, at most once per
   `max_interval`. A failed or timed-out read publishes a `bad` sample (and affects the link
   status like a failed poll), so a dead source is reported rather than hidden. A point the
   module cannot read on demand — an SNMP trap, a CAN frame — gets no heartbeat.
5. **Timing.** Held and debounced readings are published on the first pass of the runtime's
   loop after their interval ends, keeping the `ts` of the reading.
6. **`seq`** counts published samples only (§5).

The one-shot CLI `read` is not subject to the policy. The runtime publishes the effective
policies in the capability descriptor (`reports`, §7), so a consumer can tell why a point is
quiet. Because every consumer sees the reduced stream, keep a deadband smaller than the
hysteresis of any alarm threshold on the same signal, and do not give a change filter to a point
whose every reading is an occurrence (`meta.event.every`); the runtime logs a warning for the
latter.

Test vectors both SDKs run are in [test-vectors/report/](test-vectors/report/).

## 6. Command protocol

Commands let external actors (cloud operations, other flows, operators) act on a connector —
primarily **writing** points. Commands use a request/result state machine on
`te/device/<device>/ot/<protocol>/cmd/<verb>/<id>` (retained). JSON Schema:
[schemas/command.schema.json](schemas/command.schema.json).

### 6.1 State machine

```text
init ──▶ executing ──▶ successful
                   └──▶ failed
```

| `status` | Set by | Meaning |
| --- | --- | --- |
| `init` | requester | New command request, payload includes inputs. |
| `executing` | connector | Connector accepted it and is acting. |
| `successful` | connector | Completed; `result` may carry output. |
| `failed` | connector | Failed; `reason` set. |

The requester publishes the `init` message; the connector transitions it through the
remaining states on the **same topic** (retained). A clearing (empty retained) message ends
the command lifecycle.

### 6.2 The `write` verb (standard)

Request (`status: "init"`):

```json
{
  "status": "init",
  "point": "setpoint",
  "value": 21.5,
  "value_repr": "number"
}
```

- For a `typed`-writable point, `value` is the logical value in **engineering units** — the same
  units as the sample's `value`, i.e. after the point's `transform` and `map`. The SDK maps it
  back through the value map (§4.3) and then to the raw value (§4.2), and the connector encodes
  that per the point's `datatype`/`endianness`/`word_order`. A mapped point is written with its
  mapped value (`"running"`, not `1`); a value with no inverse fails the write.
- For a `raw`-writable point, the request MUST instead provide `raw` (hex) and the connector
  writes those bytes verbatim.
- The connector MUST reject (`failed`) a write to a point whose `access` does not permit it.

Result (`status: "successful"`):

```json
{ "status": "successful", "point": "setpoint", "value": 21.5 }
```

The result echoes `value` as requested (engineering units), not the raw value written.

Result (`status: "failed"`) — `reason` is free text; the Modbus wording here is illustrative:

```json
{ "status": "failed", "point": "setpoint", "reason": "modbus exception: illegal data address" }
```

### 6.3 Management verbs (standard, SDK-provided)

Beyond point I/O, every connector needs to be (re)configured at runtime: change a poll
interval, adjust serial parameters, add a device, etc. Rather than inventing a bespoke
operation per protocol (as the legacy Modbus plugin did with `c8y_ModbusConfiguration`,
`c8y_SerialConfiguration`, `c8y_ModbusDevice`, `c8y_Coils`, `c8y_Registers`), the contract
defines a small set of **protocol-neutral management verbs**. They are implemented once in the
SDK runtime — it owns the connector configuration document — so every protocol module gets them
for free without any extra code.

A connector that uses the SDK runtime MUST advertise these verbs (and the `management` feature)
in its capability descriptor (§7). All three follow the same `init → executing →
successful/failed` state machine as `write`, but on the connector's **service** command topic:

```text
te/device/main/service/<service>/ot/cmd/<verb>/<id>
```

A management command changes one connector instance's configuration file, so it is addressed to
that instance by its `service_name` rather than to a device topic that every instance of the
protocol receives (§6.5); the affected device is named in the payload. A connector:

- MUST act on management verbs only on its own service command topic;
- MUST reject (`failed`, naming the service topic in `reason`) a management verb received on the
  topic of a device it owns, and any other verb received on its service command topic;
- MUST echo the request's `origin` (§6.4) into every transition, so a bridge can complete the
  command on the entity it was issued for, which the service topic does not name.

After a successful management command the runtime persists the updated configuration to disk and
live-reloads the connector (re-validate, reconnect, reschedule) — no service restart is required.
From then on the connector answers the device commands of the devices the new configuration
defines (§6.5).

#### `set-config` — patch connector configuration

Applies a deep-merged patch to one section of the configuration document. Replaces
`c8y_ModbusConfiguration` (poll/transmit rate) and `c8y_SerialConfiguration` (serial parameters).

Request (`status: "init"`):

```json
{
  "status": "init",
  "target": "connector",
  "config": { "poll_interval": "5s", "log_level": "debug" }
}
```

- `target` selects the section to patch: `"connector"`, `"mqtt"`, `"connection"` (shared
  protocol defaults, e.g. `{ "serial": { "baudrate": 19200 } }`), or `"device:<name>"` to patch a
  single device's fields (its `point` list is left untouched unless included).
- `config` is deep-merged into the target section (objects merge recursively; scalars and arrays
  replace).
- The runtime rejects (`failed`) a `connector` patch that sets `service_name` or `protocol`: the
  service name is the address of the management commands themselves, and the protocol selects the
  module. Both change only by editing the configuration file and restarting the connector.
- The runtime rejects (`failed`) a patch that produces an invalid configuration.

#### `define-device` — add or replace a device

Inserts a device (transport + points) into the configuration, or replaces an existing device with
the same `name`. Replaces `c8y_ModbusDevice` together with the point definitions that
`c8y_Coils`/`c8y_Registers` used to stage. Child-device registration in the cloud is then handled
by the existing registration flow when the device link comes up.

Request (`status: "init"`):

```json
{
  "status": "init",
  "device": {
    "name": "plc-9",
    "protocol_address": { "transport": "tcp", "host": "10.0.0.9", "port": 502, "unit_id": 1 },
    "default_mode": "typed",
    "point": [
      { "id": "temp", "datatype": "float32", "access": "read_write",
        "address": { "table": "holding", "address": 7, "count": 2 } }
    ]
  }
}
```

The `device` object uses the same shape as a `[[device]]` entry in the configuration file (note
the point list key is `point`, matching the file's `[[device.point]]`). It replaces the whole
entry, so a `define-device` for a device switched off with `enabled = false` (§3.3) switches it
back on unless the new entry sets `enabled = false` as well.

#### `remove-device` — delete a device

Removes the named device (and its points) from the configuration and disconnects it.

```json
{ "status": "init", "device": "plc-9" }
```

### 6.4 The `write-batch` verb (standard, SDK-provided)

A parameter set edited in a cloud UI, a recipe download, or a setpoint change that spans several
registers must update *several points as one request*. Rather than have every requester fan out
N `write` commands and reassemble their results, the SDK runtime implements `write-batch` once,
on top of the module's `write`: the writes are executed **sequentially, in request order**, and
the batch **stops at the first failure** (later points are left untouched). The runtime advertises
the verb for every module that implements `write`.

Request (`status: "init"`):

```json
{
  "status": "init",
  "writes": [
    { "point": "setpoint", "value": 21.5 },
    { "point": "run_command", "value": true },
    { "point": "mask", "raw": "00ff" }
  ]
}
```

Each entry follows the `write` request rules (`value` for typed points, `raw` for raw points). An
empty `writes` array is rejected (`failed`), so a malformed request cannot succeed without touching
the device.

A request MAY carry an `origin` object: opaque correlation data the requester attaches. The
connector MUST NOT interpret it, and MUST echo it verbatim into every transition it publishes
for that command (`write` and `write-batch` alike). The topic is retained and holds exactly one
message, so the result *overwrites* the request — without the echo, a consumer that starts or
restarts afterwards replays the terminal state alone and has lost what the request said. That is
what lets a flow still tell which parameter set (§5.2) an acknowledged write belongs to, rather
than guessing and retaining a fragment under a name no definition matches.

Transitions: `executing` carries `points` (the ids about to be written); the terminal message
carries `results`, one entry per *attempted* write in order:

```json
{ "status": "successful",
  "results": [ { "point": "setpoint", "status": "successful", "value": 21.5 },
               { "point": "run_command", "status": "successful", "value": true } ] }
```

```json
{ "status": "failed",
  "reason": "write to run_command failed: access denied: point run_command is not writable",
  "results": [ { "point": "setpoint", "status": "successful", "value": 21.5 },
               { "point": "run_command", "status": "failed",
                 "reason": "write to run_command failed: access denied: point run_command is not writable" } ] }
```

The batch is **not atomic**: a failed batch may have applied the writes listed as `successful`.
A connector MAY implement `write-batch` natively (e.g. one Modbus FC16 for contiguous registers) as
long as it keeps these semantics.

### 6.5 Command ownership

Several connector instances may share a broker — one process running a directory of
configurations starts one instance per file, and further processes may run alongside it — and
every instance of a protocol subscribes to the device command topics of the whole protocol
(`te/device/+/ot/<protocol>/cmd/+/+`). Each command must still be acted on by exactly one of them.
The command topic is retained and holds one message, so a second responder's transitions
overwrite the first's: a fast `failed` ("unknown device") from an instance that does not own the
device would replace the real result of the instance that does.

- A connector MUST act on a device command only when `<device>` is a device its current
  configuration defines, and MUST NOT publish anything for a command addressed to any other
  device — not even `failed`, since it cannot know whether another instance owns that device. A
  command for a device no connector owns therefore stays at `init`; a requester needs its own
  timeout for that case.
- Ownership follows the live configuration: a device added or removed by a management verb
  (§6.3) is answered, or no longer answered, as soon as the change is applied.
- Management commands are addressed to one instance by its service name (§6.3).
- A device MUST be defined by at most one instance per protocol on a broker, and every instance
  MUST have a unique `service_name`. An SDK runtime starting a directory of configurations warns
  about both.

#### Other verbs

`write` is the only point-I/O verb a conformant connector MUST support (for writable points), and
SDK-based connectors additionally provide `write-batch` and the three management verbs above. Connectors MAY support
further verbs (e.g. `read-now`, `rescan`); any such verb MUST be declared in the capability
descriptor (§7) and documented in the connector spec.

## 7. Capability model

On startup a connector MUST publish a retained **capability descriptor** to
`te/device/main/service/<service>/ot/capabilities`. JSON Schema:
[schemas/status.schema.json](schemas/status.schema.json) (`capabilities` definition).

The fields are protocol-neutral; the **values** describe what a given connector supports. The
example below is the Modbus connector's descriptor — a CAN or OPC-UA connector publishes the
same fields with its own values (and typically `"subscribe": true`):

```json
{
  "protocol": "modbus",
  "version": "0.1.0",
  "modes": ["raw", "typed"],
  "datatypes": ["bool", "int16", "uint16", "int32", "uint32", "float32", "float64"],
  "point_kinds": ["coil", "discrete_input", "holding_register", "input_register"],
  "command_verbs": ["write", "set-config", "define-device", "remove-device"],
  "features": ["polling", "bitfield", "management"],
  "subscribe": false,
  "point_labels": [
    { "device": "plc-1", "point": "boiler_temp",
      "name": "Boiler temp", "description": "Outlet temperature after the heat exchanger" }
  ],
  "parameter_keys": [
    { "device": "plc-1", "point": "boiler_setpoint", "key": "setpoint", "group": "control" }
  ],
  "reports": {
    "default": { "max_interval": "30m" },
    "devices": [ { "device": "plc-1", "report": { "on_change": true } } ],
    "points": [
      { "device": "plc-1", "point": "boiler_temp",
        "report": { "on_change": true, "deadband": 0.5, "min_interval": "10s", "max_interval": "30m" } }
    ]
  }
}
```

| Field | Meaning |
| --- | --- |
| `modes` | Output modes supported. MUST include at least one of `raw`/`typed`. |
| `datatypes` | Subset of §4 the connector can decode in `typed` mode. |
| `point_kinds` | Protocol-specific kinds the connector understands (free strings, documented per spec). |
| `command_verbs` | Verbs accepted on `cmd/<verb>`. MUST include `write` if any point is writable; SDK-based connectors also list `write-batch` (§6.4) and the management verbs (§6.3). |
| `features` | Optional capability tags: `polling`, `subscribe`, `bitfield`, `string`, `bulk_read`, … |
| `subscribe` | Whether the connector supports event-driven (push) reads in addition to polling. |
| `point_labels` | The human-readable `name`/`description` of the configured points (§3.1), so a consumer can show something friendlier than the point id. Only points declaring one of them appear, and each entry carries only the fields it declares — **no entry means the id is the label**, so a configuration that labels nothing adds nothing here. Unlike the fields above, this describes the *configuration* rather than the connector's abilities; it lives here because it is static per point, which makes one retained message the right place for it and a per-sample echo the wrong one (§5 samples are a time series). |
| `parameter_keys` | The configured points that name their own key inside their parameter sets (`meta.parameter.key`, §5.2), with the key and any `set` / `group` exactly as configured — so a key of the form `<set>.<key>` names its set too. A consumer learns from it which point a key belongs to before the point samples — after a restart, since samples are not retained, and for a write-only point, which never samples. Only points naming a key appear, so a configuration naming none adds nothing here. Like `point_labels`, this describes the configuration. |
| `reports` | The reporting policies (§5.3): `default` is `[connector] report`, `devices` lists each device declaring a `report`, and `points` lists the **effective** (merged) policy of each point whose policy differs from its device's. A point's policy is its `points` entry if it has one, otherwise its device's entry merged over `default`. Absent when no `report` is configured. Like `point_labels`, this describes the configuration. |

Tooling and the conformance suite use the descriptor to decide which tests apply.

The descriptor is retained, so it MUST be republished whenever something it reports changes.
Everything except `point_labels`, `parameter_keys` and `reports` is a property of the connector build and so
is published once at startup; those three follow the configuration, and a connector MUST therefore republish
the descriptor after a management command (§6.3) changes it — a retained message describing the
configuration as it was at startup is worse than none. Note also that labelling every point of
a large list has a size: two hundred fully labelled points add on the order of ten kilobytes to
this one message. That is paid once per (re)publish, not per sample, which is the reason the
labels live here rather than in the sample envelope.

## 8. Status and health

- The connector MUST publish a retained service health message to
  `te/device/main/service/<service>/status/health` with at least
  `{"status":"up"|"down","time":"<rfc3339>"}` on startup and on health changes, following
  the thin-edge service health convention.
- For each device, the connector SHOULD publish a retained link-status message to
  `te/device/<device>/ot/<protocol>/status/link`:

  ```json
  { "status": "connected", "type": "acme-meter-v2", "points": ["boiler_temp", "setpoint"],
    "since": "2026-05-30T09:59:00.000Z" }
  ```

  `type` is the device's declared type (§3.1), present when it has one: the message is retained
  and published before any sample, which is what lets a consumer name the device's parameter
  sets (§5.2) and its thin-edge entity type from the start — including for a device whose
  parameters are all write-only and therefore never sampled.

  `points` lists the id of every point configured on the device, in configuration order. The
  link status is republished whenever the configuration changes (a management command, a
  reload, a restart), so this is how a consumer keeping state per point — the parameter twin
  (§5.2) — learns that a point was *removed*: missing samples cannot say it, since a write-only
  point is never sampled. SDK runtimes always include it. A consumer MUST read a status without
  it as saying nothing about the points, not as a device that has none. The list makes the
  message grow with the device — roughly the length of each id plus three bytes, so a
  few-hundred-point device passes 10 KiB — and an MQTT client with a small default packet limit
  must have it raised (the Rust runtime allows 1 MiB). Device names are unique only within one
  connector, so a consumer keeps the list per device *and* protocol.

  with `status` ∈ `{"connected","disconnected","degraded"}` and an optional `reason`.

### 8.1 Liveness

A connector that *hangs* is worse than one that fails: a protocol call which never returns
blocks the loop that publishes samples, health and link status, so the device goes silent with
nothing logged and nothing in the cloud marking it unhealthy. An SDK-based connector is
therefore bounded on two levels, both configured in `[connector]`:

| Setting | Default | Effect |
| --- | --- | --- |
| `operation_timeout` | `30s` | Upper bound on one protocol-module call (read batch, write, connect, subscribe). Exceeding it is reported as an ordinary transport error, so the existing `degraded` link and reconnect-with-backoff handling applies. |
| `stall_timeout` | `120s` | How long the loop may make no progress before the connector is considered wedged, cancelled and restarted. The MQTT last will then marks the service `down`, so the outage is visible. Must exceed `operation_timeout`; `0` disables it. |

A connector SHOULD additionally bound its own protocol requests (the Modbus module's
`connection.request_timeout_s`, the OPC UA module's `connection.request_timeout_s`): failing one
request fast keeps the poll cycle on schedule, where the runtime's bound is a backstop that
treats the whole batch as failed. The conformance suite checks this behaviour with a peer that
accepts the connection and answers nothing (check B5, silent peer).

## 9. Timestamps, encoding and ordering

- All timestamps MUST be RFC 3339 / ISO 8601, millisecond precision, UTC with a `Z` suffix.
- All payloads MUST be UTF-8 JSON.
- `raw` hex MUST be lowercase or uppercase consistently within a connector; words SHOULD be
  space-separated in protocol-natural width (e.g. per 16-bit register for Modbus).
- Samples for a single point SHOULD be published in read order; the optional `seq` field lets
  consumers detect reordering or loss.

## 10. Versioning

- This contract is versioned (`Version` header). Connectors declare the contract version they
  target in their capability descriptor's `version` (their own version) and SHOULD document
  the contract version they implement in their spec.
- Backward-incompatible changes to topics, required fields, or the command state machine
  require a new contract major version and an [RFC](../community/community-model.md).

## 11. Conformance

A connector is **contract-conformant** when it:

1. publishes valid samples (§5) for every configured point in its declared modes,
2. publishes a valid capability descriptor (§7) and health/status (§8),
3. implements the `write` verb (§6) for all writable points (SDK-based connectors get
   `write-batch` for free),
4. validates its configuration (§3) and protocol-specific schemas,
5. passes the shared [conformance suite](../conformance/conformance-suite.md), including the
   golden decode vectors for every `typed` datatype it advertises.

See the conformance document for the executable definition of these requirements.
