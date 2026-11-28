## ADDED Requirements

### Requirement: Namespaces list
The OT connectors page SHALL list, in a third list, the `tedge-dot-namespace` binaries with their
device, protocol, server endpoint, gateway, node count, whether the scan was truncated, and scan
time.
It SHALL offer *Create from namespace*, *Download* and *Delete*. The gateway's OT connectors tab
SHALL list the namespace files of its scans the same way.

#### Scenario: Page with namespace files
- **WHEN** Cumulocity holds a namespace file of `plc-7` with 1200 nodes
- **THEN** the Namespaces list shows a row with `plc-7`, the protocol OPC UA, its endpoint, `1200` and the scan time

#### Scenario: Download
- **WHEN** a user downloads a namespace file
- **THEN** the browser saves the binary unchanged, as a `.json.gz` file

### Requirement: Scan from the plugin
The Namespaces list and the gateway's tab SHALL offer *Scan address space* for gateways that
announce `c8y_ScanAddressSpace`. The dialog SHALL ask for the gateway, one of its OPC UA child
devices, and the scope (root, depth, include and exclude patterns, node classes, child limit), send
the operation, show its progress, and show the new namespace file in the list when the operation
succeeds, or show its failure reason.

#### Scenario: Successful scan
- **WHEN** a user scans `plc-7` with the include pattern `Objects/Factory/**`
- **THEN** a `c8y_ScanAddressSpace` operation with `device = "plc-7"` and that pattern goes to `plc-7`'s gateway, and when it succeeds the new namespace file appears in the list

#### Scenario: Gateway without the operation
- **WHEN** a gateway does not announce `c8y_ScanAddressSpace`
- **THEN** its tab does not offer *Scan address space*, and the dialog does not list it

### Requirement: Scanning a device that has a namespace file
When the user picks a device that already has a namespace file from that gateway, the scan dialog
SHALL say so, with the file's scan time and size, say that a successful scan replaces it and a
failed one keeps it, offer to open the existing file instead, and label the button *Scan again*.

#### Scenario: Device already scanned
- **WHEN** a user picks `plc-7` on `gw-1`, which has a namespace file of `plc-7` from 2026-11-27
- **THEN** the dialog says that `plc-7` already has a namespace file from that gateway, scanned 2026-11-27, that scanning again replaces it once the new scan succeeds, offers to open it instead, and labels the button *Scan again*

### Requirement: Upload a namespace file
The Namespaces list SHALL accept a namespace file made elsewhere, compressed or not, check its
`format` and `version`, and store it as a `tedge-dot-namespace` binary with the metadata read from
the file. A file that is not a namespace file, or has a version the plugin does not know, SHALL be
refused with its reason.

#### Scenario: File from a laptop
- **WHEN** a user uploads `line1.json.gz` written by `opcua-scan browse --output tedge-dot-namespace`
- **THEN** the list shows it with the endpoint and node count from the file

#### Scenario: Plain export
- **WHEN** a user uploads opcua-scan's plain `--output json` export
- **THEN** the plugin refuses it and says it needs `--output tedge-dot-namespace`

### Requirement: Uploads replace their predecessor
An uploaded namespace file SHALL replace earlier uploaded files of the same device and server
endpoint, so the list keeps one per server. The dialog SHALL say that it replaced a file. Files
that gateway scans produced SHALL NOT be replaced by an upload.

#### Scenario: Upload again
- **WHEN** a user uploads a new scan of `plc-7` at `opc.tcp://plc-7:4840` and an uploaded file of that device and endpoint exists
- **THEN** the list shows only the new file, and the dialog says the earlier one was replaced

### Requirement: Namespace tree
*Create from namespace* SHALL show the file's nodes as a tree that opens at `Objects`, expands on
demand and stays responsive with 200 000 nodes. It SHALL search by name and browse path, filter by
node class, data type and access, show a node's attributes, and mark truncated branches and nodes
that have no proposed point, with the reason.

#### Scenario: Search
- **WHEN** a user searches for `temp` in a file with 200 000 nodes
- **THEN** the tree shows the matching nodes with their ancestors expanded, and the page stays responsive

#### Scenario: Node without points
- **WHEN** a user selects an array variable
- **THEN** the tree shows that it has no proposed point and why, and it cannot be checked

### Requirement: Selecting nodes
Users SHALL check a node to take its proposed points, or check an object to take the proposed
points of the variables below it. A method SHALL be taken only when it is checked itself, or when
an object has nothing but methods below it. The picker SHALL show the number of checked points at
all times.

#### Scenario: Check a subtree
- **WHEN** a user checks the object `Line1`, which has 12 variables with proposed points below it
- **THEN** 12 points are checked

#### Scenario: Methods below an object
- **WHEN** a user checks the object `Pump01`, which has 20 variables and 3 methods with proposed points below it
- **THEN** the 20 variable points are checked and the 3 methods are not

#### Scenario: Method checked itself
- **WHEN** a user checks the method `resetFilter`
- **THEN** its `call_method` point is checked

### Requirement: Configuring points
For each checked point the picker SHALL show and let users change its `id`, `name`, `unit`,
`access`, subscribe or poll, the sampling or poll interval, and the measurement type and fragment.
A change SHALL apply to one point or to all selected points; a method point SHALL take only its
`id` and `name`. Point ids SHALL be unique in the result; the picker SHALL report a clash at the
points that clash.

#### Scenario: Change several points
- **WHEN** a user selects 12 checked points and sets poll with a 5 s interval
- **THEN** all 12 points get `subscribe = false` and `poll_interval = "5s"`

#### Scenario: Method among the selected points
- **WHEN** a user selects 12 variable points and a method point and sets poll with a 5 s interval, access `read` and the unit `A`
- **THEN** the 12 variable points take the change, the method point keeps its proposed settings, and the picker says that it was left alone

#### Scenario: Id clash
- **WHEN** two checked points end up with the id `temperature`
- **THEN** the picker marks both and does not create the result until one is renamed

### Requirement: Result of the picker
The picker SHALL write the checked points into a new point library, into an existing library, or
as the inline points of a device in a connector file, with the namespace table from the namespace
file. The result SHALL open in the editor and pass the usual check before it is saved.

#### Scenario: New library
- **WHEN** a user checks 12 points and chooses a new library `acme-line`
- **THEN** the editor opens a new OPC UA library `acme-line` with the 12 points and `[library] namespaces`, unsaved

#### Scenario: Device points
- **WHEN** a user picks the device `plc-7` of the connector file `opcua` as target
- **THEN** the editor opens `opcua` with the checked points added to `plc-7`'s points, unsaved

### Requirement: Editing an existing target
When the target already has points, the picker SHALL check the nodes they address. Unchecking such
a node SHALL remove its point, and checking a new node SHALL add its point. Points whose node is
not in the namespace file, and the target's comments and other settings, SHALL be kept.

#### Scenario: Merge into a library
- **WHEN** a user opens `acme-line` (12 points) as target, unchecks 2 and checks 3 new nodes
- **THEN** the editor shows `acme-line` with 13 points, and its comments are still there

#### Scenario: Point outside the scan
- **WHEN** the target has a point whose node is not in the namespace file
- **THEN** the point stays in the result and the picker lists it as not in this scan

### Requirement: Method points in the editor check
The editor SHALL report, at the field concerned, a value setting on a `call_method` point (the
fields the contract excludes, such as `datatype`, `poll_interval`, `subscribe` or
`meta.parameter`) and, for OPC UA, a method address key on another point or a variable address
key on a method point, with the connectors' messages. Such a file SHALL NOT be saved.

#### Scenario: Interval on a method
- **WHEN** a method point declares `poll_interval = "5s"`
- **THEN** the editor shows "does not apply to a call_method point" at that point's `poll_interval`, and Save stays disabled

#### Scenario: Method address on a read point
- **WHEN** a point with `access = "read"` has `address = { object_id = "...", method_id = "..." }`
- **THEN** the editor shows "applies only to a call_method point" at its `object_id`

### Requirement: Points from the editor
The editor SHALL offer *Add points from namespace* on each device of an OPC UA connector file and
on an OPC UA point library. It SHALL open the picker with the file's current text, unsaved edits
included, as the target, and the device's scan chosen: one of the same connector file first, then
the newest. Users SHALL be able to choose another scan, or scan or upload one when none fits.

#### Scenario: Device with a scan
- **WHEN** a user edits `opcua-demo.toml`, has an unsaved change, and selects *Add points from namespace* on device `Pump01`, which has a scan
- **THEN** the picker opens Pump01's scan with `opcua-demo.toml` and `Pump01` as the target

#### Scenario: Device without a scan
- **WHEN** a user selects *Add points from namespace* on a device that has no scan
- **THEN** the picker says so and offers to scan the device or upload a namespace file

### Requirement: Back to the editor
From the editor's context, *Open in editor* SHALL return to the editor with the points written
into its text, unsaved, with a note; *Back to the editor* SHALL return with the editor's text as it
was, unsaved edits included. A new file SHALL come back with its name, description, file name and
device type.

#### Scenario: Unsaved edit survives
- **WHEN** a user changes a setting in the editor, adds two points from a namespace and opens the result in the editor
- **THEN** the editor shows the changed setting and the two new points, unsaved

### Requirement: Target from the scan
When the picker opens a scan from the Namespaces list or after a scan, it SHALL preselect as the
target the repository connector entry whose file name is the scan's connector file and the scanned
device, when that entry defines the device. Otherwise the target SHALL start as a new point
library.

#### Scenario: Matching entry
- **WHEN** a user opens the scan of `Pump01` from `opcua-demo.toml` and the repository has the entry `tedge-dot-opcua-demo` with device `Pump01`
- **THEN** the picker starts with *Device in a connector file*, `opcua-demo.toml` and `Pump01`

#### Scenario: No matching entry
- **WHEN** the repository has no entry for the scan's connector file
- **THEN** the picker starts with *New point library*

### Requirement: Same protocol only
The picker SHALL offer as targets only point libraries and connector files of the scan's protocol,
and SHALL write a new library with that protocol. The editor's *Add points from namespace* SHALL
offer only scans of the file's protocol. A namespace file without a protocol in its metadata
SHALL count as OPC UA.

#### Scenario: Libraries of another protocol
- **WHEN** the repository holds the OPC UA library `acme-line` and the Modbus library `acme-meter`, and a user opens an OPC UA scan
- **THEN** *Existing point library* offers `acme-line` and not `acme-meter`

#### Scenario: Scans of another protocol
- **WHEN** a device `plc-7` has an OPC UA scan and a CANopen scan, and a user selects *Add points from namespace* on `plc-7` in an OPC UA connector file
- **THEN** the picker offers only the OPC UA scan
