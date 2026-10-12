# Spec Delta

## Purpose

Lets the OPC UA connector call methods on an OPC UA server through the `call-method` command
verb, in both the Rust and the C build.

## ADDED Requirements

### Requirement: Method addressing
The OPC UA connector SHALL address a `call_method` point with `address.object_id` and
`address.method_id`, both textual node ids, and an optional `address.inputs` listing the input
argument datatypes in order. `inputs` entries SHALL be scalar contract datatypes other than
`bytes`. A missing id, an invalid id, an unknown datatype, or a variable address key (`node_id`,
`namespace`, `identifier`, `field`, `index`, `queue_size`) SHALL be rejected at load.

#### Scenario: Valid method point
- **WHEN** a point declares `access = "call_method"` and `address = { object_id = "ns=2;s=Machine", method_id = "ns=2;s=Machine.Multiply", inputs = ["int32", "int32"] }`
- **THEN** the configuration loads

#### Scenario: Method point without a method id
- **WHEN** a `call_method` point declares only `address.object_id`
- **THEN** loading fails with an error naming the point and `method_id`

#### Scenario: Method keys on a variable
- **WHEN** a point with `access = "read"` declares `address.method_id`
- **THEN** loading fails with an error naming the point

### Requirement: Capability descriptor lists call-method
The OPC UA connector SHALL list `call-method` in its capability descriptor's `command_verbs`, in
both builds.

#### Scenario: Descriptor
- **WHEN** the OPC UA connector starts
- **THEN** its capability descriptor's `command_verbs` contains `write`, `write-batch` and `call-method`

### Requirement: Input arguments
The connector SHALL require exactly as many `arguments` as the point declares `inputs`, and SHALL
convert each to its declared datatype with the rules a `write` uses, 64-bit integers also as
decimal strings. A count mismatch or a value that does not fit SHALL fail the command, naming the
argument's position, without calling the method.

#### Scenario: Wrong argument count
- **WHEN** a point declares two `inputs` and the request carries one argument
- **THEN** the command ends `failed` with a reason stating that 2 arguments are expected, and the server receives no call

#### Scenario: Wrong argument type
- **WHEN** a point declares `inputs = ["int32"]` and the request carries `["six"]`
- **THEN** the command ends `failed` with a reason naming argument 1

### Requirement: Calling the method
The connector SHALL call the method with the OPC UA `Call` service on the device's session, using
the configured request timeout. It SHALL fail the command when the device is not connected, the
service fails or times out, the call's status is not good, or an input argument result is not
good, with a reason carrying the OPC UA status name.

#### Scenario: Method returns a bad status
- **WHEN** the server answers a call with `BadInvalidState`
- **THEN** the command ends `failed` with a reason containing `BadInvalidState`

#### Scenario: Server rejects an argument
- **WHEN** the server reports `BadTypeMismatch` for input argument 2
- **THEN** the command ends `failed` with a reason naming argument 2 and `BadTypeMismatch`

#### Scenario: Device disconnected
- **WHEN** a method call targets a device whose session is down
- **THEN** the command ends `failed` with a reason saying the device is not connected

### Requirement: Output arguments
A successful call's `outputs` SHALL list the output arguments in order. Scalars SHALL be rendered
as in a sample's `value`, with 64-bit integers beyond ±2^53−1 as decimal strings. An array SHALL
be a JSON array of such values. Any other output SHALL be `null`; the call still succeeds.

#### Scenario: Integer output
- **WHEN** a method returns one `Int32` output argument with value 42
- **THEN** the result carries `"outputs":[42]`

#### Scenario: Method without outputs
- **WHEN** a method returns no output arguments
- **THEN** the result carries `"outputs":[]`

### Requirement: Namespace translation of method ids
When a device or point library declares a namespace table, the connector SHALL translate
`object_id` and `method_id` each through it, as it does a variable's node id. When either cannot
be translated, the method call SHALL fail with the translation's reason.

#### Scenario: Moved namespace
- **WHEN** the table lists the method's namespace at index 2 and the server lists that URI at index 3
- **THEN** the connector calls the method with both ids in namespace 3

#### Scenario: Namespace missing on the server
- **WHEN** the method's namespace URI is not in the server's namespace array
- **THEN** a method call fails with a reason containing `namespace not found`
