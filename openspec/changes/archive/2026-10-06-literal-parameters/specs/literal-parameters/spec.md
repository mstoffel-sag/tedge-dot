## ADDED Requirements

### Requirement: Declaring a literal parameter
A connector configuration SHALL accept `meta.parameter.fragment`, a string naming a twin fragment
that the point's value is published as directly. A point with a usable `fragment` SHALL be a
parameter, whatever its `access`, and SHALL belong to no parameter set. The presentation options
`title`, `description`, `min`, `max`, `enum`, `default` and `order` SHALL keep their meaning. The
Rust and C SDKs SHALL treat `fragment` identically.

#### Scenario: Read-write point published as a literal
- **WHEN** a `read_write` `uint16` point `pumpSpeed` declares `meta.parameter = { fragment = "pump_speed" }`
- **THEN** it is a parameter whose fragment is `pump_speed`, and it is in no `<type>_<group>_parameters` set

#### Scenario: Configuration without fragment is unchanged
- **WHEN** no point declares `meta.parameter.fragment`
- **THEN** `describe` output, the capability descriptor and the twin fragments are identical to those before this change

### Requirement: Describe validates literal parameters
`tedge-dot describe` SHALL refuse a configuration, reporting each problem on stderr with the
same wording in Rust and C, when any of these hold:

- a `fragment` is not a non-empty plain identifier (`[A-Za-z0-9_]+`);
- `fragment` is combined with `set`, `group` or `key` on the same point;
- two points of a device name the same fragment;
- a fragment name equals a parameter set name used on the same device;
- across the rendered configurations, one name is used both as a literal fragment and as a
  parameter set.

#### Scenario: Fragment combined with key
- **WHEN** a point declares `meta.parameter = { fragment = "pump_speed", key = "speed" }`
- **THEN** `describe` exits with an error naming the point and the conflicting option

#### Scenario: Duplicate fragment on one device
- **WHEN** two points of the same device declare `fragment = "pump_speed"`
- **THEN** `describe` exits with an error naming both points

#### Scenario: Fragment collides with a set
- **WHEN** a point declares `fragment = "plant_settings"` and another point of the device declares `set = "plant_settings"`
- **THEN** `describe` exits with an error naming the fragment

### Requirement: DTM definition of a literal parameter
`tedge-dot describe` SHALL render one DTM property definition per literal fragment, with
`identifier` equal to the fragment name. Its `jsonSchema` SHALL be a primitive schema rather than
an object, and SHALL contain:

- `$schema`;
- a `type` of `integer`, `number`, `boolean` or `string`, derived from the datatype, or from the
  value map when the point has one (§4.3, including its `enum` of writable outputs);
- `title`, `description`, `minimum`, `maximum`, `enum` and `default`, derived as for a property
  of a set;
- `readOnly: true` when the point cannot be written.

It SHALL NOT contain `order`. A literal fragment declared by several devices or configurations
SHALL be rendered once, from its first declaration.

#### Scenario: Integer literal with limits
- **WHEN** a `uint16` point declares `meta.parameter = { fragment = "pump_speed", title = "Pump speed", max = 3000 }`
- **THEN** the definition has `identifier = "pump_speed"` and a `jsonSchema` with `type = "integer"`, `title = "Pump speed"`, `minimum = 0`, `maximum = 3000`, and no `properties`

#### Scenario: Mapped literal offers choices
- **WHEN** a literal point has a value map whose writable outputs are `"stopped"` and `"running"`
- **THEN** its `jsonSchema` has `type = "string"` and `enum = ["stopped", "running"]`

### Requirement: Capability descriptor lists literal parameters
The capability descriptor's `parameter_keys` SHALL include an entry
`{ "device", "point", "fragment" }` for every configured point that names a `fragment`, with the
fragment as configured. It SHALL be republished when the configuration changes, as for keys.

#### Scenario: Write-only literal is advertised
- **WHEN** a `write` point `resetCounter` on device `plc-1` declares `fragment = "reset_counter"`
- **THEN** the retained descriptor's `parameter_keys` contains `{ "device": "plc-1", "point": "resetCounter", "fragment": "reset_counter" }`

### Requirement: Twin publishing of literal parameters
`ot-parameter-state` SHALL publish a literal parameter's value as the bare JSON value of the
retained message on `te/device/<device>///twin/<fragment>`. The value SHALL come from:

- each good sample of the point;
- a successful write of the point;
- for a write-only point, only the last acknowledged write.

The flow SHALL learn the point→fragment relation from the descriptor's `parameter_keys` and from
samples (`meta.parameter.fragment`). When the point is removed from the configuration, leaves the
fragment, or no longer declares it, the flow SHALL clear the fragment with an empty retained
message. A set SHALL never hold both a literal value and keyed values: the first point to claim
the name keeps it.

#### Scenario: Sample updates the literal fragment
- **WHEN** point `pumpSpeed` with `meta.parameter.fragment = "pump_speed"` samples `42` with good quality
- **THEN** the flow publishes `42` retained on `te/device/<device>///twin/pump_speed`

#### Scenario: Known before the first sample
- **WHEN** the mapper restarts and the retained descriptor declares `pumpSpeed` → `pump_speed`, and no sample has arrived
- **THEN** an edit of `pump_speed` from the cloud is routed to `pumpSpeed`

#### Scenario: Removed point clears the fragment
- **WHEN** the link status no longer lists `pumpSpeed`
- **THEN** the flow publishes an empty retained message on `te/device/<device>///twin/pump_speed`

#### Scenario: Point moves from a set to a literal
- **WHEN** a point previously in `acme_pump_v2_control_parameters` is reconfigured with `fragment = "pump_speed"`
- **THEN** its key is removed from the set fragment and its value is published on `twin/pump_speed`

### Requirement: Writing a literal parameter
`ot-command-forward` SHALL turn a `parameter_update` command into a connector `write-batch` with
exactly one write to the point owning the literal fragment when either:

- the Cumulocity operation's `<fragment>` value is a number, string or bool; or
- the direct payload is `{ "set": "<fragment>", "value": <scalar> }`.

The request's `origin` SHALL carry `set` (the fragment). The command SHALL fail with a reason in
`origin.error`, and with no writes, in each of these cases:

- the fragment is not claimed by any point;
- a scalar is sent for a set that holds keys;
- an object is sent for a literal fragment;
- the value is `null` or an array.

#### Scenario: Parameters tab edits a literal
- **WHEN** Cumulocity sends `{ "c8y_ParameterUpdate": {}, "c8y_ParameterUpdate_pump_speed": {}, "pump_speed": 1500 }`
- **THEN** the connector receives a `write-batch` with `writes = [{ "point": "pumpSpeed", "value": 1500 }]`, and on success the twin fragment `pump_speed` becomes `1500`

#### Scenario: Scalar sent for an object set
- **WHEN** an operation carries `"acme_pump_v2_control_parameters": 5`
- **THEN** the command completes as failed and no point is written

#### Scenario: Unclaimed literal
- **WHEN** an operation carries `"unknown_fragment": 1` and no point has claimed `unknown_fragment`
- **THEN** the command completes as failed with a reason naming the fragment
