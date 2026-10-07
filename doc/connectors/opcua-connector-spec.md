# `opcua` Connector Spec — sessions, security and certificates

| Field | Value |
| --- | --- |
| Status | Implementable |
| Protocol id | `opcua` |
| Crate | `connector-opcua` (feature `opcua`) · C module `impl/c/connectors/opcua/` (option `TDOT_OPCUA`) |
| Builds on | Rust: [`async-opcua`](https://github.com/FreeOpcUa/async-opcua) (MPL-2.0), with `async-opcua-crypto` vendored and patched (`impl/rust/vendor/async-opcua-crypto/TEDGE-DOT-PATCH.md`). C: [open62541](https://github.com/open62541/open62541) 1.5 (MPL-2.0) with [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6 (Apache-2.0), both built from source and linked statically. [Connector SDK](../sdk/connector-sdk.md) |
| Implements | [OT Connector Contract](../contract/ot-connector-contract.md) v0.1.0 |
| Test vectors | [connectors/opcua/conformance/tools/genpki.py](../../connectors/opcua/conformance/tools/genpki.py) (PKI scenarios, generated at test time) |

This spec is normative for both implementations. Where it says "the connector", the Rust and
the C build behave identically; the tests named in §10 hold them to it.

## 1. Scope

| Operation | Services | Contract |
| --- | --- | --- |
| Read | Read (`Value` attribute) | polled `read_points` |
| Monitor | CreateSubscription + CreateMonitoredItems (one subscription per device) | `subscribe` |
| Write | Write (`Value` attribute) | `write` (and SDK `write-batch`) |
| Discovery | GetEndpoints (secured devices and non-anonymous identities) | connect |

Security: the `None`, `Basic256Sha256`, `Aes128_Sha256_RsaOaep` and `Aes256_Sha256_RsaPss`
policies (and, when explicitly allowed, the deprecated `Basic128Rsa15` and `Basic256`), modes
`none`, `sign`, `sign_and_encrypt`; anonymous, username and X.509 user identities; an
application instance certificate; server certificates trusted by pinning or through a CA chain
with CRLs.

Out of scope: GDS (push/pull certificate management), ECC policies, reverse connect,
cloud-side certificate operations, keys in an HSM/TPM.

## 2. Capability descriptor

```json
{ "protocol": "opcua", "modes": ["raw", "typed"],
  "datatypes": ["bool","int8","uint8","int16","uint16","int32","uint32","int64","uint64","float32","float64","string","bytes"],
  "point_kinds": ["variable"], "command_verbs": ["write", "write-batch"],
  "features": ["polling", "subscribe"], "subscribe": true }
```

`bytes` (ByteString values, §3.7) is advertised by both builds. Security is configuration, not a capability: both builds support all of §3.

## 3. Configuration

Relative paths anywhere in this section (`pki_dir`, `certificate`, `private_key`,
`password_file`, `user_certificate`, `user_private_key`) resolve against the directory of the
configuration file — never against the process working directory.

**Local-only settings** (contract §3.4): a management command (`set-config`, `define-device`)
may not add or change `pki_dir`, `certificate`, `private_key`, `create_certificate`,
`password_file`, `user_certificate`, `user_private_key`, `trust_any_server_certificate`,
`allow_plaintext_password` or `allow_deprecated_security`; such a command fails with
`<place>.<key> may only be set in the configuration file, not by a management command`.
Removing one of them is refused too (removing a whole device is not). An inline `password` is
allowed.

### 3.1 `connection`

| Key | Default | Meaning |
| --- | --- | --- |
| `application_name` | `"tedge-dot"` | The client's application name (and the CN/O of a generated certificate). |
| `application_uri` | `"urn:tedge-dot"` | The client's application URI. It must be a URI SAN of the application certificate. |
| `connect_timeout_s` | `15` | Bound on discovery and session activation. |
| `request_timeout_s` | `connector.operation_timeout` | Bound on a service call. |
| `security_policy` | `"None"` | Default policy of every device (§3.3). |
| `security_mode` | policy default | Default mode of every device (§3.3). |
| `pki_dir` | `/var/lib/tedge-dot/opcua/pki` | The PKI directory (§4). |
| `certificate`, `private_key` | — | The application certificate (DER or PEM) and key (PEM). Both or neither; when set they are used instead of the PKI directory's `own/` entries and never generated. |
| `create_certificate` | `true` | Generate a certificate when none exists (§4.2). |
| `trust_any_server_certificate` | `false` | Accept any server certificate (§5.4). Devices may override. |
| `allow_deprecated_security` | `false` | Allow `Basic128Rsa15` and `Basic256`. Devices may override. |
| `allow_plaintext_password` | `false` | Allow a password to travel unencrypted (§6.2). Devices may override. |

A value of the wrong type is a configuration error naming `[connection]` and the key.

### 3.2 `device.protocol_address`

| Key | Required | Meaning |
| --- | --- | --- |
| `endpoint` | yes | `opc.tcp://host[:port][/path]`. The connector always dials this host and port (§5.1). |
| `security_policy`, `security_mode` | no | Override the `[connection]` defaults (§3.3). |
| `user` | with a password | Username identity. |
| `password` \| `password_file` | no | At most one. A `password_file` is read at configure: its first line, without the line ending. A user without either has an empty password. |
| `user_certificate`, `user_private_key` | together | X.509 user identity (certificate DER or PEM, key PEM). The key must match the certificate and must not be readable by group or others. |
| `trust_any_server_certificate`, `allow_deprecated_security`, `allow_plaintext_password` | no | Per-device overrides of the `[connection]` switches. |

Exactly one identity: anonymous (no identity key), username, or X.509. Mixing them, a password
without `user`, both `password` and `password_file`, or only one of the two certificate keys is
a configuration error.

Secrets are never echoed: not in log output, link status, health, command results, `describe`
output or configuration errors. A password that is not a string is reported without its value.

### 3.3 Policy and mode

Policy names are the URI fragments (`Basic256Sha256`, `Aes128_Sha256_RsaOaep`,
`Aes256_Sha256_RsaPss`, …); the full `http://opcfoundation.org/UA/SecurityPolicy#…` URI is
accepted too. Modes: `none`, `sign`, `sign_and_encrypt` (`signandencrypt` is accepted as an
alias).

- The device's `security_policy` wins over the connection's. A device that sets its own
  policy but no mode gets that policy's default mode — it does **not** inherit the
  connection's mode. Otherwise the device's mode wins over the connection's.
- A policy's default mode is `none` for `None` and `sign_and_encrypt` for every other policy.
- `None` requires `none`; every other policy requires `sign` or `sign_and_encrypt`.
- Deprecated policies need `allow_deprecated_security = true`.

Violations are configuration errors naming the device and the field: `device 'plc':
security_mode 'none' cannot be used with security_policy 'Basic256Sha256' …`.

### 3.4 `point.address`

`{ node_id = "ns=2;s=Temperature" }` or `{ namespace = 2, identifier = "Temperature" | 1001 }`.

Two optional keys select one value out of a structured or array value (§3.7):

| Key | Meaning |
| --- | --- |
| `field` | A dotted path into a structure: `"Speed"`, `"Motor.Current"`, `"Items[1].Value"`. A segment may carry one zero-based element index `[n]`. |
| `index` | One zero-based element of an array variable, selected on the server through `IndexRange`. May be combined with `field` when the elements are structures. |

Points with either key are read-only (`access = "write"` or `"read_write"` is a configuration
error). `field` needs `mode = "typed"`: a raw point reads the whole structure body.

### 3.5 Reporting policy (`report`)

The SDK runtime applies the point's `report` table ([contract §5.3](../contract/ot-connector-contract.md#53-reporting-policy-report-by-exception)) to polled and
subscribed nodes alike, before a sample is published. See [Reducing data volume](../reducing-data-volume.md).

Both kinds of point support a heartbeat (`max_interval`). A polled node publishes its next
poll. A subscribed node (monitored item) only notifies on a change, so a static node would
otherwise go silent after its first value; once `max_interval` has passed without a publish,
the runtime **reads the node on demand** through the ordinary read path, bounded by
`operation_timeout`. A failed or timed-out read publishes a `bad` sample and affects the link
status like a failed poll. The packaged config sets `[connector] report = { max_interval = "30m" }`,
which keeps a device whose subscribed values never change available in Cumulocity.

### 3.6 Value mapping (`map`)

The SDK runtime applies the point's `map` ([contract §4.3](../contract/ot-connector-contract.md#43-value-mapping)) to polled and subscribed nodes
alike, after the transform, and maps a written value back before the connector writes the node.
Two cases are common. An enumeration node (an `Int32` state) can be mapped to labels, and a
`String` node holding a number can become a number with `map = { as = "number" }`. See
[Mapping values](../mapping-values.md).

### 3.7 Structured values, array elements and other built-in types (Rust build)

An OPC UA structure (a custom data type) arrives as an ExtensionObject: an encoding id and a
binary body that has no meaning without the type's layout. The connector compiles no type in. It
reads the layout from the server instead: once per session, for each node with `field` points,
it reads the variable's `DataType` and that type's `DataTypeDefinition` attribute (OPC UA 1.04),
along with the definitions of nested structures. A variable declared with the abstract
`Structure` (or `BaseDataType`) is resolved through its value: the encoding id of the structure it
holds leads to the concrete DataType by the `HasEncoding` reference. Only the types a point needs
are read. They are read again after every reconnect, so a server update is picked up.

**Datapoint mode.** Browse the server (for example with UaExpert), find the variable, and add
one point per field you want:

```toml
[[device.point]]
id       = "pump1_speed"
datatype = "float64"
address  = { node_id = "ns=2;s=Pump1.Status", field = "Speed" }

[[device.point]]
id       = "pump1_motor_current"
datatype = "float32"
address  = { node_id = "ns=2;s=Pump1.Status", field = "Motor.Current" }
```

Each field is published as an ordinary typed sample, so `transform`, `map`, `report`, `meta`
and every flow work as for any other point. Fields that share a node are served by one Read
per poll cycle and one monitored item.

- Selectable field types: Boolean, the integers, Float, Double, String, Enumerations (as
  `int32`), and the built-in types below. The declared `datatype` is checked when the definition
  is read; a mismatch gives bad samples such as `field "Speed" is Double, point declares int32
  (accepted: float64)`.
- Structures, StructureWithOptionalFields and Unions are supported. An absent optional field or
  an inactive union member gives a bad sample that says so.
- Every other built-in type, arrays and nested structures before the selected field are skipped
  by their encoded length.
- Not supported: StructureWithSubtypedValues, multi-dimensional fields, XML or JSON encoded
  bodies. Such a definition, or a server without `DataTypeDefinition` (OPC UA 1.03), gives bad
  samples starting `data type definition unavailable`. The device link is not degraded.

**Raw mode.** A `mode = "raw"` point on a structure publishes the encoded body as `raw`, with
`addr.data_type` and `addr.encoding_id` in namespace-URI form (`nsu=urn:…;i=3000`), so a flow
can decode types the connector cannot, such as those of a 1.03 server.

**Array elements.** `index = n` selects one element of an array variable, `field = "A[n]"` one
of an array field. An index beyond the current length gives bad samples naming the index; the
point recovers when the array grows. A server that ignores `IndexRange` and returns the whole
array (python-asyncua does) is handled: the connector selects the element itself.

**Other built-in types.** Values of these types are read as top-level variables, fields and
array elements; the declared `datatype` chooses the rendering:

| OPC UA type | `datatype` | Example value |
| --- | --- | --- |
| DateTime | `string` / `int64` | `2026-10-06T08:15:30.25Z` / `1791274530250` (Unix ms) |
| LocalizedText | `string` | `Betrieb` (the text; the locale is dropped) |
| StatusCode | `uint32` / `string` | `2150891520` / `BadNodeIdUnknown` |
| Guid | `string` | `72962b91-fa75-4ae6-8d28-b404dc7daf63` |
| NodeId, ExpandedNodeId | `string` | `nsu=urn:acme:types;i=1001` |
| QualifiedName | `string` | `2:Speed` |
| ByteString | `bytes` | `deadbeef` (hex) |

Writes are limited to the primitive types and String. The exact rendering rules and messages
are pinned by the vectors in `doc/contract/test-vectors/opcua-struct/`. The C build does not
implement this section yet (capability `opcua-structures`, see `impl/c/README.md`), except for a
top-level ByteString variable: it is read as `bytes` in both builds, and the C build refuses a value
longer than 127 bytes with a bad sample (its fixed value buffer) instead of truncating it.

### 3.8 Subscription timing (`sampling_interval`)

A subscribed node is not sent the moment it changes. The server runs two timers:

- **Sampling**, per monitored item: how often the server checks the node's value. The
  connector requests the point's effective `sampling_interval`
  ([contract §3.1](../contract/ot-connector-contract.md#31-common-protocol-neutral-point-fields)):
  the point's, else its device's, else `[connector]`'s, else the point's effective
  `poll_interval`. Each item has `queueSize = 1` and `discardOldest = true`, so only the
  latest value per publish is sent.
- **Publishing**, per subscription (one per device): how often the server sends the queued
  changes. It is not configurable. The connector requests the fastest effective sampling
  interval among the device's subscribed points.

A change therefore arrives up to about **one sampling interval plus one publishing interval**
after it happens. With the default `poll_interval = "2s"` and no `sampling_interval`, that is
about 4 s. To get changes faster without polling the other points faster:

```toml
[[device]]
name              = "opc-server-1"
protocol_address  = { endpoint = "opc.tcp://plc:4840/" }
poll_interval     = "10s"     # polled nodes (subscribe = false)
sampling_interval = "200ms"   # subscribed nodes: sampled and published about every 200 ms
```

`sampling_interval = "0"` asks the server for its fastest rate. The server decides what that
means; it can be far more traffic, and more load on whatever the server reads the value from.
One point at `"0"` also makes the whole device's subscription publish at the server's fastest
rate. Use it only when you need it.

The server may grant other rates than requested. When the revised sampling interval of an item,
or the revised publishing interval of a subscription, differs from the requested one, both
builds log an `info` line naming the device (and the point), the requested and the revised
value. The connector keeps what the server granted.

The publishing interval also paces the subscription keep-alive that the connector's
`check_subscription` ([SDK](../sdk/connector-sdk.md)) relies on to notice a dead session: it
allows about `publishing interval × 20 + publishing interval` without a publish. That is one
reason the publishing interval is not used as a rate limit. To limit how often a point is
published, use `report.min_interval` (§3.5): it is per point and does not delay the first change
after a quiet spell.

## 4. The PKI directory

### 4.1 Layout

```
<pki_dir>/
  own/certs/cert.der    application instance certificate
  own/private/key.pem   its private key (directory 0700, file 0600)
  trusted/certs/        trusted server certificates (pinned) and trusted CA certificates
  trusted/crl/          CRLs of the CAs in trusted/certs
  issuers/certs/        intermediate CAs: complete a chain, never trusted on their own
  issuers/crl/          CRLs of the CAs in issuers/certs
  rejected/certs/       server certificates that failed validation
```

- Files are recognised by content — a DER certificate or CRL, or PEM text with any number of
  `CERTIFICATE` / `X509 CRL` blocks — never by name or extension. A file that does not parse is
  skipped with a warning naming it; the other files still count.
- Files the connector or `tedge-dot pki` writes are named `<CN>_<thumbprint>.der` (the common
  name with every character but ASCII letters, digits, `-` and `.` replaced by `_`; the SHA-1
  thumbprint in lower-case hex) and CRLs `<thumbprint of the CRL>.crl`. Writes go through a
  temporary file and `rename(2)`.
- The trust lists are read again on every connect attempt: a change takes effect on the
  device's next (re)connect, without a reload.
- A configuration whose devices all use `None` neither creates nor reads the directory.

### 4.2 Application instance certificate

When at least one device is secured, the connector loads the certificate at configure time:
the configured `certificate`/`private_key`, else `own/`, else — when neither file exists and
`create_certificate` is true — a new one. Generation runs under an exclusive `flock` on
`own/.lock`, so connectors sharing a directory generate one certificate between them.

A generated certificate is self-signed, RSA 2048 bits, SHA-256, with a random serial,
valid for 5 years, with key usage digitalSignature/nonRepudiation/keyEncipherment/
dataEncipherment/keyCertSign, extended key usage serverAuth/clientAuth, and the subject
alternative names `application_uri` (first) followed by the machine's host name.

The certificate is refused when its key does not match, when the key file is readable by group
or others, or when `application_uri` is not one of its SANs. A certificate that cannot be
obtained is **not** a configuration error: unsecured devices work, and each secured device
stays `disconnected` with the reason `application certificate: …`. The same applies while the
certificate is expired. The connector warns at configure time and then at most once a day while
the certificate expires within 30 days. A renewed certificate is picked up on reload or restart.

## 5. Connecting

### 5.1 Endpoint selection

An anonymous device without security dials `endpoint` directly. Every other device first
calls GetEndpoints on `endpoint` (over a channel without security, which servers allow for
discovery) and selects the endpoint with the device's policy and mode that offers a user token
policy for its identity, preferring the highest `securityLevel`. The selected endpoint's URL is
replaced *in full* by the configured `endpoint` — host, port and resource path: the session
always goes to the configured address (servers behind NAT advertise names the client cannot
reach, and the UA-.NETStandard reference server advertises `opc.tcp://0.0.0.0:<port>`, which
nothing can dial), and host-name checks use the configured host.

No endpoint with the policy and mode → `no matching endpoint: the server offers no <policy>/<mode>
endpoint (it offers <policy>/<mode>, …)` (sorted, deduplicated). A policy this connector does
not implement is listed by its full URI rather than dropped, so a server offering only, say,
the ECC policies explains itself instead of appearing to offer nothing. Such endpoints exist, but none
accepts the identity → `identity unsupported: …`. The token policy must be listed, for an
anonymous identity too: an endpoint that lists no token policies is not usable, since neither
client library can activate a session on it.

### 5.2 Server certificate validation

Before dialling a secured endpoint, the connector validates the certificate the endpoint
advertises; the session validates the certificate of the channel again the same way. Steps, in
order:

0. **Key length** for the policy is checked first, before any trust lookup: 2048–4096 bits
   (1024–2048 for the deprecated policies). A key that is too short cannot be made acceptable by
   trusting the certificate, so reporting it as untrusted — and advising `tedge-dot pki trust` —
   would send the operator in a circle. Such a certificate is refused as `certificate invalid:`
   naming the key size and the policy, and is *not* copied to `rejected/certs`, which is a list
   of certificates an operator could choose to trust. (The UA-.NETStandard reference server
   auto-generates a 1024-bit certificate, so this is not hypothetical.)
1. **Pinned.** A certificate identical to one in `trusted/certs` is trusted.
2. **Chain.** Otherwise the path is built from the certificate through CAs (basicConstraints
   `cA`, subject = issuer, valid signature: RSA PKCS#1 v1.5 with SHA-1/256/384/512 or RSA-PSS
   with SHA-256/384/512) taken from `trusted/certs` then `issuers/certs`, until a CA from
   `trusted/certs` is reached. No path, a path longer than 8, or a self-issued CA that is only
   in `issuers/` → untrusted.
3. **Revocation.** For every certificate on the path, the CRLs issued (named and signed) by its
   issuer are looked up in both `crl` directories. None → revocation unknown. Its serial listed
   → revoked. A CA outside its validity period → issuer time invalid.
4. **Validity period** of the certificate.
5. **Host name**: one of the certificate's SANs after the first equals the configured host
   (case-insensitive).
6. **Application URI**: the first SAN equals the server's advertised `applicationUri`.

An untrusted certificate (steps 1–2) is copied to `rejected/certs` unless it is already there.
`rejected/certs` is a list for the operator to review (OPC UA Part 12), not a deny list: a
certificate that is pinned or chains to a trusted CA is trusted even when a copy is still there,
as happens when a server was first seen before its CA was trusted. To stop trusting a single
certificate that a trusted CA issued, revoke it in that CA's CRL.

A server may advertise its certificate chain (leaf first, then its CAs). Both builds validate
the first certificate; the CAs it sends are not used, so intermediates belong in
`issuers/certs`.

### 5.3 Reasons (link status)

A security failure leaves the device `disconnected` with a `reason` starting with one of:

| Prefix | Status codes / cause |
| --- | --- |
| `certificate untrusted:` | BadCertificateUntrusted, BadSecurityChecksFailed while validating the server certificate; includes the thumbprint, the subject and ``trust it with `tedge-dot pki trust <8 hex>` ``. |
| `application certificate rejected:` | The same status codes, but *after* §5.2 passed (or was skipped by §5.4): the refusal is then the server's judgement of **our** certificate, not ours of theirs. Names our own thumbprint and says to hand `tedge-dot pki export` to the server administrator. Only the ordering tells the two apart — the wire says the same thing both ways. |
| `certificate invalid:` | BadCertificateTimeInvalid, BadCertificateIssuerTimeInvalid, BadCertificateHostNameInvalid, BadCertificateUriInvalid, BadCertificateInvalid, BadCertificatePolicyCheckFailed, BadCertificateUseNotAllowed, BadCertificateIssuerUseNotAllowed, BadCertificateChainIncomplete |
| `certificate revoked:` | BadCertificateRevoked, BadCertificateIssuerRevoked, BadCertificate[Issuer]RevocationUnknown (the text says "revocation unknown" and to add the CRL, an empty one if the CA revoked nothing) |
| `no matching endpoint:` | §5.1 |
| `identity rejected:` | BadIdentityTokenRejected, BadIdentityTokenInvalid, BadUserAccessDenied |
| `identity unsupported:` | §5.1 |
| `plaintext password refused:` | §6.2 |
| `application certificate:` | §4.2 |

Consumers match the prefix only; the text after it is for people. The link status is published
when the status changes, so the reason shown is that of the failure that made the device
`disconnected`.

### 5.4 `trust_any_server_certificate`

Skips §5.2 entirely (messages are still signed/encrypted as configured) and writes nothing to
the PKI directory. The connector logs a warning on every connect.

## 6. Identity

### 6.1 Tokens

The token uses the selected endpoint's user token policy. A username token is encrypted as the
policy (or, when it names none, the channel's policy) requires; an X.509 token is signed with
the user key using the policy's algorithm.

### 6.2 Plaintext passwords

A password is unprotected, and refused — `plaintext password refused: …` — unless
`allow_plaintext_password` is true (then a warning is logged on every connect), when:

- the channel has no message security (`none`), whatever the username token policy says. OPC UA
  Part 4 Table 193 lets such a token be encrypted, but to a server certificate that nothing on
  a `none` channel authenticates (neither client library validates it there), so any server
  the device is pointed at could decrypt it;
- the channel is signed only (`sign`) and the token policy explicitly names `None`: the password
  is then in clear on the wire.

On `sign_and_encrypt`, or with a token policy that names a real policy on `sign`, the server
certificate was validated (§5.2) and the password is protected.

## 7. Status

Link status `info` (contract §8):

```json
{ "endpoint": "opc.tcp://plc:4840/", "security_policy": "Basic256Sha256",
  "security_mode": "sign_and_encrypt", "server_certificate": "trusted",
  "server_thumbprint": "a1b2…" }
```

`server_certificate` (`"trusted"` or `"not_verified"`) and `server_thumbprint` are present once
known for a secured device. The object never holds a credential.

## 8. `tedge-dot pki`

`tedge-dot pki <action> [--pki-dir <dir>] [-c|--config <file>] [--json]` works offline (no
broker, no server). The PKI directory is `--pki-dir`, else the `pki_dir` of `--config`, else of
`/etc/tedge/plugins/ot/opcua.toml` when it exists, else the default. The connection settings
(`application_name`, `application_uri`, `certificate`, `private_key`) come from the same file.
Exit status: 0 success, 1 usage or input error, 2 the named certificate does not exist.

| Action | Does |
| --- | --- |
| `show` | The application certificate: paths, subject, issuer, thumbprint, validity, application URI, host names, whether the URI matches `application_uri`. |
| `export [--pem] [-o FILE]` | Write the certificate (DER by default), never the key. |
| `create [--application-uri URI] [--hostname NAME]... [--days N] [--force]` | Generate the certificate in `own/` (§4.2). An existing one is refused unless `--force`, which keeps it as `cert.der.<unix time>` / `key.pem.<unix time>`. |
| `list [trusted\|issuers\|rejected]` | Certificates with group, thumbprint, subject, expiry, whether a CA and (for a CA) whether its CRL is present. |
| `trust <thumbprint\|file>` | Move a rejected certificate to `trusted/`, or import every certificate of a file there (and out of `rejected/`). |
| `reject <thumbprint>` | Move a trusted certificate to `rejected/`. Warns when the certificate is still trusted through a CA (revoke it instead). |
| `remove <thumbprint> [--group G]` | Delete a certificate. |
| `add-issuer <file>` | Import CA certificates into `issuers/` (non-CA certificates are refused). |
| `add-crl <file>` | Store each CRL next to the CA (trusted or issuers) that issued it; a CRL of an unknown CA is refused. |

A thumbprint is matched case-insensitively and may be a prefix of at least 8 hex digits; a
prefix matching several certificates is an error listing them. Run as root on a PKI directory
another user owns (the packaged one belongs to `tedge`), the command works as that user
(effective user and group): what it writes belongs to the owner, and a symlink placed in the
directory cannot redirect a write to a file only root may change. The file an action reads
and the `export --output` file are accessed as root.

JSON field names (both builds; keys unordered):

- `show`/`create`: `certificate`, `private_key`, `subject`, `issuer`, `thumbprint`,
  `not_before`, `not_after`, `application_uri` (or `null`), `hostnames`,
  `configured_application_uri`, `uri_matches`, and for `create` `created`.
- `list`: `pki_dir`, `certificates[]` of `{group, thumbprint, subject, not_after, ca, crl, file}`
  (`crl` is `null` for a non-CA).
- `trust`/`reject`/`remove`/`add-issuer`: `action`, `certificates[]` (as above).
- `add-crl`: `action`, `crls[]` of `{file}`.
- `export`: `thumbprint`, `format`, and `file` (with `--output`) or `pem`.

Subjects print as `CN=…, O=…` in encoding order; times as `YYYY-MM-DDTHH:MM:SSZ`.

## 9. Packaging

The packages create `/var/lib/tedge-dot/opcua/pki` (owner `tedge`, mode 0750) with
`own/private` (0700), generate nothing, and leave the contents alone on upgrade and removal. A
Debian purge deletes the directory.

## 10. Tests

| Layer | Where |
| --- | --- |
| PKI vectors (both builds, same scenarios) | `connector-opcua/tests/pki.rs`, `impl/c/tests/opcua_pki.c` over `genpki.py` |
| Configuration rules | `connector-opcua/src/config.rs`, `impl/c/tests/config.c`; secrets property test `connector-opcua/tests/secrets.rs` |
| Secured sessions (in-process server) | `connector-opcua/tests/security.rs` |
| Conformance over secured channels | `connectors/opcua/conformance-secure.toml`, `conformance-secure-c.toml` |
| End to end | `connectors/opcua/tests/opcua_security_e2e.robot` (`docker-compose.secure.yaml`) |
| Interop, third-party server | `connectors/opcua/interop/tests/opcua_interop.robot` (`just test-interop opcua`, and `-c`) |
| `tedge-dot pki` | `impl/rust/tests/pki_cli.rs`, `impl/c/tests/opcua_pki_cli.sh`, parity `impl/c/ci/pki-parity.sh` |
| Fuzzing | `connector-opcua/fuzz` (`pki_files`) |

Every layer above the interop one runs against a server this project wrote, so a misreading of
the specification is invisible to it. The interop suite exists to catch that: it runs both
implementations against the OPC Foundation UA-.NETStandard reference stack (packaged by
`php-opcua/uanetstandard-test-suite`, MIT). It is a separate CI job from `e2e`, so an upstream
regression cannot fail the suites that gate our own behaviour, and it is the only place where
`Aes128_Sha256_RsaOaep`, `Basic256` and `Basic128Rsa15` are exercised on the wire.
