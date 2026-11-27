## Why

Writing an OPC UA connector file or point library today means copying node ids, data types and
access levels by hand from a separate OPC UA client. The OT connectors plugin edits and sends the
result, but it cannot show what a server offers. opcua-scan
([reubenmiller/opcua-discovery](https://github.com/reubenmiller/opcua-discovery)) already browses
large address spaces and writes tedge-dot point libraries, but only in a terminal on a machine that
reaches the server. Users want to scan a server from Cumulocity, keep the result, pick nodes from a
tree and get a file they can send (tedge-dot#149).

## What Changes

- A **`c8y_ScanAddressSpace`** operation on a gateway names one of its OPC UA devices and runs
  opcua-scan there with that device's connection settings from its connector file,
  so the scan connects like the connector does and no credential leaves the gateway. The operation
  takes opcua-scan's scope options (`root`, `depth`, `include`, `exclude`, `type`, `max_children`).
- The scan result is a **namespace file**: gzip-compressed JSON with the browse tree, the
  node attributes, the server's namespace table, and for each mappable node the tedge-dot point(s)
  opcua-scan proposes. The format is versioned and documented in tedge-dot; opcua-scan writes it
  with `--output tedge-dot-namespace`.
- The gateway uploads the namespace file to Cumulocity as an **inventory binary of type
  `tedge-dot-namespace`**, with metadata (gateway, device, connector file, endpoint, application
  URI, namespace table, scan time, scope, node count, truncation) and a link to the scanned device.
- The **OT connectors plugin** gets:
  - a **Namespaces** list in Management > OT connectors and on the gateway's OT connectors tab,
    with *Scan address space*, *Upload namespace file*, *Download* and *Delete*;
  - a **namespace picker**: the namespace file as a lazily rendered tree with search and filters, where
    users check nodes or subtrees and see and change each proposed point (id, name, unit,
    subscribe or poll, interval, access, measurement type and fragment), one at a time or several
    at once;
  - **output** into a new point library, an existing library (merge, with existing points checked
    and unchecked points removed) or a device's inline points in a connector file, always through
    the existing form, TOML editor and validation before saving.
- opcua-scan (external repository) gets the namespace file output format; tedge-dot documents how to
  install it on a gateway. tedge-dot itself gets no browse code (the scan stays in opcua-scan).

## Capabilities

### New Capabilities

- `namespace-files`: the namespace file format, how a namespace file is stored in Cumulocity, and what
  its metadata holds.
- `namespace-scan`: the `c8y_ScanAddressSpace` operation, how the gateway runs opcua-scan with
  a device's connector settings, and how the namespace file is uploaded and the operation completed.

### Modified Capabilities

- `device-management-ui`: the OT connectors plugin lists namespace files, starts scans,
  uploads and downloads namespace files, and builds point libraries and connector-file points
  from one.

## Impact

- **ui/**: new components for the Namespaces list, the scan dialog and the namespace picker; new
  logic under `ui/src/shared/` for namespace file parsing, tree filtering, selection and point
  generation, with unit tests; the plugin manifest describes the new feature.
- **operations/**: a `c8y_ScanAddressSpace` command template and the `ot-scan-address-space`
  script (find the device's connector file, run opcua-scan, gzip, upload through the mapper's
  Cumulocity proxy, complete the operation).
- **doc/**: namespace file format reference, scan guide, update of `doc/c8y-connector-files.md`;
  site pages and the docs map.
- **packaging**: the scan script and template in the OPC UA packages; opcua-scan is a recommended,
  not required, dependency.
- **External**: opcua-scan `--output tedge-dot-namespace` (fork `mstoffel-sag/opcua-discovery`,
  then upstream). Until it is released, the operation fails with a clear message.
- No change to the connectors, the contract's point model or the SDKs.
