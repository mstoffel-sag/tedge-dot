## Context

An OPC UA structured value reaches the client as an `ExtensionObject`: a `TypeId` (the binary
**encoding** node, not the DataType node), an encoding byte, and a length-prefixed body. A client
that does not know the type keeps the body opaque:

- **async-opcua 0.18** (pinned in `impl/rust/Cargo.lock`, features `["client"]`). With no type
  loader registered for the type, its `FallbackTypeLoader` yields `ByteStringBody`, and
  `raw_body()` and `encoding_id()` return the body and the encoding id
  (`async-opcua-types/src/type_loader/fallback.rs`). It also ships `DynamicTypeLoader` and
  `DataTypeTreeBuilder`. We do not use them; see D3.
- **open62541 v1.5.5** (FetchContent in `impl/c/CMakeLists.txt`). It keeps unknown types as
  `UA_EXTENSIONOBJECT_ENCODED_BYTESTRING`, and it offers
  `UA_Client_readDatatypeDefinitionAttribute`.

Today both connectors give up before they look at the body:

- Rust: `variant_to_value` falls through to `None` (`lib.rs:1150`), and `build_sample` turns
  that into a bad sample ahead of the raw/typed branch (`lib.rs:1241`).
- C: the `UA_Variant_hasScalarType` chain ends in `tdot_sample_bad` (`connector_opcua.c:1402`).

The value model is primitive by design:

- Rust `Value` is `Bool | Number | Text`, and C `tdot_value_t` is NONE/BOOL/NUM/STR with a 255-byte
  string and a 256-byte `raw`.
- Contract §4 limits the driver to primitive decoding plus bit-field extraction.
- Every flow consumes one primitive per point.

## Goals / Non-Goals

**Goals:**
- Read individual fields of any binary-encoded structure from a server that publishes a
  `DataTypeDefinition`, with no type knowledge compiled into the connector and identical
  behaviour in Rust and C.
- Make the raw body available as a good sample, so that a flow can decode anything the connector
  cannot. This answers the issue's "decode in a flow" question.
- Keep the envelope, the flows and the SDK value model unchanged. A field point is an ordinary
  typed point.

**Non-Goals:**
- **Writes** of a structure or of a field. Writing a field is a read-modify-write of the whole
  body, which is not atomic against other writers. It is deferred until someone needs it, and
  would most likely be a `write` of the whole raw body.
- **Whole arrays and runtime-length arrays.** A whole array as one value needs a list `value`,
  which is the same contract change as the JSON object. One sample per element of an array whose
  length is known only at runtime has the same "rows at runtime" problem as SNMP table walks in
  TODO.md. Matrices (arrays with more than one dimension) are also out of scope. Only fixed
  element indices are supported (D7).
- **XmlElement, DataValue, DiagnosticInfo and Variant-typed fields.** They have no sensible
  mapping to a number, boolean or string. They are skipped correctly but cannot be selected.
- **An object or JSON `value`** carrying the whole structure. That needs a new `value_repr`,
  schema changes, a new variant in both value models, and every flow taught to handle it, and C
  could not hold it within its 255-byte strings. Field points give the same information within
  the existing contract.
- **Legacy `DataTypeDictionary` (`.bsd` XML)** definitions from 1.03 servers. See Open
  Questions.
- **XML- or JSON-encoded ExtensionObjects.** Only the binary encoding is used on UA-TCP.

## Decisions

### D1: Field selection lives in the point address

```toml
[[device.point]]
name     = "pump1_speed"
address  = { node_id = "ns=2;s=Pump1.Status", field = "Speed" }
datatype = "float64"
unit     = "rpm"

[[device.point]]
name     = "pump1_motor_current"
address  = { node_id = "ns=2;s=Pump1.Status", field = "Motor.Current" }
datatype = "float32"
```

- `field` is a dotted path of structure field names. Each segment except the last must name a
  field whose type is itself a structure.
- A field name that contains `.` cannot be addressed. This is accepted as a limitation, because
  OPC UA browse names conventionally don't contain one.
- `field` is part of the address because it says *what* is read, just as a Modbus bit index
  does. That puts it in the right place for point libraries: a library can describe a device
  type's structure once, as a list of field points.

*Alternatives:*
- **A `fields` table on one point that emits many samples.** This breaks "one sample per
  point", and with it `report`, `map` and the link-status point list.
- **Decoding in a flow.** A flow has no session to read the definition. It stays possible
  through D4.

### D2: Typed and validated late, not at config time
A field point must declare `datatype`, as every typed OPC UA point already must
(`lib.rs:268`). The configuration cannot know the field's real type, because that comes from
the server. The check therefore happens when the definition is resolved (D3):

| Field's DataType | Accepted `datatype` |
| --- | --- |
| Boolean | `bool` |
| SByte / Byte / Int16 / UInt16 / Int32 / UInt32 / Int64 / UInt64 | the matching `int*` / `uint*` |
| Float / Double | `float32` / `float64` |
| String | `string` |
| a subtype of Enumeration | `int32` |

The built-in types from D8 are accepted with the datatypes listed there. For an indexed array
(D7) the element type is checked. Anything else is not selectable:
structures, arrays without an index, ByteString, DateTime, Guid, NodeId,
LocalizedText, Variant and so on. A mismatch, or a field that cannot be selected, gives bad
samples whose `error` names the field's actual type, for example
`field "Speed" is Double, point declares int32`. The config is not failed, because the server
can change its definition and other devices on the same connector must keep running.

Static checks happen at config time:

- `field` is a non-empty path with no empty segments;
- `mode = "typed"`;
- `access` is read-only.

`mode = "raw"` with `field` is rejected, because the raw route is the whole body (D4).

### D3: Our own Part 6 walker, driven by a definition read per data type
For each distinct node that has field points, once per session:

1. Read the variable's `DataType` attribute. If it is abstract (`Structure` or
   `BaseDataType`, as many servers declare, the OPC Foundation reference server among them),
   read the value once and follow its encoding id back to the concrete DataType through the
   inverse `HasEncoding` reference. *Found in implementation: the reference server's
   `ExtensionObjects` variables are declared this way.*
2. Read that DataType node's `DataTypeDefinition`, getting a `StructureDefinition` with its
   `structureType` (Structure, StructureWithOptionalFields or Union), its `fields[]` and each
   field's `dataType` and `valueRank`.
3. Recursively resolve the definitions of nested structure types that lie on a selected path or
   must be *skipped* before it. For nested types, cache the result per DataType node id within
   the session.
4. Compile each field point into a **plan**: the sequence of skip and descend steps that leads
   to its value, plus the expected built-in type.

When a sample arrives, the walker runs the plan over the body:

- a value that is present gives a typed sample;
- an absent optional field, or a union member that is not the active switch, gives a bad sample
  with that reason;
- a body that ends early, or an encoding id that does not belong to the expected type, gives a
  bad sample that names it.

The walker is a **pure function** of `(definition, body, path)`,
`connector-opcua/src/structure.rs`, so that a later C port (D10) can share its vectors. It must be
able to skip every built-in type in Part 6 §5.2: fixed-width types, String, ByteString and
XmlElement, Guid, NodeId and ExpandedNodeId in all their encodings, QualifiedName,
LocalizedText, ExtensionObject (length-prefixed, so it can be skipped without a definition),
Variant (including arrays and dimensions), DataValue, DiagnosticInfo (recursive), and arrays
of any of these.

Why not the libraries' own dynamic decoders:

- **async-opcua `DataTypeTreeBuilder`** browses the server's *entire* DataTypes folder, which
  can mean thousands of nodes on a large server, at every connect.
- **open62541 `UA_Client_getRemoteDataTypes`** builds padded C structs that we would then have
  to walk generically again. We could not verify whether our reduced FetchContent build keeps
  `UA_ENABLE_TYPEDESCRIPTION`.
- With our own walker, the two builds share one algorithm and one set of test vectors. This is
  the same parity arrangement as `decode_primitive`, and it satisfies the validation policy's
  "parsers of external input require a fuzz target".

*Cost:* about 400 lines per build. That is acceptable for a format that is fixed by the
specification.

### D4: Raw passthrough for anything else
A `mode = "raw"` point on an ExtensionObject node publishes a **good** sample:

- `raw` is the body bytes (not including the TypeId and the length prefix);
- `addr.encoding_id` and `addr.data_type` are given as `nsu=<uri>;<id>`, because namespace
  indices are not stable (see the namespace-URI TODO);
- `addr.data_type` is resolved from the variable's `DataType` attribute once per session.

This gives a flow everything it needs to recognise and decode a type that the connector cannot
handle, for example a 1.03 server with no `DataTypeDefinition`.

For a later C port (D10): a body longer than `TDOT_RAW_MAX` would have to give a bad sample
naming the limit rather than be truncated. Field points would not be subject to it, because the
body stays in open62541's memory and only the decoded primitive is copied out.

`report` on a raw point already compares raw hex (`report.rs:206`), so on-change works for it.

### D5: One read and one monitored item per node
Field points that share a `node_id` on a device are grouped:

- **Poll path:** the batch is deduplicated by node, the node is read once, and its value is
  fanned out to every due field point of that node. A point that is not due is not published,
  even though its node was read.
- **Push path:** there is one monitored item per node. Each notification is decoded once per
  plan, and one sample is emitted per field point that has `subscribe` enabled.
- A plain point and field points on the same node also share the read.

### D6: Resolution failures are point-level, refreshed per session
If the definition cannot be read (`BadAttributeIdInvalid`, an empty definition, a nested type
without a definition, or an unsupported `structureType`), every field point of that node
publishes bad samples on each cycle with `error = "data type definition unavailable: <reason>"`.

- The device link is **not** degraded by this. It is a configuration-versus-server mismatch, not
  a transport fault.
- Definitions and plans are dropped and rebuilt at every new session, so a server firmware update
  is picked up on reconnect.
- They are not refreshed within a session. A server that changes a definition without a restart
  is out of scope, and a mid-session mismatch shows up as decode errors on the affected points.

### D7: Fixed array elements by index
A point can address one element of a one-dimensional array, in two places:

```toml
# array field inside a structure: the walker indexes it
address = { node_id = "ns=2;s=Pump1.Status", field = "Samples[3]" }
address = { node_id = "ns=2;s=Order", field = "Items[1].Value" }

# array variable: the server indexes it
address = { node_id = "ns=2;s=Line.Temperatures", index = 3 }
address = { node_id = "ns=2;s=Recipes", index = 0, field = "Setpoint" }  # array of structures
```

- **Inside a structure**, the walker already decodes each array's length prefix in order to skip
  the array. Indexing means skipping `n` elements and then decoding one. For variable-length
  elements (strings, structures) the skip is element by element.
- **On an array variable**, the server does the work through `IndexRange = "n"` on the
  `ReadValueId` and on the monitored item. Both libraries support this, and async-opcua already
  sends an empty `index_range` today. The server answers with an **array of one element**
  (OPC UA Part 4: an IndexRange selects a sub-range, it does not turn an array into a scalar), so
  the connector unwraps it and the existing conversion code handles the element unchanged. With
  `field`, the element is an ExtensionObject and goes through the walker. *Found in
  implementation: the design first assumed a scalar.* Points on the same node with different indices cannot share an `IndexRange`, so they
  are separate `ReadValueId`s in one Read request and separate monitored items.
- **Out of range.** An index beyond the current length is a runtime condition, not a config
  error. It gives bad samples (`index 5 out of range, length 2`, or the server's
  `BadIndexRangeNoData`), and the point recovers by itself when the array grows.
- Indexed points are read-only, like field points. Writing one element through `IndexRange` is
  possible in OPC UA and could be added later.

This covers fixed-slot data such as zones, axes, phases and recipe slots, with every element an
ordinary typed point. It deliberately does not cover "every element, however many there are".

### D8: More built-in types, mapped onto existing datatypes
DateTime, LocalizedText, StatusCode, Guid, NodeId, ExpandedNodeId, QualifiedName and
ByteString become readable everywhere a primitive is: as a top-level variable, as a structure
field and as an array element. Today a top-level `DateTime` variable such as a "last
maintenance" timestamp cannot be read at all.

No new SDK datatype and no contract change are needed. Each type renders into an existing
datatype, and where there are two sensible forms, the point's declared `datatype` chooses:

- **DateTime.** `string` gives RFC 3339 UTC, and `int64` gives Unix milliseconds. The text form
  keeps OPC UA's 100 ns resolution (up to 7 fraction digits, trimmed), so nothing is lost. The
  number form matches what `ts_ms` already uses. OPC UA's "not set" value (0) is rendered
  literally (`1601-01-01T00:00:00Z`, or -11644473600000), rather than turned into an error or a
  special value, so the sample stays good and a flow can recognise it. Part 6's clamp at year
  9999 keeps RFC 3339 valid.
- **StatusCode.** `uint32` gives the code, and `string` gives the symbolic name. Both libraries
  ship the name table. An unnamed code falls back to hex, so the output is the same in both
  builds.
- **NodeId and ExpandedNodeId** use `nsu=` for a namespace other than 0, for the same reason as
  D4: an index is not stable across a server update. This needs the server's `NamespaceArray`,
  which is read once per session and is also needed by D4.
- **QualifiedName** keeps the index form `<index>:<name>`, its Part 6 text form. There is no
  standard URI form for it.
- **LocalizedText** gives only the text. The locale is dropped, because a value per locale is
  not something a point can carry.
- **ByteString** maps to the SDK's existing `bytes` datatype, which is added to the OPC UA
  capability descriptor, or to raw mode.

**Behaviour change for existing points.** For primitive types, the value still follows the
server's type, exactly as today. For the new types, the declared `datatype` chooses the
rendering and is checked: an unlisted combination gives bad samples naming the accepted
datatypes, as in D2. These nodes produced only bad samples before, so no working configuration
changes.

**Writes stay out of scope.** A `string` point on a DateTime node still writes a String, which
the server rejects with `BadTypeMismatch`. Parsing these types back from their text forms is
deferred.

### D10: Rust only; the C build records the gap
The C implementation is not part of this change. It would need its own walker, resolution through
open62541, and per-tick shared reads (as its SNMP module does). Instead:

- the C build declares the capability `opcua-structures` missing (`C_MISSING_CAPABILITIES` in
  the justfile, the parity table in `impl/c/README.md`), so its e2e runs skip those cases;
- the shared e2e configuration still loads in C, where `field` and `index` are ignored and those
  points give bad samples;
- *added in implementation*: the C SDK gains the `bytes` datatype after all, and the C module reads
  a top-level ByteString as `bytes`, so the one built-in type outside the structure walker is at
  parity. Before, `datatype = "bytes"` parsed to "no datatype", and a Rust configuration failed to
  load in C with "typed point requires a datatype". A `bytes` value is limited to 127 bytes there
  (the hex fills the fixed 256-byte value buffer); a longer one is a bad sample, never truncated;
- the structure device of the e2e harness is a connector instance of its own
  (`connectors/opcua/connector-structures.toml`). The C runtime runs one loop per configuration
  file, and in a single file the extra device's reads, all timing out while the suite freezes
  the simulator, delayed the push-failure detection of the other devices past the test's wait
  (*found in implementation*: "Push Delivery Recovers From A Silent Server" failed for C only);
- the vectors are language-neutral (definitions, body hex, expected value, `raw` and exact
  message), so a later C walker is held to the same behaviour.

### D9: Option, not in this change: automatic decoding in a flow
With datapoint mode the user picks the fields. With raw mode a flow decodes, but only with a
layout that the flow author hard-codes per type id, because a flow has no OPC UA session from
which to read the definition. An automatic route would close that gap:

1. The connector publishes each definition it has resolved (for raw points as well as field
   points) **retained**, once per type and per session, at
   `…/ot/opcua/typedef/<url-encoded data_type>`. The payload is the `StructureDefinition` as
   JSON, with field names, types, the optional/union flags and the nested definitions inlined.
2. A generic `ot-struct` flow keeps these definitions as state (as `ot-alarm` keeps
   `status/link`). It looks up each raw sample's `addr.data_type`, walks the body, and sends one
   measurement group per structure, for example
   `{"pump1_status": {"Running": 1, "Speed": 1450.0, "Motor.Current": 3.2}}`.

The configuration would then be one raw point per variable, with no field names, no datatypes
and no layout.

It is not part of this change because:

- it adds a third walker (JavaScript) beside Rust and C;
- `map`, `report`, `unit` and per-field alarms do not apply inside a structure decoded by a
  flow;
- strings and enums have no place in a measurement;
- in C it is bounded by the 256-byte raw limit (D4).

This change keeps the option open: the `addr.data_type` from D4 is the lookup key the flow would
use, and the shared vectors from D3 would be its test suite. It should be decided when there is
demand for "everything in a structure" without configuring individual fields.

## Risks / Trade-offs

- **Definitions that are wrong or inconsistent with the body.** The walker checks the
  ExtensionObject's encoding id against the type's encoding (read from the DataType node's
  `HasEncoding` "Default Binary" reference at resolution), and it never reads past the body.
  Otherwise the definition is trusted, which is the same trust a compiled header would get.
- **No test server with definitions in CI today.** The UA-.NETStandard interop build has no
  `DataTypeDefinition`. → Extend the asyncua simulator with `new_struct`, which populates the
  definition. Add the shared vectors so the walker is tested without a server.
- **Drift once a second walker exists (C, D10).** → The vectors are the contract: they are
  language-neutral and pin values, `raw` bytes and exact messages.
- **Performance.** One extra Read of two attributes per distinct node per session, plus one per
  nested type. Decoding is linear in the body size, and the plans are precompiled.

## Migration Plan

This change is additive. Existing points are unaffected, and raw points on ExtensionObject
nodes switch from bad to good samples. Rollback means removing the `field` points.

## Open Questions

- **Legacy 1.03 servers (DataTypeDictionary).** Should a later change parse the `.bsd`
  dictionary, or let a point library declare a layout for servers without definitions? Raw
  passthrough plus a flow covers them meanwhile. This should be decided once a real 1.03 device
  turns up.
- **Make `datatype` optional for field points?** The server's definition already states the
  field's type, so requiring it in the TOML is redundant. Taking it from the definition would
  mean the point's datatype is known only after the definition is resolved, which affects the
  capability descriptor and the link status. This change keeps it required (D2).
- **An `inspect` CLI command** (`tedge-dot-opcua inspect --node <id>`) that prints a structure's
  fields and types and emits ready-to-paste `[[device.point]]` blocks. It would reuse the
  resolution from D3 and save switching to an external browser such as UaExpert. It is a
  candidate follow-up.
- **Should `field` also accept a numeric index** (`field = "#2"`) for servers whose field names
  are not stable or are localized? Proposed: no, until it is requested.
