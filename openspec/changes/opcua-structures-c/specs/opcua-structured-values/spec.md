## MODIFIED Requirements

### Requirement: Raw mode for structured values
When a `mode = "raw"` OPC UA point reads a value that is a binary-encoded ExtensionObject, the
connector SHALL publish a sample with `quality = "good"`:

- `raw` SHALL be the encoded body, without the TypeId and the length prefix.
- `addr.encoding_id` and `addr.data_type` SHALL be given in namespace-URI form
  (`nsu=<uri>;<identifier>`).

This SHALL apply to both the poll path and the subscription path.

In the C build a sample's `raw` holds at most 256 bytes. A body longer than that SHALL give a bad
sample whose `error` names the body's length and the limit. The body SHALL NOT be truncated.

#### Scenario: Raw structure read
- **WHEN** a raw point addresses a variable that holds a `TestPointXYZ` value `{X=1.5, Y=2.5, Z=3.5}` encoded as three little-endian doubles
- **THEN** the sample is good, `raw` holds the 24 bytes `000000000000f83f` `0000000000000440` `0000000000000c40`, and `addr.encoding_id` names the type's binary encoding node by namespace URI

#### Scenario: Primitive raw points unchanged
- **WHEN** a raw point reads a scalar Double
- **THEN** the sample is identical to one published before this change

#### Scenario: Body beyond the C raw limit
- **WHEN** a raw point in the C build reads a structure whose body is 300 bytes long
- **THEN** the point publishes a bad sample whose `error` names 300 bytes and the 256-byte limit, and no partial `raw`

### Requirement: Test vectors and implementation scope
This capability SHALL be implemented by both the Rust and the C connector. The vectors in
`doc/contract/test-vectors/opcua-struct/` SHALL each consist of a definition set, a body, a path
and an expected value or error message, in a language-neutral form, and the unit tests of both
builds SHALL run every one of them. Each build's walker SHALL have a fuzz target.

Neither build SHALL declare the capability `opcua-structures` missing, so the e2e cases tagged
`requires:opcua-structures` run against both.

Where the C build's fixed-size sample model differs, it SHALL say so rather than publish a wrong
value: a String or LocalizedText value longer than 255 bytes is cut at a character boundary, as
top-level strings are in C, and a `bytes` value longer than 127 bytes or a raw body longer than
256 bytes is a bad sample naming the limit.

#### Scenario: Vectors pass in both builds
- **WHEN** the Rust and the C unit tests run the vectors
- **THEN** every vector produces its expected value and `raw`, or its exact error message, in both

#### Scenario: The C build runs the capability's e2e cases
- **WHEN** the OPC UA e2e suite runs against the C build
- **THEN** the cases tagged `requires:opcua-structures` run and pass
