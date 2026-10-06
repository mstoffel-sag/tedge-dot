# `snmp` Connector Spec — polling and notifications

| Field | Value |
| --- | --- |
| Status | Implementable |
| Protocol id | `snmp` |
| Crate | `connector-snmp` (feature `snmp`) · C module `impl/c/connectors/snmp/` (option `TDOT_SNMP`) |
| Builds on | Rust: [`snmp2`](https://github.com/roboplc/snmp2) (MIT/Apache-2.0). C: [net-snmp](https://github.com/net-snmp/net-snmp) `libnetsnmp` (BSD-style), built from source as a minimal static library. [Connector SDK](../sdk/connector-sdk.md) |
| Implements | [OT Connector Contract](../contract/ot-connector-contract.md) v0.1.0 |
| Golden vectors | [connectors/snmp/conformance/trap-vectors.json](../../connectors/snmp/conformance/trap-vectors.json) |

## 1. Scope

One connector talks to SNMP equipment both ways a site needs:

| Operation | Versions | Library call (Rust / C) | Contract |
| --- | --- | --- | --- |
| GET (batched) | v1, v2c, v3 | `AsyncSession::get_many` / `snmp_synch_response` with `SNMP_MSG_GET` | polled `read_points` |
| GETBULK (scalars) | v2c, v3 | `AsyncSession::getbulk` (non-repeaters = N over the scalars' PARENT OIDs) / `SNMP_MSG_GETBULK` | same, one request per batch (§5.1) |
| SET | v1, v2c, v3 | `AsyncSession::set` / `SNMP_MSG_SET` | `write` (and SDK `write-batch`) |
| Trap receive | v1, v2c, v3 | `Pdu::from_bytes[_with_security]` on our socket / `netsnmp_transport_open_server` + `snmp_add` callback | `subscribe` |
| Inform receive + Response | v2c, v3 | Response built with the library / `snmp_send` of a cloned `SNMP_MSG_RESPONSE` | `subscribe` |

Out of scope for this increment (see `TODO.md`): table walks into per-row points (a contract change —
row instances exist only at runtime while the contract's point list is static), MIB name
resolution, AgentX, and SNMP over TCP/DTLS.

As for every connector, meaning is flow work: trap points become events (`ot-event`,
`meta.event.every = true`, §9) or alarms (`ot-alarm`); numeric polled points become measurements.

### Library policy

The protocol encoding is the libraries' job; the connector owns only what the contract adds on
top — point matching, the notification semantics of §4.3, and datatype conversion (§6). Where a
library is too lenient for the shared golden vectors, the gap is fixed **in the library** (Rust:
a patched copy in `impl/rust/vendor/snmp2` with a `TEDGE-DOT-PATCH.md`, and the patch proposed
upstream — the async-opcua precedent) rather than worked around with a second decoder.

## 2. Capability descriptor

```json
{
  "protocol": "snmp",
  "version": "0.1.0",
  "modes": ["raw", "typed"],
  "datatypes": ["bool", "int8", "uint8", "int16", "uint16", "int32", "uint32",
                "int64", "uint64", "float32", "float64", "string"],
  "point_kinds": ["object", "trap", "varbind"],
  "command_verbs": ["write", "write-batch", "set-config", "define-device", "remove-device"],
  "features": ["polling", "subscribe", "bulk_read", "snmpv3", "management"],
  "subscribe": true
}
```

`write` is declared by the module; the runtime adds `write-batch` and the management verbs.

## 3. Configuration

### 3.1 `connection`

| Field | Default | Notes |
| --- | --- | --- |
| `listen` | `"0.0.0.0:162"` | UDP address notifications are received on (`<IPv4>:<port>`, `[<IPv6>]:<port>`; an IPv6 wildcard also receives IPv4). **Bound only while at least one device has a trap point.** No `SO_REUSEADDR`: a port another receiver holds fails loudly. |
| `community` | *(any)* | String or non-empty list: communities accepted on v1/v2c notifications, unless a device overrides it. |
| `forwarders` | `[]` | IP addresses or DNS names (resolved when the connector connects) of trusted trap forwarders (e.g. a host `snmptrapd` configured with `forward default udp:127.0.0.1:1162`). A notification from one of them is routed by the value of its **last** `snmpTrapAddress.0` varbind (`1.3.6.1.6.3.18.1.3.0`) instead of its UDP source; without that varbind it is dropped. Note that stock `snmptrapd` 5.9.x forwards v1/v2c notifications unchanged and does **not** add the varbind (the code adding it exists only on net-snmp's development branch), so with it the sender must include `snmpTrapAddress.0` itself — many devices do on request, and proxies/forwarders that add it (newer net-snmp, commercial trap proxies) work as is. Informs from a forwarder are acknowledged to the forwarder. |
| `engine_id` | *(derived)* | Hex string, 5–32 octets: this receiver's SNMPv3 engine ID, authoritative for **v3 informs**. Default: `80000000` `05` followed by the first 8 octets of SHA-256 of `/etc/machine-id` (the hostname when absent) — stable across restarts. |
| `request_timeout` | `"2s"` | Default per-request timeout for polling/SET (duration). |
| `retries` | `1` | Default number of re-sends after a timeout. |
| `max_varbinds` | `20` | Most OIDs in one GET/GETBULK/SET request; larger batches are split. |

### 3.2 `device.protocol_address`

| Field | Required | Default | Notes |
| --- | --- | --- | --- |
| `host` | yes | | IP address or DNS name; resolved when the device connects. Used for requests **and** to route notifications (by source address). |
| `port` | no | `161` | Agent port for GET/SET. |
| `version` | no | `"v2c"` | `"v1"`, `"v2c"` or `"v3"`. Applies to requests; notifications are accepted in the version they arrive in, checked against this device's credentials. |
| `community` | no | `"public"` for requests | v1/v2c: string used for GET. For received notifications, the accepted list is this key (string or list) else `[connection] community`. |
| `write_community` | no | `community` | v1/v2c community used for SET. |
| `v3` | when `version = "v3"` or to accept v3 notifications | | Table below. |
| `request_timeout`, `retries`, `max_varbinds` | no | `[connection]` | Per-device overrides. |
| `bulk` | no | `true` for v2c/v3 | Use GETBULK with non-repeaters = N for batches (§5.1); `false` forces GET. |

`v3` table:

| Field | Required | Notes |
| --- | --- | --- |
| `user` | yes | USM user name. |
| `level` | no | `"noAuthNoPriv"`, `"authNoPriv"`, `"authPriv"`; default derived from which passwords are present. |
| `auth_protocol` | with auth | `"MD5"`, `"SHA"` (SHA-1); `"SHA224"`, `"SHA256"`, `"SHA384"`, `"SHA512"`: Rust only (capability `snmpv3-sha2`, §10). |
| `auth_password` / `auth_password_file` | with auth | Exactly one. A `_file` path is read at configure (first line, trailing newline stripped), so the secret need not live in a config file management commands rewrite. A management command may not add or change either `_file` key (contract §3.4). |
| `priv_protocol` | with priv | `"DES"`, `"AES"` (AES-128). `"AES192"`, `"AES256"` (Blumenthal key extension): Rust only (capability `snmpv3-sha2`, §10). |
| `priv_password` / `priv_password_file` | with priv | Exactly one. |
| `context` | no | Context name for requests. |
| `engine_id` | no | Hex: the device's engine ID. Requests discover it when absent; a v3 **trap** is only accepted from that engine once known. Known means: configured here; or learned from the first authenticated trap; or, when a poll has *authenticated* with it, from that exchange — the Rust build adopts a discovered engine only at `level` authNoPriv or above, because discovery itself (RFC 3414 §4) carries no HMAC and anything answering at the device's address could otherwise choose it. The C build adopts the engine its session discovered, unauthenticated, and does not revise it afterwards (see `TODO.md`). |

Secrets are never echoed: not in link-status `info` (which is `{ "host", "port", "version" }`), the
capability descriptor, `describe` output, or log lines. The packaged config file is installed
`0640 tedge:tedge`.

An unknown key in `connection`, `protocol_address` or `v3` is a configuration error. Two devices
with the same `host` literal (compared as addresses) are rejected; names resolving to one address
are reported at connect (the later device `disconnected`).

### 3.3 `point.address`

| Kind | Address | Delivered by | Access | Allowed typed datatypes |
| --- | --- | --- | --- | --- |
| `object` | `{ oid = "<instance OID>", type? }` | polling (GET) | `read`, `read_write`, `write` | all advertised |
| `trap` | `{ trap = "<OID>" \| [..] }` | notification | `read` | `string` (the trap OID), `bool` (`true`) |
| `varbind` | `{ trap = …, oid = "<OID>" }` | notification | `read` | all advertised |

- `oid` of an **object** is the full instance (`1.3.6.1.2.1.1.3.0`); a response naming a
  different OID, or an exception (`noSuchObject`, `noSuchInstance`, `endOfMibView`), is a `bad`
  sample for that point.
- `trap` matches a notification whose trap OID (§4.3) equals one of them or lies beneath it.
- A **varbind** point selects the first varbind at or beneath `oid`; a matching notification
  without it yields a `bad` sample.
- `type` (object points, needed for SET only): the SNMP type written — `"integer"`, `"unsigned32"`,
  `"gauge32"`, `"counter32"`, `"counter64"`, `"timeticks"`, `"octet_string"`, `"ip_address"`,
  `"oid"`. Default from the datatype: `bool`/signed integers → `integer`, `uint8`–`uint32` →
  `unsigned32`, `uint64` → `counter64`, `string` → `octet_string`; floats have no default and
  require `type`. A writable point whose type cannot be derived is a configuration error.
  A writable point with a `transform` on an integer SNMP type declares the integer datatype
  (e.g. `uint32` for `timeticks` with `divisor = 100`): the SDK rounds the inverted write value
  only for integer datatypes (contract §4.2), and a fractional value is rejected on SET.
- OID syntax: dotted decimal, optional leading `.`, 2–128 arcs, first arc 0–2, second < 40 unless
  the first is 2, 32-bit arcs.
- Rules: `subscribe = false` is rejected on trap/varbind points and ignored on objects (they are
  polled anyway); trap/varbind points must be `access = "read"`; `datatype = "bytes"` is not
  supported (use `mode = "raw"`).

## 4. Notifications

### 4.1 Receive flow

For each datagram on the listening socket:

1. **Route** by UDP source (IPv4-mapped IPv6 compared as IPv4). From a trusted forwarder, route by
   its last `snmpTrapAddress.0` instead (the message must be decoded first for that). Unknown
   source: drop, debug log (once a minute per source), never acknowledge.
2. **Decode** with the library, with the device's v3 credentials for a v3 message. Failure: drop,
   warning (once a minute per source).
3. **Authenticate**: v1/v2c community in the accepted list; v3 user/engine/keys per §3.2 (the
   library's USM checks). Failure: drop, warning (once a minute per device). A v3 notification
   below the device's configured `level` is refused **without becoming the engine the device's
   notifications are then expected from**. Authenticating a v3 message requires keys localized
   to the engine ID the message *claims*, so a receiver that adopts that engine as the device's
   before judging the level can be made to discard the real one by a single unauthenticated
   datagram carrying the user name, which every v3 message sends in the clear; the device's
   notifications then fail the engine check until the connector restarts. Both implementations
   judge the level before that adoption. Neither promises to leave *no* trace of a refused
   message: the C build hands the datagram to net-snmp's USM, which localizes keys for the
   claimed engine in order to attempt verification at all, and those entries are process-wide
   and are not removed again (see `TODO.md`).
4. **Acknowledge** an InformRequest: a Response with the same request-id and varbinds, error-status
   and error-index 0, sent from the listening socket to the datagram's source. For v3 the
   receiver is the authoritative engine (`[connection] engine_id`, boots and time kept by the
   connector): an inform from a sender that has not discovered it is answered with the USM
   Report the library produces (unknown engine ID / not in time window), exactly as `snmptrapd`.
   `snmpEngineBoots` MUST be at least 1 (RFC 3414 §2.2 reserves 0 for an engine that has not
   initialised its time): a receiver advertising 0 is refused by a sender that then sends boots
   and time 0, is answered `usmStatsNotInTimeWindows`, re-synchronises and repeats, so the inform
   never completes instead of failing. Without a file to count restarts in, both implementations
   derive it from the wall clock — seconds since 2020-01-01, clamped to 1…2³¹−2 — which grows
   across restarts, as a sender caching our boots requires.
5. **Publish** one sample per subscribed trap/varbind point the notification matches, in
   configuration order.

### 4.2 Decoding requirements

Whatever the library, the shared vectors hold (§8): every `messages[*]` decodes to its expectation
and every `malformed[*]` is rejected. In particular a varbind list that does not decode completely
rejects the whole notification (a library that stops iterating silently must be patched), and a
Counter32/Gauge32/TimeTicks above 2³²−1 is an error, not a truncation.

### 4.3 Notification semantics (connector-owned)

- v1/v2c: exactly as before — v1 `generic-trap` 0–5 → `1.3.6.1.6.3.1.1.5.<g+1>`, 6 →
  `<enterprise>.0.<specific>` (32-bit specific, ≤ 128 arcs), other generic values rejected.
- v2c/v3 notifications: trap OID = value of the first `snmpTrapOID.0` varbind (must be an OID);
  uptime = first `sysUpTime.0` when TimeTicks.
- Only Trap-PDU (v1), SNMPv2-Trap-PDU and InformRequest-PDU are notifications; anything else on
  the notification port is rejected.
- Limits: 256 varbinds, 128 arcs per OID.

## 5. Polling and writes

### 5.1 Reads

`read_points` receives the device's due points; object points are fetched in batches of at most
`max_varbinds`, each batch one request.

A GETBULK's non-repeaters are answered with the **lexicographic successor** of each requested OID
(RFC 3416 §4.2.3: GETNEXT semantics), not with the OID itself, so a batch asks for the **parent**
of each scalar instance — the successor of `x` is `x.0` while that instance exists, and any other
name coming back means it does not. Only scalar object points (an OID ending in `.0`) can be read
that way: they go in GETBULK batches (non-repeaters = batch size, max-repetitions 0) when `bulk`
is on and the version is v2c/v3. Every other object point — and every point of a v1 device or one
with `bulk = false` — goes in a GET batch. Per batch:

| Outcome | Samples |
| --- | --- |
| Response, error-status 0 | one per point; an exception or a varbind naming another OID is `bad` |
| error-status ≠ 0 (v1 `noSuchName`, …) | v2c/v3: the whole batch `bad` with the error. v1: the point at `error-index` is `bad`, and the remaining points are re-requested without it (at most once per point per cycle) |
| timeout after `retries` | whole batch `bad` with the reason, and the cycle's remaining batches are reported `bad` unasked (a device that stopped answering must not cost one timeout per batch). Every sample of the device is then `bad`, which is what degrades the link and starts the runtime's reconnect/backoff |
| v3 auth/USM failure | whole batch `bad`, transport error |

Rust wraps every `AsyncSession` call in `tokio::time::timeout(request_timeout)` and re-sends up to
`retries` times (the library has no timeout of its own).

### 5.2 Writes

`execute("write")` on an object point with `access` allowing writes: encode `value` (typed) per
`type` with range checks against the SNMP type (e.g. `unsigned32` rejects negatives, `integer`
32-bit), or `raw` hex as the content octets of `type`; one SET with `write_community`/v3; success
when error-status is 0. The result echoes the value. Failures carry the SNMP error name
(`notWritable`, `wrongType`, `noAccess`, …) in `reason`.

### 5.3 Reporting policy (`report`)

The SDK runtime applies the point's `report` table ([contract §5.3](../contract/ot-connector-contract.md#53-reporting-policy-report-by-exception)) to polled objects
and to notification points before a sample is published. See [Reducing data volume](../reducing-data-volume.md).

A polled object point supports a heartbeat (`max_interval`): its next poll. Trap and varbind
points **cannot be read on demand** (Rust returns `Unsupported`, C `TDOT_READ_NO_DATA`), so
they get no heartbeat and are published only when a notification arrives. Do not give a
change filter or a debounce to a notification point whose event uses `meta.event.every`: two
identical notifications are two occurrences, and the runtime warns about the combination. The
packaged config shows `max_interval` only as a commented-out example, since without a change
filter every polled object is already published at each poll.

### 5.4 Value mapping (`map`)

The SDK runtime applies the point's `map` ([contract §4.3](../contract/ot-connector-contract.md#43-value-mapping)) to polled objects and
notification points after the conversion below, and maps a written value back before a `SET`.
Two cases are common. An `INTEGER` enumeration (`ifOperStatus`: `1` up, `2` down) can be mapped
to labels, and a `DisplayString` holding a number can become a number with
`map = { as = "number" }`. See [Mapping values](../mapping-values.md).

## 6. Converting a value to a datatype (typed mode)

Unchanged, and shared by polled objects and varbind points:

| SNMP type | `bool` | `int8` … `uint64` | `float32` / `float64` | `string` |
| --- | --- | --- | --- | --- |
| `integer`, `counter32`, `gauge32`, `timeticks`, `counter64` | ≠ 0 | in range, else `bad`; beyond ±(2⁵³−1) a decimal string | nearest float | decimal |
| `octet_string` | `bad` | `bad` | `bad` | strict UTF-8, else `bad` |
| `oid`, `ip_address` | `bad` | `bad` | `bad` | dotted decimal |
| `opaque`, `null`, exceptions, unknown | `bad` | `bad` | `bad` | `bad` |

`raw` is the **canonical content octets** of the value: the DER encoding of what it decodes to —
minimal two's complement for every integer type, so an unsigned value whose top bit is set carries
a leading `00`, and the canonical (minimal sub-identifier) OID encoding — with OCTET STRING, Opaque
and unknown types kept as received. An implementation whose library hands over decoded values
rather than the octets therefore produces the same `raw` as one that keeps them, whatever
non-minimal encoding the sender used. For a trap point it is the trap OID's encoding. A `bad`
conversion keeps `raw`; a missing varbind or an exception has empty `raw`.

Sample `addr`:

```json
{ "oid": "1.3.6.1.2.1.1.3.0" }                                         // object point
{ "source": "192.168.10.2", "version": "v3", "pdu": "inform",
  "trap": "1.3.6.1.6.3.1.1.5.3", "oid": "1.3.6.1.2.1.2.2.1.1.3",
  "forwarder": "127.0.0.1", "agent_addr": "10.0.0.9" }                  // notification points
```

`forwarder` only when routed through one, `agent_addr` only for v1, `oid` only when a varbind was
selected.

## 7. Status and liveness

- `connected`: host resolved and — when the device has trap points — the listening socket bound.
  Polling failures degrade/reconnect through the runtime as for any polled protocol.
- A device with only trap points is never `degraded` by silence.
- Rust `check_subscription` / C drain return: a listener receive error other than would-block,
  interrupted, or connection refused/reset reports the link down.
- Link `info`: `{ "host", "port", "version" }`.

## 8. Acceptance vectors

`connectors/snmp/conformance/trap-vectors.json` (generated by an independent Python reference
decoder from net-snmp captures and synthetic edge cases) is read by both implementations' unit
tests through the **library-based** decode path plus §4.3:

- `messages` — full expected decoding (version, community, PDU kind, request-id, agent-addr,
  uptime, trap OID + canonical encoding, every varbind's name/type/value/raw);
- `malformed` — must be rejected;
- `conversions` — §6.

- `v3` — SNMPv3: `users` (the credential sets the captures were sent with, from which a reader
  localizes the keys itself), `receiver` (the engine a probe is answered from), `messages` (authPriv
  SHA/AES, authNoPriv SHA, authPriv MD5/DES and noAuthNoPriv traps, each with the v3 header on top
  of the v1/v2c expectation), `rejected` (a wrong password) and `probes` (an engine ID discovery
  probe with the Report that answers it). Captured from net-snmp with a fixed engine ID by
  `conformance/tools/capture-v3.sh`; the expectations come from a Python + `openssl` reference
  (`conformance/tools/v3vectors.py`), neither implementation's code.

The e2e suite (`connectors/snmp/tests/`) runs against both implementations with:

- `agent` — a pysnmp SNMP agent: v1/v2c communities and v3 users (authNoPriv SHA, authPriv
  SHA/AES), scalars of every type, a value that changes, writable integer/string objects;
- `simulator` — net-snmp `snmptrap`/`snmpinform` for v1, v2c and v3 notifications;
- `stranger` — an unconfigured sender; `forwarder` — `snmptrapd` forwarding to the connector.

## 9. Flows

Unchanged: trap points are occurrences — `meta.event = { …, every = true }` raises an event for
every notification; alarms use a trap point matching the raise and clear notifications with
`when = { equals = "<raise OID>" }`.

## 10. Implementation notes

**Push and polling on one device.** Object points are polled while trap/varbind points of the same
device are pushed. The C SDK already lets a module accept points one by one
(`subscribe_device` sets `pt->subscribed`). The Rust runtime marks every point it passes to
`Connector::subscribe` as pushed, so the Rust SDK gains a backward-compatible hook — a
`Connector` method, default "every point", telling the runtime which points a module delivers by
push — and only those are passed to `subscribe` and taken off the polling schedule.

**Rust.** One `AsyncSession` per device (v1/v2c/v3), recreated on reconnect; batches as §5.1.
The listener task parses with `Pdu::from_bytes` (v1/v2c) or `from_bytes_with_security` using the
routed device's `Security` (v3 traps). v3 informs need receiver-side USM (authoritative engine,
Report generation), which `snmp2` does not provide: implement it in the vendored copy and propose
it upstream. Known library gaps to patch: silent varbind truncation, 32-bit value narrowing,
the `unsafe` write through a shared slice when zeroing authentication parameters, no request
timeout.

**C.** net-snmp is built by CMake from a pinned release tarball (`ExternalProject`/FetchContent,
checksum pinned) with `--disable-agent --disable-applications --disable-mibs --with-mib-modules=""
--without-perl-modules --with-openssl=internal --disable-shared --with-transports="UDP UDPIPv6"
--with-security-modules=usm`, for native and zig cross builds (glibc 2.17 floor; verified: a
static aarch64 build links against `GLIBC_2.17` symbols only, ~0.5 MB). `--with-openssl=internal`
is net-snmp's own copy of the OpenSSL routines USM needs: MD5 and SHA-1 authentication, DES and
AES-128 privacy, no system OpenSSL. SHA-2 authentication and AES-192/256 need a real OpenSSL, so
they are Rust-only — tracked as the `snmpv3-sha2` capability in `C_MISSING_CAPABILITIES`, with
the tests that need it tagged `requires:snmpv3-sha2`. A C configuration naming one of them still
loads: that device connects `disconnected` with a reason naming the capability, and every other
device of the connector works — a config shared between the two builds must not fail wholesale. Requests use one `netsnmp_session` per device (`snmp_sess_open`, single-
session API, so no global session list is shared between connector threads); notifications use a
server transport (`netsnmp_transport_open_server`) with `snmp_sess_add` and a callback that
queues per device; `drain_subscriptions` runs `snmp_sess_select_info`/`snmp_sess_read` with a
zero timeout. v3 users for notifications are created with the library's USM API
(`usm_create_user` + `usm_set_user_password`-equivalent key localisation) per device.
