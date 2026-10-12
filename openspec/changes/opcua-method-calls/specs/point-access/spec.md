# Spec Delta

## Purpose

Defines what each point `access` value allows a connector to do with a point — read it, write it
or call it as a method — and the protocol-neutral `call-method` command verb.

## ADDED Requirements

### Requirement: Access values
A point's `access` SHALL be one of `"read"` (the default), `"write"`, `"read_write"` or
`"call_method"`, inline and in a point library, in both the Rust and the C build. Any other value
SHALL be rejected at load, naming the point and the field.

#### Scenario: call_method is accepted
- **WHEN** a point declares `access = "call_method"` in a configuration whose connector supports the `call-method` verb
- **THEN** the configuration loads

#### Scenario: Unknown access is rejected
- **WHEN** a point declares `access = "execute"`
- **THEN** loading fails with an error naming the point and `access`

### Requirement: Points that cannot be read are never read
A point whose `access` is `"write"` or `"call_method"` SHALL NOT be polled, subscribed, read for a
heartbeat or published as a sample, in both builds. Points with `"read"` or `"read_write"` SHALL
be read as before.

#### Scenario: Write-only point publishes no samples
- **WHEN** a device has a point with `access = "write"` and the connector runs for several poll intervals
- **THEN** no sample is published for that point, and the connector issues no read for it

#### Scenario: Write-only point can still be written
- **WHEN** a `write` command targets a point with `access = "write"`
- **THEN** the write is executed and the command ends `successful`

#### Scenario: CLI wildcard read skips points that cannot be read
- **WHEN** `tedge-dot read` is run with a wildcard point pattern on a device with `write` and `call_method` points
- **THEN** neither of those points is read

### Requirement: call_method points are not values
A `call_method` point SHALL NOT declare `datatype`, `mode`, `endianness`, `word_order`,
`transform`, `map`, `poll_interval`, `sampling_interval`, `subscribe`, `report` or
`meta.parameter`, after a library point and the site's patch are merged. Such a declaration SHALL
be rejected at load. A `call_method` point SHALL NOT be a parameter.

#### Scenario: datatype on a method point
- **WHEN** a point declares `access = "call_method"` and `datatype = "int32"`
- **THEN** loading fails with an error naming the point and `datatype`

#### Scenario: Method point is not a parameter
- **WHEN** a configuration contains a `call_method` point
- **THEN** the point appears in no parameter set, DTM definition or `parameter_keys` entry

### Requirement: call_method requires connector support
A configuration containing a `call_method` point SHALL be rejected at load when the connector
does not list `call-method` in its capability descriptor's `command_verbs`.

#### Scenario: Modbus method point
- **WHEN** a Modbus configuration declares a point with `access = "call_method"`
- **THEN** loading fails with an error saying the connector does not support method calls

### Requirement: The call-method verb
A connector that supports method calls SHALL accept
`te/device/<device>/ot/<protocol>/cmd/call-method/<id>` with a request carrying `point` and an
optional `arguments` array (default empty). The command SHALL follow the §6.1 state machine and
echo `origin`. A successful result SHALL carry `point` and an `outputs` array.

#### Scenario: Successful call
- **WHEN** a `call-method` request `{"status":"init","point":"multiply","arguments":[6,7]}` targets a `call_method` point
- **THEN** the command passes through `executing` and ends `{"status":"successful","point":"multiply","outputs":[42]}`

#### Scenario: arguments is not an array
- **WHEN** a `call-method` request carries `"arguments": 5`
- **THEN** the command ends `failed` with a reason saying `arguments` must be an array

### Requirement: Verbs check the point's access
The `call-method` verb SHALL fail for a point whose `access` is not `"call_method"`. The `write`
and `write-batch` verbs SHALL fail for a `call_method` point. In each case the reason SHALL name
the point.

#### Scenario: Method call on a variable
- **WHEN** a `call-method` request targets a point with `access = "read_write"`
- **THEN** the command ends `failed` with a reason saying the point is not a method

#### Scenario: Write to a method point
- **WHEN** a `write` request targets a point with `access = "call_method"`
- **THEN** the command ends `failed` with a reason saying the point is not writable

### Requirement: Method calls reset reporting state
After a `call-method` command, successful or not, the SDK runtimes SHALL reset the device's
reporting state as they do after a write, so the next reading of each point is published.

#### Scenario: Value changed by a method
- **WHEN** a point has `report.on_change = true` and a method call changes its value back to the last published value
- **THEN** the next reading of the point is published anyway

### Requirement: Method calls are not retried
The SDK runtimes and connectors SHALL NOT retry a method call, whatever the failure.

#### Scenario: Timeout
- **WHEN** a method call times out
- **THEN** the command ends `failed` and the method is not called again

### Requirement: The capability descriptor lists the methods
The capability descriptor SHALL carry `methods`, listing every `call_method` point of the
configuration with its `device`, `point`, its `name` / `description` when declared, and its
`address` exactly as configured, in both builds. It SHALL be absent when no point is a method, and
republished when a configuration change alters it.

#### Scenario: Unlabelled method is listed
- **WHEN** a device has a `call_method` point without `name` or `description`
- **THEN** the descriptor's `methods` contains an entry with its device, point id and address

#### Scenario: No methods
- **WHEN** no point of the configuration is a `call_method` point
- **THEN** the descriptor has no `methods` field

### Requirement: Devices with methods advertise the ot_call_method command
The `ot-registration` flow SHALL publish a retained `{"methods":[...]}` on
`te/device/<device>///cmd/ot_call_method` for every registered device the descriptor lists methods
for, with the descriptor's entries without `device`, whichever of the registration and the
descriptor comes first. It SHALL clear the message when a later descriptor lists no methods for
the device, and SHALL NOT republish an unchanged list.

#### Scenario: Descriptor before registration
- **WHEN** the descriptor listing Pump01's methods arrives before Pump01's first connected link status
- **THEN** the capability is published when Pump01 registers

#### Scenario: Methods removed
- **WHEN** a later descriptor lists no methods for Pump01
- **THEN** an empty retained message is published on `te/device/Pump01///cmd/ot_call_method`

#### Scenario: A thin-edge command calls a method
- **WHEN** `{"status":"init","point":"multiply","arguments":[3,4]}` is published on `te/device/machine///cmd/ot_call_method/<id>`
- **THEN** the command ends `successful` with `"outputs":[12]` on the same topic
