### OPC UA structured values in the default package (`tedge-dot-c`)

The C build now reads OPC UA structured values, as the Rust build does since the previous release.
You no longer need `tedge-dot-rs` to read one field of an ExtensionObject. See
[doc/connectors/opcua-connector-spec.md](../doc/connectors/opcua-connector-spec.md) §3.7.

- `address.field` selects a field of a structure (`Motor.Current`, `Items[1].Value`), decoded with
  the `DataTypeDefinition` the server publishes.
- `address.index` selects one element of an array variable.
- A `mode = "raw"` point on a structure publishes the encoded body, with `addr.encoding_id` and
  `addr.data_type`.
- DateTime, LocalizedText, StatusCode, Guid, NodeId, ExpandedNodeId and QualifiedName values are
  read as `string` (DateTime also as `int64`, StatusCode also as `uint32`).

Configurations that used `field` or `index` with the C build loaded before but gave bad samples.
They now give values, with no change to the configuration.

The C build keeps its fixed value sizes: a raw structure body longer than 256 bytes and a `bytes`
value longer than 127 bytes are bad samples that name the limit, and string values are cut at
255 bytes, as other C string values are.
