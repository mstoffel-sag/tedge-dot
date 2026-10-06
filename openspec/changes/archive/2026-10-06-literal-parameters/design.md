## Context

Device parameters (RFC 0003, RFC 0005) map writable points onto Cumulocity device parameters:

- `tedge-dot describe` renders one DTM property definition per *parameter set*. Each definition
  is an object `jsonSchema` with one property per key.
- `ot-parameter-state` keeps one retained twin fragment per set (`{ <key>: value }`).
- `ot-command-forward` turns a `c8y_ParameterUpdate` (`"<set>": { <key>: value, ... }`) into one
  connector `write-batch`. It resolves each key to its point through the flow's claim records
  (`ot-parameter-point:<device>:<set>:<key>`).
- Keys a point names itself are advertised in the retained capability descriptor
  (`parameter_keys`). The flow therefore knows them before the first sample, and for write-only
  points.

Every step assumes the fragment is an object. Users want a point to be the fragment
(`"pump_speed": 42`) when one value is all that is needed.

## Goals / Non-Goals

**Goals:**

- `meta.parameter = { fragment = "<name>" }` publishes the point's value as the fragment itself.
- The full round-trip works for literal parameters: DTM definition, twin, Parameters tab edit,
  point write, twin updated from the result.
- The existing guarantees still hold:
  - the fragment is known before samples (descriptor);
  - removed points are pruned;
  - write-only points show the last acknowledged write;
  - value maps (§4.3) apply.
- Rust and C SDKs at parity, including `describe` stderr.

**Non-Goals:**

- A point that is both a literal fragment and a key in a set, or that names several fragments.
  This could come later by allowing a list.
- A derived (type-qualified) fragment name. The name is verbatim, like `set`, and the operator
  owns its tenant-wide uniqueness.
- Object or array literals. Point values are scalars.
- Changes to the tedge-parameter-plugin or its `c8y_ParameterUpdate` template.

## Decisions

### 1. Syntax: `fragment = "<name>"`, exclusive with `set` / `group` / `key`

The user chose this. A separate option reads clearly ("this point IS the fragment"), and the table
form leaves room for more options later. Alternatives considered:

- `set = "x", literal = true`: overloads `set`, and a "set" with one member is confusing.
- `fragment = true` with a derived name: too long for the use case.

`describe` refuses `fragment` combined with `set`, `group` or `key`, and refuses a name that is
not a plain identifier, the same way it refuses a dotted key with `set`. The runtime does not
validate. As with keys, an invalid fragment makes the flow fall back to the point's usual
placement (default set), and `describe` reports it.

### 2. Flow model: a literal is a set with one reserved key `""`

Literal fragments reuse the claim machinery in `ot-parameter-state` (`claim`, `dropValue`,
`placePoint`, pruning). They do not get a parallel state model. A literal point is placed in
set `<fragment>` under the reserved key `""`, which can never be a configured key, because a key
must be non-empty.

- `twinMessages` publishes `JSON.stringify(values[""])` when a set's only key is `""`, and an
  empty retained message when the set is empty.
- `ot-command-forward` resolves a scalar edit with `pointOf(device, set, "")`. Unlike a key, `""`
  has no fallback to "the key is the point id". When no point has claimed it, the write fails
  with a clear `origin.error`.
- A set cannot hold both shapes. Claiming `""` in a set that already has other keys fails, and so
  does the reverse. The first claimant keeps the set, matching the existing first-claim-wins rule.
  `describe` refuses the configuration up front.

This keeps every pruning and restart path that four review rounds hardened (see the parameter-keys
history). The alternative was a separate `ot-parameter-literal:*` state family, which would need
every pruning case written and tested again.

### 3. Descriptor: `parameter_keys` entries carry `fragment`

A point naming a fragment is listed as `{ device, point, fragment }`, with no `key`, and as
configured. Points with neither `key` nor `fragment` are still omitted. A new descriptor field is
not needed, because the consumer (the flow) and the reason (known before samples, write-only
points) are the same as for keys.

This is an additive change to the contract §7 table.

### 4. DTM rendering: primitive `jsonSchema`

The definition takes `identifier = <fragment>`. Its `jsonSchema` is `property_schema(param)` (type,
title, description, min/max, enum, default, readOnly) plus `$schema`, rather than an object
wrapping it. `order` is dropped, because it has no meaning at the top level. Tags and contexts are
unchanged.

Within one config, two points declaring the same fragment are refused. Across configs (one DTM
identifier tenant-wide), the same literal fragment from several devices merges first-wins, like
keys do. A name used as a literal in one place and as a set in another is refused.

### 5. Write path shapes

`parameterRequest` in `ot-command-forward` accepts these shapes:

- the Cumulocity shape where `op[<fragment>]` is a scalar (number, string or bool) → literal;
- the Cumulocity shape where `op[<fragment>]` is an object → set (unchanged);
- the direct shape `{ "set": "<fragment>", "value": <scalar> }` → literal.

`null` and arrays are rejected. The batch carries one write. `origin` records `set` and
`value`, so `ot-command-result` and `ot-parameter-state` (which uses `origin.set` to place a
write-only point) work unchanged.

## Risks / Trade-offs

- [Cumulocity DTM may not accept a primitive `jsonSchema` as a device-parameter definition, or the
  Parameters tab may send a different operation shape for it.] → Verify early against the live
  tenant (`cloud/modbus`) before touching the flows. If the shape differs, adapt
  `parameterRequest` only.
- [c8y mapper handling of a scalar twin payload, or of its clearing.] → The cloud suite asserts
  both the inventory value and that clearing removes the fragment.
- [Tenant-wide name collisions, since verbatim names are not qualified by device type.] → This is
  the same exposure as `set`, documented as such. `describe` catches collisions within the
  configs it renders.
- [The reserved key `""` leaks into shared state that other flows read.] → Only
  `ot-command-forward` reads `ot-parameter-point:*`, and it is updated in the same change.

## Migration Plan

This is additive, and existing configurations are unaffected. To move a point from a set to a
literal fragment, an operator:

1. adds `fragment` to the point;
2. runs `describe` and registers the new definition;
3. reloads the connector.

The flow drops the point from its old set at the next descriptor or sample (existing "point left a
set" handling).

## Open Questions

- Exact `c8y_ParameterUpdate` payload the Parameters tab sends for a primitive definition. It is
  assumed to be `{ "c8y_ParameterUpdate_<f>": {}, "<f>": <scalar> }`.
  - Verified on the live tenant (cloud suite): the DTM service accepts a primitive definition
    with the `operation` context. The c8y mapper turns a scalar twin payload into
    `"<f>": <scalar>`. An operation of the assumed shape passes through the
    tedge-parameter-plugin template, writes the point, and completes.
  - Not yet verified: that the Parameters tab renders a primitive definition, and that it sends
    exactly this shape. That needs the UI.
