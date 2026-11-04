## ADDED Requirements

### Requirement: Protocol module in both builds
Both the Rust and the C build SHALL provide a protocol module with id `aveva-pi` that reaches an AVEVA
PI Web API server over HTTPS. Given the same configuration, both builds SHALL render the same
capability descriptor, the same samples (apart from read times) and the same command results.
The descriptor SHALL declare `subscribe: true`, modes `raw` and `typed`, and the command verbs
`write` and `write-batch`.

#### Scenario: Descriptor parity
- **WHEN** `just c-describe-parity` renders `demo/config/aveva-pi.toml` with both binaries
- **THEN** the two capability descriptors are equal

#### Scenario: Same suite, both builds
- **WHEN** `just test-e2e aveva-pi` and `just test-e2e-c aveva-pi` run
- **THEN** both pass the same Robot suite with no PI-specific `requires:` skips

### Requirement: Device and point addressing
A device's `protocol_address` SHALL require `base_url` (an `https://` or `http://` URL of the PI
Web API root) and accept `data_server`, `af_database` and `delivery`. A point's `address` SHALL
have exactly one of `tag`, `path` or `web_id`. A `tag` SHALL require the device's `data_server`.
A `path` that contains `|` SHALL address an AF attribute, and any other `path` a PI point. Any
other combination SHALL be rejected at load with an error naming the device or point.

#### Scenario: Tag without data server
- **WHEN** a point declares `address = { tag = "SINUSOID" }` on a device without `data_server`
- **THEN** loading fails with an error naming the point and `data_server`

#### Scenario: Two keys
- **WHEN** a point declares `address = { tag = "A", web_id = "F1DP..." }`
- **THEN** loading fails with an error naming the point

#### Scenario: AF attribute path
- **WHEN** a point declares `address = { path = '\\AF\Plant\Pump01|Pressure' }`
- **THEN** the connector resolves it with the attributes endpoint, not the points endpoint

### Requirement: WebId resolution is cached
The connector SHALL resolve every point's WebId once per connection, on connect, and SHALL NOT
look up a point again while the connection holds. A point given by `web_id` SHALL NOT be looked
up. A lookup answered with a 4xx status other than 401 and 403, or with no item, SHALL mean the
point cannot be resolved. A point that cannot be resolved SHALL produce a `bad` sample with `error` starting
`not found:` on each read. It SHALL be retried every `resolve_retry` (default 5 minutes), and it
SHALL NOT change the device's link state.

#### Scenario: No lookups per poll
- **WHEN** a snapshot device with 20 points is polled 10 times
- **THEN** the simulator records 20 resolution requests in total and 10 `streamsets/value` requests

#### Scenario: Unknown tag
- **WHEN** one of a device's points names a tag the server does not have
- **THEN** that point publishes `bad` samples with `error` starting `not found:`, the other points publish `good` samples, and the link is `connected`

### Requirement: TLS verification by default
The connector SHALL verify the server certificate and host name against the system trust store,
or against `tls.ca_file` when set. `tls.insecure_skip_verify = true` SHALL disable verification
and SHALL log a `warn` on every connect. A failed verification SHALL leave the link `degraded`
with a reason that names the TLS failure.

#### Scenario: Untrusted server certificate
- **WHEN** the simulator presents a certificate signed by a CA that is not in `tls.ca_file`
- **THEN** no request is sent and the link status reason names the certificate verification failure

#### Scenario: Configured CA
- **WHEN** `tls.ca_file` holds the simulator's CA
- **THEN** the link becomes `connected`

### Requirement: Authentication methods
`auth.method` SHALL accept `none`, `basic`, `bearer` and `certificate`. `basic` SHALL send
`username` and the content of `password_file`. `bearer` SHALL send the content of `token_file`
as `Authorization: Bearer`, and SHALL re-read the file once after a 401 before treating the
credentials as rejected. `certificate` SHALL present `cert_file` and `key_file` as a TLS client
certificate. A password or token given inline SHALL be rejected at load. Trailing newlines in
secret files SHALL be ignored. Any method other than `none` with an `http://` `base_url` SHALL be
rejected at load: credentials are never sent over plain HTTP. A device on `http://` SHALL be
logged at `warn` when it connects.

#### Scenario: Basic auth
- **WHEN** the simulator requires Basic auth and the configuration gives the right username and password file
- **THEN** reads succeed

#### Scenario: Rotated token
- **WHEN** the simulator starts rejecting the old token and `token_file` already holds a new valid token
- **THEN** the connector re-reads the file after the first 401 and the next request succeeds without a link outage

#### Scenario: Client certificate
- **WHEN** the simulator requires a client certificate and `auth.method = "certificate"` names a valid pair
- **THEN** reads succeed, and without the pair the TLS handshake fails

#### Scenario: Credentials over plain HTTP
- **WHEN** a device has `base_url = "http://…"` and `auth.method` is `basic`, `bearer` or `certificate`
- **THEN** loading fails with an error naming the method and `https://`; with `method = "none"` it loads

#### Scenario: Inline password
- **WHEN** the configuration sets `auth.password = "secret"`
- **THEN** loading fails with an error naming `auth.password` and `password_file`

### Requirement: Local-only settings
Management commands (`set-config`, `define-device`) SHALL NOT add, change or remove
`auth.password_file`, `auth.token_file`, `auth.cert_file`, `auth.key_file`, `tls.ca_file`,
`tls.insecure_skip_verify` or `state_dir`, at connection or device level, in either build.

#### Scenario: Remote attempt to skip verification
- **WHEN** a `set-config` command sets `connection.tls.insecure_skip_verify = true`
- **THEN** the command fails with a reason naming the key, and the configuration file is unchanged

### Requirement: Bounded requests and link status
Every HTTP request SHALL be bounded by `request_timeout` (default 10s) and its connection setup
by `connect_timeout` (default 5s). A timeout, a 5xx response, a TLS or DNS failure, or a closed
WebSocket SHALL mark the link `degraded` with a reason naming the HTTP status or error, and the
runtime SHALL reconnect with backoff. A 401 or 403 on the base URL SHALL mark the link
`disconnected` with reason `unauthorized`.

#### Scenario: Server stops answering
- **WHEN** the simulator accepts connections but stops answering
- **THEN** within `request_timeout` plus one poll interval the link is `degraded`, and the connector keeps publishing its health and other devices' samples

#### Scenario: Wrong password
- **WHEN** the password file holds a wrong password
- **THEN** the link is `disconnected` with reason `unauthorized`, and the connector retries with backoff, not in a tight loop

### Requirement: Request chunking
Requests that take WebIds in the query string SHALL carry at most `max_web_ids_per_request`
(default 50) WebIds and SHALL have a URL of at most `max_url_bytes` (default 8192), and a device
SHALL have at most `max_concurrent_requests` (default 4) in flight. On HTTP 414, or HTTP 400
without a PI `Errors` body, the connector SHALL halve that device's chunk size, down to 1, and log
once at `warn`.

#### Scenario: Large device
- **WHEN** a snapshot device has 120 points and the default chunk size
- **THEN** one poll sends 3 `streamsets/value` requests

#### Scenario: URL too long
- **WHEN** the simulator answers 414 to requests with more than 20 WebIds
- **THEN** the connector settles on chunks of at most 20 WebIds and every point publishes `good` samples

#### Scenario: Request too long for HTTP.sys
- **WHEN** the simulator answers 400 with an HTML body to requests longer than 4096 bytes
- **THEN** the connector halves its chunk size until the requests fit, and every point publishes `good` samples
