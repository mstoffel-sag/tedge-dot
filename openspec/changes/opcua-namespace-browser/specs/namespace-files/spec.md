## Purpose

Defines the namespace file: a stored scan of an OPC UA server's address space with the tedge-dot
points proposed for its nodes, and how it is kept in Cumulocity so the OT connectors plugin can
build connector files and point libraries from it.

## ADDED Requirements

### Requirement: Namespace file format
A namespace file SHALL be gzip-compressed UTF-8 JSON with a `format` of `tedge-dot-namespace`, a
`version` (starting at 1), a `server` object (endpoint URL, application URI, product name), the
server's `namespaces` table as an array of URIs in index order, a `scan` object (time, root,
depth, include and exclude patterns, node classes, tool name and version) and a `nodes` array.

#### Scenario: Minimal file
- **WHEN** opcua-scan scans a server with one variable under `Objects`
- **THEN** the decompressed file has `format = "tedge-dot-namespace"`, `version = 1`, the server's namespace table, and the `Objects` node and the variable in `nodes`

#### Scenario: Unknown version
- **WHEN** a reader that knows version 1 opens a file with `version = 2`
- **THEN** it refuses the file and names the version it found

### Requirement: Node entries
Each node SHALL carry its node id with a namespace URI (`nsu=`), its parent's node id (none for the
root), browse name, display name, node class, and, where they apply, description, data type,
value rank, access level, user access level, historizing, engineering unit and whether it looks
subscribable. Nodes SHALL be listed parents first.

#### Scenario: Variable entry
- **WHEN** the scan finds the Double variable `Temperature` with unit `°C` under `Line1`
- **THEN** its entry names `Line1`'s node id as parent, the data type `Double`, the unit `°C`, and its access levels

#### Scenario: Index-independent ids
- **WHEN** the server numbers the namespace `urn:acme:line` as index 3
- **THEN** node ids in that namespace are written as `nsu=urn:acme:line;...`, not `ns=3;...`

### Requirement: Proposed points
A node that opcua-scan can map to tedge-dot points SHALL carry them in `points`, each a point
table as the point-library output writes it. A node it cannot map SHALL carry no `points` and a
`skipped` reason instead. A structure variable SHALL propose one point per mappable leaf field.

#### Scenario: Scalar variable
- **WHEN** the scan finds a readable, writable, subscribable `Double` variable
- **THEN** its entry proposes one point with `datatype = "float64"`, `access = "read_write"` and its `node_id` address

#### Scenario: Structure variable
- **WHEN** the scan finds a variable of a structure type with the fields `Current` and `Voltage`
- **THEN** its entry proposes two read-only points addressed with `field = "Current"` and `field = "Voltage"`

#### Scenario: Array variable
- **WHEN** the scan finds a variable with value rank 1
- **THEN** its entry has no `points` and its `skipped` reason names arrays

### Requirement: Truncated branches
When the scan stops below a node because of the depth limit, the per-branch child limit or a
browse error, that node SHALL carry `truncated` with the reason, so a reader can tell an empty
branch from one that was not browsed.

#### Scenario: Child limit
- **WHEN** a folder has 8000 children and the scan's child limit is 5000
- **THEN** the folder's entry has `truncated = "max_children"` and the file holds 5000 of its children

### Requirement: No secrets
A namespace file SHALL NOT contain user names, passwords, private keys or certificates, and SHALL
NOT contain current values unless the scan was asked to preview values.

#### Scenario: Password-protected server
- **WHEN** the scan connects with a user name and a password file
- **THEN** neither the user name nor the password appears in the namespace file or its metadata

### Requirement: Stored in Cumulocity
A namespace file SHALL be stored as a Cumulocity inventory binary of type `tedge-dot-namespace`
with the content type `application/gzip`. The binary's managed object SHALL record the gateway,
the device, the connector file, the endpoint, the application URI, the namespace table, the scan
time, the scan scope, the node count and whether any branch was truncated.

#### Scenario: Scan from a gateway
- **WHEN** a scan of device `plc-7` from connector file `opcua` on gateway `gw-1` finishes
- **THEN** Cumulocity holds a `tedge-dot-namespace` binary whose managed object names `gw-1`, `plc-7`, `opcua`, the endpoint and the node count

#### Scenario: Listing
- **WHEN** a client queries the inventory for `type eq 'tedge-dot-namespace'`
- **THEN** it gets every namespace file with its metadata, without downloading any binary
