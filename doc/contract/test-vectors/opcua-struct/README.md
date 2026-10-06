# OPC UA structured-value test vectors

`vectors.json` pins how a connector decodes one value out of an OPC UA structure
(ExtensionObject body), an array, or one of the additional built-in types, and how it renders
that value into a sample. Both OPC UA connectors run every vector in their unit tests: the
Rust connector (`impl/rust/crates/connector-opcua/src/structure.rs`) and the C connector
(`impl/c/connectors/opcua/structure.c`). The vectors are how the two decoders are kept
identical, messages included. The behaviour is specified in
`openspec/changes/opcua-custom-datatypes` and in the OPC UA connector spec.

## Generating

`generate.py` writes `vectors.json`. Run it with the Python and asyncua of the OPC UA simulator
image:

    uv run --python 3.12 --with 'asyncua~=1.1' doc/contract/test-vectors/opcua-struct/generate.py

The bodies are encoded by asyncua, which shares no code with either connector: each entry in
`types` becomes a real `StructureDefinition`, asyncua generates its class as it does for a
server's `DataTypeDefinition`, and its encoder writes the body. Expected values are written by
hand, and the generator cross-checks every plain-field expectation against asyncua's decoder
before it writes the file. One body is assembled by hand because asyncua cannot encode a
`DiagnosticInfo` nested in another. Its bytes are annotated in the generator.

Do not edit `vectors.json` by hand.

## Format

`namespaces` is the server's namespace array, used to render `nsu=` forms.

`types` holds the data type definitions, keyed by type name:

| Field | Meaning |
| --- | --- |
| `structure_type` | `Structure`, `StructureWithOptionalFields` or `Union`. |
| `fields` | The fields in order. Each has `name`, `type`, and optionally `array` (ValueRank 1) and `optional` (IsOptional). |
| `enum` | Instead of `structure_type`/`fields`: an Enumeration, with its member names. Encoded as Int32. |

A field's `type` is a built-in type name (OPC UA Part 6 §5.1.2: `Boolean`, `Double`,
`LocalizedText`, `Variant`, …) or the name of another entry in `types`.

### `read`

| Field | Meaning |
| --- | --- |
| `name` | What the vector shows. |
| `root` | The type of `body`: an entry in `types`, or a built-in type name for a value decoded on its own (an array element or a top-level variable). |
| `body` | The binary encoding, hex. For a structure it is the ExtensionObject body. |
| `path` | The point's `field` path. Absent when `root` is a built-in type. |
| `datatype` | The point's declared `datatype`. |
| `out` | The sample's `value`: one of `num`, `str`, `bool`, or `hex` for a `bytes` point. Absent when `error` is set. |
| `raw` | The sample's `raw` bytes, hex, without grouping. |
| `error` | The bad sample's `error` text, without the `point <id>: ` prefix the runtime adds. |

`num` is compared by value. A 64-bit integer that is outside the safe range would be a `str`
(contract §4.1). None of the current vectors needs one.

### `invalid_paths`

Field paths the configuration loader rejects, with the message (without the device and point
prefix).
