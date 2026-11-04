## ADDED Requirements

### Requirement: Write a value to a PI stream
The connector SHALL write a value to a PI stream for the `write` verb (and each item of
`write-batch`) on a point with `access` `write` or `read_write`: it SHALL post `{"Value": <value>}` to `streams/{webId}/value`, without a `Timestamp` so
the server time is used. It SHALL use `updateOption` and `bufferOption` from `[connection]`
(defaults `Replace` and `BufferIfPossible`) and SHALL send the header
`X-Requested-With: XMLHttpRequest`. A 202 or 204 answer SHALL end the command `successful`. The
value SHALL be encoded for the point's `datatype` after the
SDK has reversed any `map` and `transform`. A write SHALL never be retried.

#### Scenario: Numeric write
- **WHEN** a `write` command sets a `float64` point to `42.5`
- **THEN** the simulator archives `42.5` for that tag at server time, the command ends `successful`, and the next sample of the point reads `42.5`

#### Scenario: CSRF header
- **WHEN** the simulator enforces CSRF defence
- **THEN** writes succeed, because every write carries `X-Requested-With`

### Requirement: Digital writes
A write to a digital point SHALL accept a state name (string) or a state code (integer), and SHALL
send the matching state from the point's cached digital set as
`{"Value": {"Name": <name>, "Value": <code>, "IsSystem": false}}`. A state the point's digital set
does not contain SHALL fail the command with a reason naming the value and
the set, without sending a request.

#### Scenario: Write by name
- **WHEN** a `write` command sets a digital point with `datatype = "string"` to `"Stopped"`
- **THEN** the simulator receives `{"Value":{"Name":"Stopped","Value":0,"IsSystem":false}}`, archives the `Stopped` state, and the command ends `successful`

#### Scenario: Unknown state
- **WHEN** a `write` command sets a digital point to `"Exploded"`
- **THEN** the command ends `failed` with a reason naming `Exploded` and the digital set, and no request is sent

### Requirement: Write errors
An HTTP 4xx answer SHALL end the command `failed` with a reason containing the HTTP status and
PI's `Errors` text, and SHALL NOT change the link state. A 5xx answer, a timeout or a transport
error SHALL end the command `failed` with the status or error in the reason; the link state SHALL
follow the device's reads, as for every connector (a write never changes it). A write to an
unresolved point SHALL fail with a reason starting `not found:`.

#### Scenario: Read-only tag on the server
- **WHEN** the simulator answers 403 with `{"Errors":["write access denied"]}`
- **THEN** the command ends `failed` with a reason containing `403` and `write access denied`, and the link stays `connected`

#### Scenario: Server down during a write
- **WHEN** the simulator answers 503 to a write
- **THEN** the command ends `failed` with a reason containing `503`

### Requirement: Writes as device parameters
A writable PI point with `meta.parameter` SHALL work as a device parameter (contract §5.2) without
PI-specific code in the flows: a parameter change from the cloud SHALL arrive as `write-batch` and
be written as above.

#### Scenario: Cloud parameter change
- **WHEN** the cloud e2e suite changes a PI point's parameter value
- **THEN** the simulator archives the new value and the device twin shows it
