# Namespace files: building points from an OPC UA scan

A **namespace file** is a stored scan of an OPC UA server's address space. It holds the browse tree,
each node's attributes, the server's namespace table, and for every node that can be read or
called the tedge-dot point(s) [opcua-scan](https://github.com/reubenmiller/opcua-discovery)
proposes for it. Cumulocity keeps namespace files as binaries; the OT connectors plugin opens one
as a tree, where you check nodes and get a point library or a device's points
([Managing connector files from Cumulocity](c8y-connector-files.md#points-from-a-namespace-scan)).

```text
 Cumulocity                         gateway                                    OPC UA server
 c8y_ScanAddressSpace ─▶ c8y mapper ─▶ ot-scan-address-space ─▶ opcua-scan ─▶ browse
       ▲                                     │ (device's settings from its connector file)
       └─ binaryId, nodes ◀── tedge-dot-namespace binary (gzip JSON) ◀─┘
```

## Install opcua-scan

The scan runs `opcua-scan` 0.2.0 or later on the gateway, as the c8y mapper's user (`tedge`).
tedge-dot does not install it. Until opcua-scan publishes packages, build it with a Rust toolchain
(1.88 or later) for the gateway's architecture and copy the binary to `/usr/local/bin`:

```sh
cargo install --locked --git https://github.com/reubenmiller/opcua-discovery opcua-scan
opcua-scan browse --help | grep tedge-dot-namespace   # the version can write namespace files
```

Another location works too: set `OPCUA_SCAN` for the mapper (see [Settings](#settings)).

The gateway also needs `jq`, `curl`, `gzip` and `timeout`, which most distributions have.

## Scan from Cumulocity

The tedge-dot packages install the operation template
`/etc/tedge/operations/c8y/c8y_ScanAddressSpace` and the script `/usr/bin/ot-scan-address-space`,
so every gateway with tedge-dot lists `c8y_ScanAddressSpace` in its supported operations.

In the OT connectors plugin, select **Scan address space** in the Namespaces list or on the
gateway's OT connectors tab, pick the OPC UA device and, optionally, a scope. The operation goes to
the **gateway** and names the device:

```json
{
  "c8y_ScanAddressSpace": {
    "device": "plc-7",
    "root": "i=85",
    "depth": "unlimited",
    "include": ["Objects/Factory/Line1/**"],
    "exclude": ["**/Diagnostics/**"],
    "type": "Variable,Method",
    "max_children": 5000
  }
}
```

| Field | Default | Meaning |
| --- | --- | --- |
| `device` | required | A `[[device]]` of an OPC UA connector file in `/etc/tedge/plugins/ot`. |
| `root` | `i=85` (Objects) | The node to start at. |
| `depth` | `unlimited` | Levels below `root`. |
| `include`, `exclude` | none | Browse-path globs: `*` within a name, `**` across levels. |
| `type` | `Variable,Method` | Node classes to keep; the objects above them are always kept. |
| `max_children` | none | Children read per node; a node with more is marked truncated. |

The script finds the connector file that defines the device, and runs
`opcua-scan browse --tedge-dot-config <file> --device <device> --output tedge-dot-namespace`.
The scan connects with the device's endpoint, security policy and mode, user and password file,
and the connector's PKI directory (`[connection] pki_dir`), so it trusts the servers the connector
trusts. The operation cannot carry an endpoint or credentials; fields like that are ignored.

On success the script uploads the compressed file as a binary of type `tedge-dot-namespace`,
stores its metadata, adds it as a child addition of the device's managed object (or of the
gateway, when the device is not registered), and writes `binaryId` and `nodes` into the
operation's fragment.

Each device keeps **one namespace file**: after a successful scan the script deletes the earlier
files of the same gateway and device. A scan that fails leaves the earlier file in place, and
files of other devices and uploaded files are not touched.

### Settings

The script reads these from its environment, which a systemd drop-in for `tedge-mapper-c8y` sets:

| Variable | Default | |
| --- | --- | --- |
| `OPCUA_SCAN` | `opcua-scan` on `PATH` | The opcua-scan executable. |
| `TEDGE_DOT_SCAN_TIMEOUT` | `540` | Seconds the scan may run. The template allows the operation 600. |
| `TEDGE_DOT_SCAN_MAX_BYTES` | `52428800` (50 MB) | Largest compressed file the gateway uploads. |
| `OT_CONFIG_DIR` | `/etc/tedge/plugins/ot` | Where the connector files are. |

### When a scan fails

The operation fails with one line that says why, and nothing is uploaded:

- opcua-scan is missing, or too old to write namespace files;
- the operation names no device, or no OPC UA connector file (or more than one) defines it;
- the server does not answer, refuses the session, or its certificate is not trusted (the last
  line of opcua-scan's output is in the reason);
- the scan finds no node, takes longer than `TEDGE_DOT_SCAN_TIMEOUT`, or its file is larger than
  `TEDGE_DOT_SCAN_MAX_BYTES`: narrow it with `include` or `depth`;
- the upload to Cumulocity fails.

The scan opens a second session next to the connector's. A server that allows only one session per
client refuses it; stop the connector for the scan, or raise the server's limit.

## Scan from a laptop

opcua-scan writes the same file anywhere it can reach the server, with its own connection options
or with a copy of the connector file:

```sh
opcua-scan browse --tedge-dot-config opcua.toml --device plc-7 --depth unlimited \
    --output tedge-dot-namespace --file plc-7.json
```

**Upload namespace file** in the Namespaces list stores it, compressed or not, after checking it.
It replaces an earlier upload of the same device and endpoint; files from gateway scans stay.
opcua-scan's plain `--output json` export is not a namespace file and is refused.

## The format

A namespace file is UTF-8 JSON, stored gzip-compressed. Its schema is
[`namespace-file.schema.json`](contract/schemas/namespace-file.schema.json); the
[test vectors](contract/test-vectors/namespace-file/) show complete files.

```jsonc
{
  "format": "tedge-dot-namespace",          // readers refuse anything else
  "version": 1,                             // readers refuse versions they do not know
  "protocol": "opcua",
  "server": { "endpoint": "opc.tcp://plc-7:4840", "application_uri": "urn:acme:plc-7" },
  "namespaces": ["http://opcfoundation.org/UA/", "urn:acme:plc-7", "urn:acme:line"],
  "scan": { "time": "2026-11-27T10:00:00Z", "tool": "opcua-scan", "tool_version": "0.2.0",
            "root": "i=85", "depth": "unlimited", "types": ["Variable", "Method"],
            "device": "plc-7", "connector_file": "opcua" },
  "nodes": [
    { "id": "i=85", "browse_name": "Objects", "display_name": "Objects", "node_class": "Object" },
    { "id": "nsu=urn:acme:line;s=Line1", "parent": "i=85", "browse_name": "Line1",
      "display_name": "Line1", "node_class": "Object" },
    { "id": "nsu=urn:acme:line;s=Line1.Temperature", "parent": "nsu=urn:acme:line;s=Line1",
      "browse_name": "Temperature", "display_name": "Temperature", "node_class": "Variable",
      "datatype": "Double", "access_level": 3, "user_access_level": 3, "unit": "°C",
      "subscribable": true,
      "points": [{ "id": "line1_temperature", "name": "Temperature", "datatype": "float64",
                   "access": "read_write", "address": { "node_id": "ns=2;s=Line1.Temperature" } }] }
  ]
}
```

- **Node ids** carry their namespace URI (`nsu=<uri>;...`), so the file does not depend on the
  server's index order; namespace 0 ids stay plain (`i=85`).
- **Nodes** are listed parents first; every node but the scan's root names its `parent`. A node
  id appears once.
- **`points`** are point tables as opcua-scan's point-library output writes them: one for a
  scalar variable or a method, one per mappable leaf field for a structure. Their ids use
  `ns=<index>` of the file's `namespaces` table. A variable or method without points has a
  `skipped` reason instead (arrays, unsupported types, properties, no access).
- **`truncated`** (`depth`, `max_children`, `error`) marks a node whose children the scan did not
  all read, so an empty branch can be told from one that was not browsed.
- **No secrets**: no user names, passwords, keys or certificates. Current values appear only when
  the scan was asked to preview them (`--preview-values`).

### In Cumulocity

A namespace file is an inventory binary (`type = "tedge-dot-namespace"`, content type
`application/gzip`). Its managed object carries the metadata the Namespaces list shows, so listing
never downloads a file:

```json
"tedgeDotNamespace": {
  "formatVersion": 1, "protocol": "opcua",
  "gateway": "gw-1", "gatewayId": "12345", "device": "plc-7", "connectorFile": "opcua",
  "endpoint": "opc.tcp://plc-7:4840", "applicationUri": "urn:acme:plc-7",
  "namespaces": ["http://opcfoundation.org/UA/", "urn:acme:plc-7", "urn:acme:line"],
  "scannedAt": "2026-11-27T10:00:00Z", "tool": "opcua-scan", "toolVersion": "0.2.0",
  "scope": { "root": "i=85", "depth": "unlimited", "include": [], "exclude": [], "types": ["Variable", "Method"] },
  "nodes": 3, "points": 1, "truncated": false
}
```

`gateway` is the gateway's device id, `gatewayId` its managed object id. Query them with
`type eq 'tedge-dot-namespace'`.
