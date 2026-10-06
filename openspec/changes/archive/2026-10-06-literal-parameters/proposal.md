## Why

Every device parameter is published to Cumulocity as a key inside an object fragment (a *parameter
set*): `"acme_pump_v2_control_parameters": { "pump_speed": 42 }`. When a single value is all an
operator needs, the object only adds nesting: a longer fragment name, an extra level in the
Parameters tab, and an inventory shape that does not match fragments other tools already use
(`"pump_speed": 42`). There is no way to publish a point as the fragment itself.

## What Changes

- New `meta.parameter.fragment = "<name>"` option: the point is a *literal parameter*. Its value is
  the twin fragment itself (`te/device/<d>///twin/<name>` carries `42`, not `{ "<key>": 42 }`), so
  Cumulocity's inventory holds `"<name>": 42` (number, bool or string).
  - The name is used verbatim, like `set`. It must be a plain identifier (`[A-Za-z0-9_]`).
  - `fragment` cannot be combined with `set`, `group` or `key`, since the point is not in a set.
    The presentation options (`title`, `description`, `min`, `max`, `enum`, `default`, `order`)
    still apply.
- `tedge-dot describe` renders a literal parameter as a DTM property definition with a primitive
  `jsonSchema` (`type` `integer`/`number`/`boolean`/`string`, from the datatype or the value
  map). It does not render an object with one property.
- `describe` refuses these configurations:
  - `fragment` combined with `set`, `group` or `key`;
  - two points of a device naming the same fragment;
  - a fragment name a device also uses as a parameter set;
  - across configurations, a name used as a literal in one place and as a set in another.
- The capability descriptor's `parameter_keys` also lists points that name a `fragment`. The flow
  then knows them before any sample: after a restart, and for write-only points.
- `ot-parameter-state` publishes and prunes literal fragments with the same rules as sets. A
  literal fragment holds a bare value, and an empty retained message removes it.
- `ot-command-forward` turns a `c8y_ParameterUpdate` whose `<fragment>` value is a scalar into a
  write of the one point that owns the fragment. The same applies to the direct `parameter_update`
  shape `{ "set": "<fragment>", "value": <scalar> }`. A scalar sent for an object set fails the
  command, and so does an object sent for a literal fragment.
- Rust and C SDKs are kept at parity (describe output, refusals, descriptor).
- Not breaking: configurations without `fragment` behave as today.

## Capabilities

### New Capabilities

- `literal-parameters`: declaring a point as a literal parameter fragment, rendering its DTM
  definition, advertising it in the capability descriptor, publishing it to the twin, and writing
  it from a `c8y_ParameterUpdate`.

### Modified Capabilities

<!-- None: device parameters (RFC 0003/0005) predate openspec and have no spec in openspec/specs/. -->

## Impact

- **SDKs:**
  - Rust: `impl/rust/crates/sdk/src/descriptor.rs` (parameters, DTM rendering, validation,
    `parameter_keys`) and `impl/rust/src/main.rs` (describe).
  - C: `impl/c/sdk/src/descriptor.c`, `impl/c/sdk/include/tedge_dot/descriptor.h`,
    `impl/c/sdk/src/runtime.c` (`add_parameter_keys`) and `impl/c/tests/describe.c`.
  - The describe parity check.
- **Flows:** `flows/ot-parameter-state/main.js` and `flows/ot-command-forward/main.js`, plus
  their cases in `flows/test-flows.sh`.
- **Contract and docs:** `doc/contract/ot-connector-contract.md` (§5.2 parameters, §7
  `parameter_keys`), `doc/contract/schemas/status.schema.json` if it covers the descriptor,
  `flows/README.md`, the `doc/rfc/0003` and `0005` notes, and `README.md`.
- **Tests:**
  - e2e: modbus and opcua flows cases.
  - Live round-trip: a literal point in `cloud/modbus/modbus.toml` and
    `cloud/modbus/tests/parameters_c8y.robot`.
- **External:** relies on Cumulocity DTM accepting a primitive property definition, and on the
  c8y mapper accepting a scalar twin payload. Both are verified by the cloud suite.
