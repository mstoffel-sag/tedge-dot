## Purpose

Defines how a gateway scans the address space of an OPC UA device on request from Cumulocity,
with the device's own connection settings, and stores the result as a namespace file.

## ADDED Requirements

### Requirement: Scan operation
The gateway SHALL accept a `c8y_ScanAddressSpace` operation that names an OPC UA device of its
connector files in `device`. The other parameters SHALL be optional: `root` (default `i=85`),
`depth` (number or `unlimited`, default `unlimited`), `include` and `exclude` (lists of
browse-path globs), `type` (node classes, default `Variable,Method`, with the objects above them)
and `max_children`.

#### Scenario: Scan with a scope
- **WHEN** Cumulocity sends `{"c8y_ScanAddressSpace": {"device": "plc-7", "include": ["Objects/Factory/Line1/**"]}}` to gateway `gw-1`
- **THEN** `gw-1` scans only the branches of `plc-7`'s server that match the pattern

#### Scenario: Scan without a scope
- **WHEN** Cumulocity sends `{"c8y_ScanAddressSpace": {"device": "plc-7"}}` to `gw-1`
- **THEN** `gw-1` scans `plc-7`'s server from `Objects` with unlimited depth

#### Scenario: No device
- **WHEN** the operation has no `device`
- **THEN** the operation FAILS and says that `device` is required

### Requirement: Announced on gateways
A gateway with tedge-dot's OPC UA connector package installed SHALL list `c8y_ScanAddressSpace`
in its supported operations.

#### Scenario: Gateway with OPC UA
- **WHEN** tedge-dot's OPC UA package is installed on gateway `gw-1`
- **THEN** `gw-1` lists `c8y_ScanAddressSpace` in its supported operations

### Requirement: Device connection settings
The scan SHALL connect with the settings of the device in the connector file that defines it: the
endpoint, security policy and mode, user name and password file, and the gateway's application
certificate. The operation SHALL NOT accept connection settings or credentials.

#### Scenario: Secured server
- **WHEN** `plc-7` connects with `Basic256Sha256`, `SignAndEncrypt` and a user with a password file
- **THEN** the scan connects with the same policy, mode, user and password file

#### Scenario: Connection fields in the operation
- **WHEN** the operation carries an `endpoint` or `password` field
- **THEN** the gateway ignores it and connects with the connector file's settings

### Requirement: Upload and result
After a successful scan the gateway SHALL upload the namespace file to Cumulocity as a
`tedge-dot-namespace` binary and SHALL complete the operation with the binary's id and the node
count. The binary SHALL be a child addition of the scanned device's managed object when the
gateway has registered that device, else of the gateway.

#### Scenario: Successful scan
- **WHEN** the scan of `plc-7` finds 1200 nodes
- **THEN** the operation is SUCCESSFUL, its fragment holds the new binary's id and `nodes = 1200`, and `plc-7`'s managed object lists the binary as a child addition

### Requirement: One file per device
After a successful scan and upload, the gateway SHALL delete the earlier namespace files of the
same gateway and device, so each device has one current namespace file. A scan that fails SHALL
leave the earlier file in place. Files of other devices and uploaded files SHALL NOT be touched.

#### Scenario: Scan again
- **WHEN** `plc-7` on gateway `gw-1` has a namespace file and a new scan of `plc-7` succeeds
- **THEN** Cumulocity holds only the new namespace file of `plc-7` on `gw-1`

#### Scenario: Scan fails
- **WHEN** `plc-7` has a namespace file and a new scan of `plc-7` fails because the server does not answer
- **THEN** the earlier namespace file of `plc-7` is still there

#### Scenario: Other device
- **WHEN** a scan of `plc-7` succeeds on a gateway that also has a namespace file of `plc-8`
- **THEN** the namespace file of `plc-8` is still there

### Requirement: Failures
The operation SHALL fail with a reason a user can act on when opcua-scan is not installed or too
old, the device is not in any connector file, the server cannot be reached or refuses the session,
the scan finds no node, or the upload fails. A failed scan SHALL NOT upload a partial file.

#### Scenario: opcua-scan missing
- **WHEN** the gateway has no opcua-scan
- **THEN** the operation FAILS with a reason that says opcua-scan is not installed and links the install guide

#### Scenario: Server down
- **WHEN** `plc-7`'s endpoint does not answer
- **THEN** the operation FAILS with the endpoint and the connection error, and no binary is created

### Requirement: Scan limits
A scan SHALL stop after a time limit (default 10 minutes) and SHALL refuse to upload a file above
a size limit (default 50 MB compressed). Both limits SHALL be configurable on the gateway. A scan
that hits either limit SHALL fail and name the limit.

#### Scenario: Too large
- **WHEN** the compressed namespace file is 80 MB and the limit is 50 MB
- **THEN** the operation FAILS, names the limit and suggests narrowing the scan with `include` or `depth`
