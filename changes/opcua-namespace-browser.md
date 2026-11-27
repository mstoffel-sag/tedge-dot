### OPC UA points from an address-space scan

The OT connectors plugin builds OPC UA point libraries and device points from a scan of the
server, instead of node ids copied from another client:

- **Scan address space** asks a gateway to scan one of its OPC UA devices with the new
  `c8y_ScanAddressSpace` operation. The gateway runs
  [opcua-scan](https://github.com/reubenmiller/opcua-discovery) with the device's settings from
  its connector file, and stores the result in Cumulocity as a *namespace file*. Install
  opcua-scan 0.2.0 or later on gateways that should scan; tedge-dot does not bring it.
- **Create from namespace** shows the scan as a tree. Check nodes or whole branches, change the
  proposed points (id, name, unit, subscribe or poll, access, measurement type and fragment), and
  write them into a new library, an existing library or a device of a connector file. Existing
  points, settings and comments stay, and node ids follow the target's namespace table.
- A scan made with opcua-scan elsewhere can be uploaded to the same list.
- Methods are picked one by one, not with their object, and take only an id and a name. The
  editor, the docs site's config builder and the contract schema now refuse value settings on a
  `call_method` point, and OPC UA method address keys on other points, as the connectors do.

See [Namespace files](https://github.com/thin-edge/tedge-dot/blob/main/doc/namespace-files.md)
and [Points from a namespace scan](https://github.com/thin-edge/tedge-dot/blob/main/doc/c8y-connector-files.md#points-from-a-namespace-scan).
