## ADDED Requirements

### Requirement: Raw mode for structured values
When a `mode = "raw"` OPC UA point reads a value that is a binary-encoded ExtensionObject, the
connector SHALL publish a sample with `quality = "good"`:

- `raw` SHALL be the encoded body, without the TypeId and the length prefix.
- `addr.encoding_id` and `addr.data_type` SHALL be given in namespace-URI form
  (`nsu=<uri>;<identifier>`).

This SHALL apply to both the poll path and the subscription path.

#### Scenario: Raw structure read
- **WHEN** a raw point addresses a variable that holds a `TestPointXYZ` value `{X=1.5, Y=2.5, Z=3.5}` encoded as three little-endian doubles
- **THEN** the sample is good, `raw` holds the 24 bytes `000000000000f83f` `0000000000000440` `0000000000000c40`, and `addr.encoding_id` names the type's binary encoding node by namespace URI

#### Scenario: Primitive raw points unchanged
- **WHEN** a raw point reads a scalar Double
- **THEN** the sample is identical to one published before this change

### Requirement: Datapoint mode (structure field points)
The OPC UA point address SHALL accept an optional `field`. Its value is a dotted path of
structure field names. Any segment MAY carry one element index `[n]` (see "Array element
points"). Each segment except the last SHALL name a field whose type, after indexing, is a
structure. Configuration validation SHALL reject:

- an empty path or an empty segment;
- `field` combined with `mode = "raw"`;
- `field` combined with a writable `access`.

A field point SHALL otherwise behave as a typed point. It SHALL declare `datatype`, and
`transform`, `map`, `report`, `meta`, `subscribe` and point libraries SHALL apply to it
unchanged.

#### Scenario: Top-level field
- **WHEN** a point declares `address = { node_id = "ns=2;s=Pump1.Status", field = "Speed" }` and `datatype = "float64"`, and the node holds a structure whose `Speed` field is the Double 1450.0
- **THEN** the point publishes a good sample with `value = 1450.0` and `datatype = "float64"`

#### Scenario: Nested field
- **WHEN** a point declares `field = "Motor.Current"` and `datatype = "float32"`, `Motor` is a nested structure field, and its `Current` is the Float 3.2
- **THEN** the point publishes a good sample with `value` equal to 3.2 as a float32

#### Scenario: Field after a variable-length field
- **WHEN** a structure's fields are `Label` (String), `Samples` (an array of Double) and `Count` (UInt32), and a point selects `field = "Count"`
- **THEN** the walker skips `Label` and `Samples` by their encoded lengths, and the point publishes the correct `Count`

#### Scenario: Writable field point rejected
- **WHEN** a field point declares `access = "read_write"`
- **THEN** configuration validation fails with an error that names the point and states that field points are read-only

#### Scenario: Value map on a field
- **WHEN** a field point selects an Enumeration-typed field, declares `datatype = "int32"` and a `map` from 1 to `"running"`, and the field holds 1
- **THEN** the sample has `value = "running"` and `source_value = 1`

### Requirement: Array element points
A point SHALL be able to select **one element** of a one-dimensional array by a fixed,
zero-based index:

- **Inside a structure**, a `field` path segment SHALL accept a suffix `[n]`, for example
  `field = "Samples[3]"` or `field = "Items[2].Value"`. The walker SHALL decode the array's
  length prefix and skip the preceding elements, including variable-length elements and
  structure elements.
- **On an array variable**, the OPC UA address SHALL accept `index = n`. The connector SHALL
  select the element on the server by sending the `IndexRange` `"n"` in the Read and in the
  monitored item. The server answers with an array holding that one element, which the connector
  SHALL unwrap. `index` MAY be combined with `field` when the array's elements are
  structures.

The selected element SHALL then be treated like a scalar of the array's element type. A point
that has an index SHALL be read-only in this change. Configuration validation SHALL reject a
negative or non-integer index. An index at or beyond the array's current length SHALL give bad
samples whose `error` names the index and the length (or the server's `BadIndexRangeNoData`). It
SHALL NOT fail the configuration, because the length may change at runtime.

Selecting a whole array as one value, publishing one sample per element of a runtime-length
array, and indexing arrays with more than one dimension are out of scope. An array without an
index SHALL remain non-selectable.

#### Scenario: Element of an array field
- **WHEN** a point declares `field = "Samples[3]"` and `datatype = "float64"`, and `Samples` is an array of Double holding `[1.0, 2.0, 3.0, 4.5]`
- **THEN** the point publishes a good sample with `value = 4.5`

#### Scenario: Field of a structure element
- **WHEN** a point declares `field = "Items[1].Value"`, `Items` is an array of a structure that has a String `Name` followed by a Double `Value`, and the second element's `Value` is 7.0
- **THEN** the walker skips the first element (including its string) and the point publishes `value = 7.0`

#### Scenario: Element of an array variable
- **WHEN** a point declares `address = { node_id = "ns=2;s=Line.Temperatures", index = 3 }` and `datatype = "float64"` on a `Double[]` variable
- **THEN** the connector reads the node with `IndexRange = "3"`, and the point publishes the fourth element as an ordinary scalar sample

#### Scenario: Index out of range
- **WHEN** a point selects `Samples[5]` and the array currently has 2 elements
- **THEN** the point publishes bad samples whose `error` names index 5 and length 2, and the point recovers without a reload once the array has grown

#### Scenario: Array without index
- **WHEN** a point selects `field = "Samples"`, where `Samples` is an array
- **THEN** the point publishes bad samples stating that the field is an array and needs an index

### Requirement: Additional built-in types
The connector SHALL read the following OPC UA built-in types as **top-level variables**, as
**structure fields** and as **array elements**. The point's declared `datatype` SHALL select the
rendering:

| OPC UA type | `datatype` | Value |
| --- | --- | --- |
| DateTime | `string` | RFC 3339 UTC: `YYYY-MM-DDTHH:MM:SS[.fffffff]Z`. The fraction has up to 7 digits with trailing zeros removed, and is omitted when it is zero. |
| DateTime | `int64` | milliseconds since the Unix epoch, rounded towards negative infinity |
| LocalizedText | `string` | the text; the locale is dropped |
| StatusCode | `uint32` | the 32-bit code |
| StatusCode | `string` | the symbolic name (e.g. `BadNodeIdUnknown`), or `0x` plus 8 upper-case hex digits when the code has no name |
| Guid | `string` | `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`, lower case |
| NodeId | `string` | the Part 6 text form, with `nsu=<uri>;` for a namespace index other than 0 |
| ExpandedNodeId | `string` | as NodeId, prefixed with `svr=<n>;` when the server index is not 0 |
| QualifiedName | `string` | `<name>` for namespace 0, otherwise `<index>:<name>` |
| ByteString | `bytes` | the bytes, as contract §4 defines for `bytes` |

The following rules SHALL also apply:

- A DateTime at or beyond 9999-12-31T23:59:59.9999999Z SHALL render as that value.
- A null String, ByteString or LocalizedText SHALL give an empty value.
- `raw` SHALL follow the existing convention: big-endian bytes of the numeric form (the DateTime
  tick count, the StatusCode), the UTF-8 bytes of the string form, and the bytes themselves for
  a ByteString.
- A `mode = "raw"` point on a variable of one of these types SHALL publish a good sample with
  that `raw`.
- A declared `datatype` that is not listed for the type SHALL give bad samples whose `error`
  names the actual type and the accepted datatypes.
- Writes SHALL remain limited to the existing primitive types and String.

XmlElement, DataValue, DiagnosticInfo and Variant-typed (`BaseDataType`) fields SHALL remain
non-selectable.

#### Scenario: DateTime as text
- **WHEN** a point declares `datatype = "string"` on a DateTime variable holding 2026-10-06 08:15:30.25 UTC
- **THEN** the sample has `value = "2026-10-06T08:15:30.25Z"`

#### Scenario: DateTime as a number
- **WHEN** a point declares `datatype = "int64"` on the same variable
- **THEN** the sample has `value` 1791274530250, the number of milliseconds since the Unix epoch, encoded as int64 values are in contract §4.1

#### Scenario: StatusCode by name
- **WHEN** a field point declares `datatype = "string"` on a StatusCode field holding 0x80340000
- **THEN** the sample has `value = "BadNodeIdUnknown"`

#### Scenario: NodeId with a stable namespace
- **WHEN** a point reads a NodeId value `ns=3;i=1001`, and namespace 3 is `urn:acme:types`
- **THEN** the sample has `value = "nsu=urn:acme:types;i=1001"`

#### Scenario: LocalizedText field
- **WHEN** a field point declares `datatype = "string"` on a LocalizedText field holding `{locale "de-DE", text "Betrieb"}`
- **THEN** the sample has `value = "Betrieb"`

#### Scenario: Unsupported rendering
- **WHEN** a point declares `datatype = "float64"` on a DateTime variable
- **THEN** the point publishes bad samples whose `error` names DateTime and the accepted datatypes `string` and `int64`

### Requirement: Definition resolution from the server
For every node that has field points, the connector SHALL read the variable's `DataType`
attribute and that DataType node's `DataTypeDefinition` attribute once per session, along with
the definitions of any nested structure types it needs. The connector SHALL NOT rely on type
definitions compiled into it, and SHALL NOT browse the server's entire type hierarchy. It SHALL
discard all resolved definitions when a session ends and SHALL resolve them again in the next
session.

When the variable's DataType is abstract (`Structure`, `i=22`, or `BaseDataType`, `i=24`), the
connector SHALL find the concrete DataType from the variable's current value: the
ExtensionObject's encoding id, and the DataType that refers to that encoding by `HasEncoding`.

When the definition cannot be resolved, every field point of that node SHALL publish bad samples
whose `error` starts with `data type definition unavailable`. The device link status SHALL NOT be
degraded for this reason.

#### Scenario: Server without definitions
- **WHEN** a field point addresses a structure variable on a server that does not populate `DataTypeDefinition`
- **THEN** the point publishes bad samples with `error` starting `data type definition unavailable`, the device stays `connected`, and a raw point on the same node still publishes good samples

#### Scenario: Variable declared with the abstract Structure
- **WHEN** a field point selects `X` of a variable whose DataType is `Structure` (`i=22`) and whose value is a `TestPointXYZ` encoded as `nsu=urn:opcua:test-server:custom-types;i=3010`
- **THEN** the connector resolves `TestPointXYZ` through the encoding's `HasEncoding` reference, reads its definition, and publishes `X`

#### Scenario: Definition changes across a reconnect
- **WHEN** the server restarts with a definition that inserts a new field before `Speed`
- **THEN** after the reconnect, the `Speed` point publishes the correct value without a configuration change

### Requirement: Type check against the definition
When the definition is resolved, the connector SHALL check each field point's declared `datatype`
against the field's DataType:

| Field type | Accepted `datatype` |
| --- | --- |
| Boolean | `bool` |
| an integer or floating-point type | the integer or float type of the same width and signedness |
| String | `string` |
| an Enumeration subtype | `int32` |
| a type listed in "Additional built-in types" | the datatypes listed there |

For an indexed array, the check SHALL apply to the element type. A structure, an array
without an index, or any other built-in type SHALL NOT be selectable. A
mismatch or a non-selectable field SHALL give bad samples whose `error` names the field's actual
type, and SHALL NOT fail the configuration. A path that names a field that does not exist SHALL
be treated the same way.

#### Scenario: Declared type mismatch
- **WHEN** a point declares `datatype = "int32"` for a field whose type is Double
- **THEN** the point publishes bad samples whose `error` says that the field is Double and the point declares int32, and the device's other points are unaffected

#### Scenario: Unknown field name
- **WHEN** a point selects `field = "Sped"` and the definition has no such field
- **THEN** the point publishes bad samples whose `error` names the missing field and lists the available field names

### Requirement: Optional fields, unions and malformed bodies
The walker SHALL support the `Structure`, `StructureWithOptionalFields` and `Union` structure
types:

- A selected optional field that is absent according to the encoding mask SHALL give a bad
  sample stating that the field is absent.
- A selected union member that is not the active switch SHALL give a bad sample naming the
  active member.
- A body that ends before the selected field has been decoded, or a length prefix that exceeds
  the remaining body, SHALL give a bad sample. The walker SHALL NOT read beyond the body.
- An ExtensionObject whose encoding id is not the expected type's binary encoding SHALL give a
  bad sample that names both ids.

#### Scenario: Absent optional field
- **WHEN** a structure with optional fields omits `Comment`, and a point selects it
- **THEN** the point publishes a bad sample with `error` stating that `Comment` is absent

#### Scenario: Truncated body
- **WHEN** the body is 5 bytes shorter than the selected field requires
- **THEN** the point publishes a bad sample, and the process neither crashes nor reads beyond the buffer

### Requirement: Shared reads per node
Field points and plain points that share a `node_id` on one device SHALL be served by **one**
Read per poll cycle and **one** monitored item per subscription. Points that select different
`index` values on the same node need separate `IndexRange`s, so they SHALL be separate
ReadValueIds within the same Read request and separate monitored items. Each value SHALL be decoded once
and fanned out as one sample per due or subscribed point. A point that is not due SHALL NOT be
published because another point of the same node was read.

#### Scenario: Three fields, one read
- **WHEN** three field points select different fields of the same node with the same `poll_interval`
- **THEN** each poll cycle issues one Read for that node and publishes three samples

### Requirement: Test vectors and implementation scope
This capability SHALL be implemented by the Rust connector. The vectors in
`doc/contract/test-vectors/opcua-struct/` SHALL each consist of a definition set, a body, a path
and an expected value or error message, in a language-neutral form, and the Rust unit tests SHALL
run every one of them. The Rust walker SHALL have a fuzz target.

The C build SHALL declare the capability `opcua-structures` missing, so that its e2e runs skip
the cases that need it, and a configuration that uses `address.field` or `address.index` SHALL
still load there. A later C implementation SHALL pass the same vectors.

#### Scenario: Vectors pass in the Rust build
- **WHEN** the Rust unit tests run the vectors
- **THEN** every vector produces its expected value and `raw`, or its exact error message

#### Scenario: The C build skips the capability
- **WHEN** the OPC UA e2e suite runs against the C build
- **THEN** the cases tagged `requires:opcua-structures` are reported as skipped, and the configuration with `field` and `index` points loads
