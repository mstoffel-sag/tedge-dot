## Why

Issue thin-edge/tedge-dot#57 asks for a way to read **custom (structured) data types** from any
OPC UA server, without compiling the type definitions into the connector. A server sends such a
value as an `ExtensionObject`: an encoding id followed by a binary body that is opaque unless the
client knows the structure's layout. Since OPC UA 1.04, a server can describe that layout in the
`DataTypeDefinition` attribute of the type's DataType node. That attribute is a
`StructureDefinition` giving each field's name, data type, array rank and whether it is optional.
OPC UA Part 6 then defines how to decode the body field by field.

Neither build handles this today:

- Rust `variant_to_value` (`impl/rust/crates/connector-opcua/src/lib.rs:1128`) and C
  `variant_to_sample` (`impl/c/connectors/opcua/connector_opcua.c:1304`) only accept the 12
  primitive scalar types. Any other value becomes a **bad sample** ("unsupported OPC-UA value
  type"), including ExtensionObject.
- This happens **before** the raw/typed branch, so `mode = "raw"` cannot read a structure
  either. The raw body that the libraries already hold is thrown away. async-opcua decodes an
  unknown type to `ByteStringBody` through its `FallbackTypeLoader`. open62541 keeps it as
  `UA_EXTENSIONOBJECT_ENCODED_BYTESTRING`.

The issue also asks whether the decoding could happen **in a flow, after reading the raw
value**. A flow can only partly do this:

- It can decode bytes, but only once the connector publishes the body as a good raw sample.
- It has no OPC UA session, so it cannot read the `DataTypeDefinition`. The layout would have to
  be hard-coded in the flow.
- A structure would arrive as one sample, while every downstream flow (`ot-measurement`,
  `ot-alarm`, the parameter twin) works one primitive per point.

This change therefore specifies two modes:

- **Datapoint mode** is the primary path. The user browses the server, finds the structure
  variable and lists the fields they want as points in the TOML. The connector decodes the
  structure using the server's own definition and publishes each field as an ordinary typed
  sample, which the standard flows already understand.
- **Raw mode** is the fallback. The connector publishes the body as a good raw sample together
  with its type id, and a flow decodes it using a layout that the flow author supplies.

A third route, **automatic decoding in a flow**, is recorded as an option for a later change and
is not specified here (see design D9).

## What Changes

- **Raw mode: passthrough of ExtensionObjects** (Rust build). A `mode = "raw"` point whose node holds
  a binary-encoded ExtensionObject publishes a **good** sample:
  - `raw` is the encoded body;
  - `addr` gains `encoding_id` and `data_type` (the namespace URI form `nsu=…`, which stays
    stable across server restarts).

  A flow can decode it, as the issue suggests.
- **Datapoint mode: structure field points** (Rust build). The OPC UA point address accepts an optional `field`:
  `address = { node_id = "ns=2;s=Pump1.Status", field = "Speed" }`. Nested structures use a dotted
  path (`field = "Motor.Current"`).
  - The connector reads the variable's `DataType` and that type's `DataTypeDefinition` once per
    session, and again after each reconnect, so a server update is picked up.
  - It decodes the body with a Part 6 walker and publishes the selected field as an ordinary
    typed sample.
  - A field must be a scalar of a type the SDK can represent: Boolean, the integers, Float,
    Double, String, or an Enumeration (decoded as int32).
  - **Array elements by fixed index**: `field = "Samples[3]"` or `"Items[1].Value"` inside a
    structure, and `index = 3` on an array variable (selected on the server through
    `IndexRange`). An out-of-range index gives bad samples until the array grows. Whole arrays
    and runtime-length arrays remain unsupported.
  - The walker can **skip** every built-in type, including arrays, Variant, DataValue,
    DiagnosticInfo and nested structures, so fields that come after them can still be reached.
  - Optional fields that are absent, and union members that are not the active switch, give a
    bad sample with a stated reason.
- **More built-in types** (Rust build), as top-level variables, structure fields and array
  elements, each rendered into an existing SDK datatype that the point's `datatype` selects:
  - DateTime → `string` (RFC 3339 UTC) or `int64` (Unix ms);
  - LocalizedText → `string` (the text);
  - StatusCode → `uint32` or `string` (the symbolic name);
  - Guid, NodeId, ExpandedNodeId and QualifiedName → `string` (text form, with `nsu=` for
    namespaces);
  - ByteString → `bytes` or raw.

  These are read-only. XmlElement, DataValue, DiagnosticInfo and Variant-typed fields remain
  unsupported.
- **Reads are shared.** Several field points on the same node cost one Read per poll cycle and
  one monitored item per subscription. Each notification fans out to one sample per field point.
- **Points stay typed and primitive.** A field point must declare `datatype`. The type is checked
  against the definition when the definition is resolved: a mismatch gives bad samples that name
  the field's actual type, and does not fail the config. `transform`, `map`, `report`, `meta`
  and point libraries all apply unchanged. Field points are **read-only**: a writable `access` (`write`, `read_write`) is
  rejected at configuration time, because writing one field needs a non-atomic
  read-modify-write of the whole structure.
- **No definition.** If the server has no `DataTypeDefinition` for the type (a pre-1.04 server,
  or the UA-.NETStandard interop build), the field points give bad samples ("data type
  definition unavailable"). The device link stays up and the raw route still works.
- **Contract amendment.** §4 lists structure field selection as an allowed decoding refinement,
  next to bit-field extraction. The driver still performs no renaming or shaping, and `value`
  stays a number, boolean or string.
- **Shared test vectors** of the form definition + body hex → field values or a skip error. Both
  builds run them. Add a Rust fuzz target for the walker, because it parses external input.
- **Simulator.** The asyncua simulator gains structure variables whose `DataTypeDefinition` is
  populated: nested, optional fields, union, and an array field before a scalar.
- **Interop.** The UA-.NETStandard server's `ExtensionObjects/PointValue` (three little-endian
  doubles, no definition) covers the raw passthrough and the "definition unavailable" path.
- **Docs.** The OPC UA connector spec, a point-library example, and a short example in the flows
  README of decoding a raw body in a flow.

Option, not in this change (see design D9):

- **Automatic decoding.** The connector would publish each resolved definition retained on a
  typedef topic, and a generic `ot-struct` flow would decode raw samples with it into one
  measurement group per structure. Nobody would need to write fields or a layout.

Non-goals (see design):

- the C implementation (design D10);
- writing structures;
- whole arrays as one value, one sample per element of a runtime-length array, and matrices;
- XmlElement, DataValue, DiagnosticInfo and Variant-typed fields, and writes of the newly readable built-in types;
- an object or JSON `value` in the envelope;
- legacy (1.03) `DataTypeDictionary` / `.bsd` definitions;
- XML or JSON encoded ExtensionObjects.

## Capabilities

### New Capabilities
- `opcua-structured-values`: reading OPC UA ExtensionObject values. This covers raw-body
  passthrough with type identification; field points decoded from the server's
  `DataTypeDefinition`; definition resolution and refresh; the shared read per node; the error
  behaviour; and the capability gap the C build records.

### Modified Capabilities
<!-- None: opcua-pki-management, opcua-secure-connections and point-value-mapping are unaffected.
     point-value-mapping applies to field points unchanged, because they are ordinary typed points. -->

## Impact

- **Contract and schemas**:
  - `doc/contract/ot-connector-contract.md` §4: structure field selection.
  - §5: `raw` for a structure is the encoded body.
  - `doc/contract/schemas/config.schema.json`: the OPC UA address `field`.
  - New vectors in `doc/contract/test-vectors/opcua-struct/*.json`.
- **Rust**:
  - `impl/rust/crates/connector-opcua/src/{config.rs,lib.rs}`.
  - A new pure `structure.rs` module: the definition model, the Part 6 walker and field-path
    resolution.
  - The definition is read with a plain Read of `AttributeId::DataTypeDefinition`. No
    `DataTypeTreeBuilder` is used, so the connector never browses a server's whole type tree.
  - A new fuzz target.
- **C: not implemented in this change** (design D10). The C build records the capability
  `opcua-structures` as missing (`C_MISSING_CAPABILITIES`, the parity table in
  `impl/c/README.md`), so its e2e runs skip the new cases. The vectors are language-neutral, so
  a later C implementation is held to the same behaviour.
- **Tests**:
  - `connectors/opcua/sim/server.py` (new structure nodes);
  - the OPC UA e2e suite;
  - `connectors/opcua/interop` (PointValue);
  - the C e2e run, to confirm the shared configuration still loads there and the new cases are
    skipped.
- **Docs**: `doc/connectors/opcua-connector-spec.md` (§2 capabilities, §3.4 address, a new
  structured-values section), `impl/c/README.md` (the parity table), `flows/README.md`,
  `demo/points.d/opcua/`, and `packaging/release-notes.md`.
- **Behaviour**: no change for existing configs. A raw point on an ExtensionObject node used to
  produce bad samples and now produces good ones.
