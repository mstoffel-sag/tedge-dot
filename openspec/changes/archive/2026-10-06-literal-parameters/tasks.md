## 1. Verify the Cumulocity side first

- [ ] 1.1 Against the live tenant, register a DTM property definition with a primitive `jsonSchema` (e.g. `identifier = "pump_speed"`, `type = "integer"`). Confirm the Parameters tab shows it for a device carrying `"pump_speed": 42`.
- [ ] 1.2 Edit it in the Parameters tab and capture the exact `c8y_ParameterUpdate` operation payload. Update design.md Open Questions and the write-path shape if it differs.
- [x] 1.3 Confirm the c8y mapper turns a scalar retained payload on `te/device/<d>///twin/<f>` into `"<f>": <scalar>`, and that an empty retained message removes the fragment.

## 2. Rust SDK (describe + descriptor)

- [x] 2.1 `descriptor.rs`: carry a literal fragment on `Parameter` (e.g. `fragment: Option<String>`). In `parameters_of`, give a point with `fragment` a single parameter whose set is the fragment and whose key is the reserved `""`.
- [x] 2.2 Validation: refuse an invalid fragment, `fragment` combined with `set`/`group`/`key`, a duplicate fragment per device, and a fragment equal to a set on the device (extend `invalid_keys` / `key_conflicts` and the `_across` variants). Also refuse a cross-config literal/set clash.
- [x] 2.3 `c8y_dtm_definitions_across`: render literal fragments with a primitive `jsonSchema` (`property_schema` + `$schema`, no `order`). Merge first-wins across devices and configs.
- [x] 2.4 `parameter_keys`: emit `{ device, point, fragment }` for points naming a fragment.
- [x] 2.5 Unit tests: each requirement scenario (integer literal schema, mapped literal enum, refusals, descriptor entry, unchanged output without `fragment`).

## 3. C SDK parity

- [x] 3.1 `descriptor.c` / `descriptor.h`: the same parameter model, validation messages, and primitive DTM rendering.
- [x] 3.2 `runtime.c` `add_parameter_keys`: emit `fragment` entries.
- [x] 3.3 `impl/c/tests/describe.c` cases mirroring 2.5. Run the describe parity check so stdout and stderr match Rust.

## 4. Flows

- [x] 4.1 `ot-parameter-state`:
  - read `meta.parameter.fragment` from samples and `fragment` from descriptor entries, and place the point in set `<fragment>` under key `""`;
  - refuse mixing `""` with other keys in one set (first claim wins);
  - `twinMessages` publishes the bare value for a literal set.
- [x] 4.2 `ot-command-forward`:
  - accept a scalar `op[<fragment>]` and the direct `{ set, value }` shape, and resolve the point with `pointOf(..., "")`, with no id fallback;
  - fail on unclaimed, shape-mismatched, `null` or array values via `origin.error`.
- [x] 4.3 Update the header comments of both flows (shared state, shapes) and `flows/README.md`.
- [x] 4.4 `flows/test-flows.sh` cases:
  - sample → literal twin;
  - descriptor before sample (restart);
  - write-only literal via write result;
  - pruning on link status;
  - set → literal move and the reverse;
  - literal/set clash;
  - scalar operation → one-write batch;
  - each failure shape.

## 5. Contract and docs

- [x] 5.1 `doc/contract/ot-connector-contract.md`: document `fragment` in §5.2 and the `fragment` entry in §7 `parameter_keys`. Update `doc/contract/schemas/*.json` where the descriptor is described.
- [x] 5.2 Add a literal-parameter example to `README.md` and the RFC 0003/0005 notes.

## 6. End-to-end

- [x] 6.1 modbus and opcua e2e (`flows` tag): a literal point publishes a bare twin value and is written via `parameter_update`. Run for both `IMPL=rust` and `IMPL=c`.
- [x] 6.2 `cloud/modbus/modbus.toml`: add a literal point. In `parameters_c8y.robot`, add:
  - the definition is rendered and registered;
  - the inventory holds the scalar fragment;
  - a Parameters-tab operation writes the point and the fragment reflects it.
- [x] 6.3 Run `just build` and then the cloud suite. Confirm all existing parameter tests still pass.
