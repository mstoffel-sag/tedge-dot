# `aveva-pi` Connector Spec — AVEVA PI Web API

| Field | Value |
| --- | --- |
| Status | Implementable. The connector is a **preview**: tested against the project's PI Web API simulator, not yet against a real PI System. |
| Protocol id | `aveva-pi` |
| Crate | `connector-aveva-pi` (feature `aveva-pi`) · C module `impl/c/connectors/aveva-pi/` (option `TDOT_AVEVA_PI`) |
| Builds on | Rust: [`reqwest`](https://github.com/seanmonstar/reqwest) with rustls (MIT/Apache-2.0), [`tokio-tungstenite`](https://github.com/snapview/tokio-tungstenite) with rustls (MIT). C: [libcurl](https://curl.se/libcurl/) ≥ 8.11 (curl license, MIT-style) built statically on the fetched mbedTLS 3.6, [cJSON](https://github.com/DaveGamble/cJSON) (MIT). [Connector SDK](../sdk/connector-sdk.md) |
| Implements | [OT Connector Contract](../contract/ot-connector-contract.md) v0.3.0, including §5.3 rule 7 (replayed readings) |
| Golden vectors | [connectors/aveva-pi/conformance/decode-vectors.json](../../connectors/aveva-pi/conformance/decode-vectors.json) |
| Design | [openspec/changes/aveva-pi-connector/design.md](../../openspec/changes/aveva-pi-connector/design.md) |

AVEVA, PI System, PI Server and PI Web API are trademarks of AVEVA Group Limited or its
subsidiaries. tedge-dot is not affiliated with or endorsed by AVEVA. The connector talks to the
documented PI Web API REST interface only.

## 1. Scope

The connector reads and writes PI points and AF attributes through a PI Web API server, over
HTTPS. It is tedge-dot's first connector for a REST/JSON system rather than a fieldbus, and the
worked example in [Writing a connector for an HTTP/REST system](rest-connectors.md).

| Operation | PI Web API request | Contract |
| --- | --- | --- |
| Resolve a PI point | `GET points?path=\\<server>\<tag>&selectedFields=WebId;Name;PointType;EngineeringUnits;DigitalSetName` | on connect |
| Resolve an AF attribute | `GET attributes?path=\\<af>\<db>\<element>\|<attribute>&selectedFields=WebId;Name;Type;DefaultUnitsNameAbbreviation` | on connect |
| Digital state set | `GET dataservers?path=\\<server>`, `GET dataservers/{webId}/enumerationsets`, `GET enumerationsets/{webId}/values` | on connect, for digital points |
| Current values (batched) | `GET streamsets/value?webId=..&webId=..&selectedFields=..` | polled `read_points`, heartbeat reads |
| Archived events | `GET streamsets/recorded?webId=..&startTime=..&endTime=*&maxCount=..&boundaryType=Inside` | recorded delivery, backfill |
| Push | WebSocket `streamsets/channel?webId=..&includeInitialValues=..&heartbeatRate=..` | `subscribe` |
| Write | `POST streams/{webId}/value?updateOption=..&bufferOption=..` | `write` (and SDK `write-batch`) |

Scaling, naming, alarms and shaping stay in flows. Out of scope: Kerberos/NTLM authentication,
discovery or browsing, writing history or annotations, static AF attribute values (no PI Point
data reference), event frames and AF analyses, and edits of archived values older than the
delivery cursor.

## 2. Capability descriptor

```json
{
  "protocol": "aveva-pi",
  "version": "0.1.0",
  "modes": ["raw", "typed"],
  "datatypes": ["bool", "int8", "uint8", "int16", "uint16", "int32", "uint32",
                "int64", "uint64", "float32", "float64", "string"],
  "point_kinds": ["point", "attribute", "stream"],
  "command_verbs": ["write", "write-batch", "set-config", "define-device", "remove-device"],
  "features": ["polling", "subscribe", "bulk_read", "backfill", "management"],
  "subscribe": true
}
```

`write` is declared by the module; the runtime adds `write-batch` and the management verbs. A
point's kind follows from its address: `point` (a `tag`, or a `path` without `|`), `attribute`
(a `path` with `|`), `stream` (a `web_id`).

## 3. Configuration

### 3.1 `connection`

Every key except `updateOption`, `bufferOption` and the limits can be overridden per device in
`protocol_address`.

| Field | Default | Notes |
| --- | --- | --- |
| `auth` | `{ method = "none" }` | Table below. |
| `tls` | `{}` | `ca_file` (PEM bundle added to the system roots), `insecure_skip_verify` (default `false`; logged at `warn` on every connect). |
| `request_timeout` | `"10s"` | Per HTTP request. |
| `connect_timeout` | `"5s"` | TCP + TLS setup. |
| `resolve_retry` | `"5m"` | How often an unresolved point is looked up again. |
| `max_web_ids_per_request` | `50` | WebIds per `streamsets/*` request. |
| `max_url_bytes` | `8192` | Longest request URL; a chunk is cut earlier when its URL would be longer. |
| `max_concurrent_requests` | `4` | Requests of one device in flight. |
| `update_option` | `"Replace"` | `updateOption` of writes: `Replace`, `Insert`, `NoReplace`, `ReplaceOnly`, `InsertNoCompression`. |
| `buffer_option` | `"BufferIfPossible"` | `bufferOption` of writes: `DoNotBuffer`, `BufferIfPossible`, `Buffer`. |
| `state_dir` | `"/var/lib/tedge-dot/aveva-pi"` | Where the delivery cursors are kept (§5.4). |
| `backfill` | see below | `false` disables catch-up. |
| `channel` | see below | Channels settings. |

`auth`:

| `method` | Keys | Notes |
| --- | --- | --- |
| `none` | | No `Authorization` header. |
| `basic` | `username`, `password_file` | The file's content, trailing newlines stripped. |
| `bearer` | `token_file` | Sent as `Authorization: Bearer`. Re-read once after a 401 before the credentials count as rejected, so an external refresher can rotate it. |
| `certificate` | `cert_file`, `key_file` | PEM client certificate and key (mutual TLS). |

An inline `password` or `token` is a configuration error naming the `_file` key.

`backfill`: `max_age` (`"1h"`), `max_events` per point (`10000`), `page_size` (`1000`, the
`maxCount` of a `recorded` page), `live_buffer` per device (`10000`), `cursor_flush` (`"10s"`).

`channel`: `heartbeat_rate` (`5`, the `heartbeatRate` sent), `polling_interval` (`"1s"`, the
server's `ChannelPollingInterval`, which the connector cannot read), `include_initial_values`
(`true`).

**Local-only settings** (contract §3.4): `auth.password_file`, `auth.token_file`,
`auth.cert_file`, `auth.key_file`, `tls.ca_file`, `tls.insecure_skip_verify` and `state_dir`, at
connection or device level. A management command may not add, change or remove them.

### 3.2 `device.protocol_address`

| Field | Required | Notes |
| --- | --- | --- |
| `base_url` | yes | The PI Web API root, `https://host/piwebapi`. `http://` is allowed only without authentication (any `auth.method` but `none` is refused at load: credentials never go over plain HTTP), and is logged at `warn`. |
| `data_server` | for `tag` points | The PI Data Archive that bare tag names live in. |
| `af_database` | no | `\\<af server>\<database>`, the prefix of relative attribute paths. |
| `delivery` | no | `"channel"` (default), `"recorded"` or `"snapshot"` (§5). |
| any `[connection]` key | no | Device override. |

Several devices may share one `base_url`; they share one HTTP client and connection pool per
`(base_url, auth)`.

### 3.3 `point.address`

Exactly one key:

| Address | Kind | Resolved with |
| --- | --- | --- |
| `{ tag = "SINUSOID" }` | `point` | `points?path=\\<data_server>\SINUSOID` |
| `{ path = '\\PISRV01\SINUSOID' }` | `point` | `points?path=` |
| `{ path = '\\AF\Plant\Line1\Pump01\|Discharge Pressure' }` | `attribute` | `attributes?path=` |
| `{ path = 'Line1\Pump01\|Discharge Pressure' }` | `attribute` | `attributes?path=<af_database>\…` |
| `{ web_id = "F1DP…" }` | `stream` | not looked up; WebIds are opaque |

TOML literal strings (`'…'`) keep the backslashes as written.

```toml
[connector]
protocol     = "aveva-pi"
service_name = "tedge-dot-aveva-pi"
poll_interval = "10s"
output       = "measurements"

[connection]
auth = { method = "basic", username = "piuser", password_file = "/etc/tedge/plugins/ot/secrets/aveva-pi.password" }
tls  = { ca_file = "/etc/tedge/plugins/ot/certs/aveva-pi-ca.pem" }

[[device]]
name = "pi-plant1"
protocol_address = { base_url = "https://pi.example.com/piwebapi", data_server = "PISRV01" }

  [[device.point]]
  id = "reactor_temp"
  datatype = "float64"
  address = { tag = "REACTOR01.TEMP" }

  [[device.point]]
  id = "pump_state"
  datatype = "string"
  access = "read_write"
  address = { path = '\\AF\Plant\Line1\Pump01|State' }
```

## 4. Decoding a PI value (typed mode)

A PI value is JSON: `{"Timestamp", "Value", "UnitsAbbreviation", "Good", "Questionable",
"Substituted", "Annotated"}`. One pure function, `decode(value, point_type, datatype, mode)`,
turns it into the sample's value, quality, error and `addr` flags in both builds, checked against
the golden vectors.

| `Value` | `datatype` | Result |
| --- | --- | --- |
| number | integer type | the number, if it is integral and in range; else `bad` (`"<value> does not fit <datatype>"`) |
| number | `float32` / `float64` | the number (`float32` rounded) |
| number | `bool` | `0` → `false`, `1` → `true`; else `bad` |
| number | `string` | the number's JSON text |
| string | `string` | the string |
| string | numeric / `bool` | `bad` (`"expected <datatype>, got a string"`) |
| boolean | `bool` | the boolean |
| `{"Name","Value","IsSystem": false}` (digital) | `string` | `Name` |
| same | integer type / `bool` | `Value` (the state code), as for a number |
| same | float | `Value` |
| `{…,"IsSystem": true}` | any | `bad`, `error` = `Name` (for example `"I/O Timeout"`) |
| `{"Errors": [...]}` or `null` with an `Errors` entry | any | `bad`, `error` = the messages joined with `"; "` |
| anything else (array, object) | any | `bad` (`"unsupported PI value"`) |

- **Quality.** `Good = false` → `bad` with `error` = the digital state name when the value is
  one, else `"not good"`. `Questionable`, `Substituted` and `Annotated` stay `good` and are listed
  in `addr.flags` (`["questionable", "substituted", "annotated"]`, only those set; the key is
  absent when none is). The flags are read from JSON booleans or the strings `"true"` / `"false"`.
- **Raw mode.** The raw bytes are the canonical JSON text of `Value`, whatever its type; quality
  follows the rules above.
- **Canonical text.** Compact JSON, object keys sorted, strings escaped as JSON. A number with no
  fraction and a magnitude below 2^53 is written as an integer (`7`, not `7.0`); any other number
  in its shortest round-trip digits, in exponent form (`1e+300`, `5e-05`) when its decimal exponent
  is below −4 or at least 16, as Python's `repr` writes it. Error texts and numbers converted to
  `string` use the same form.
- **Units.** A point without `unit` takes the PI unit: the point's `EngineeringUnits` or the
  attribute's `DefaultUnitsNameAbbreviation`, else the value's `UnitsAbbreviation`, when non-empty.

Default datatypes (used by the migration script, §9; the configuration must still name one):

| PI point type | `datatype` |
| --- | --- |
| Float16, Float32 | `float32` |
| Float64 | `float64` |
| Int16 / Int32 | `int16` / `int32` |
| Digital | `int32`: the state code, a measurement series. `string` gives the name from PI's state set instead, which is no measurement; configure both points for both |
| String, Timestamp | `string` |
| Blob | raw mode only |

## 5. Read flow

`protocol_address.delivery` decides how a device's readable points are delivered:

| `delivery` | Pushed points | `read_points` |
| --- | --- | --- |
| `"channel"` | Channels WebSocket, with backfill | `streamsets/value` |
| `"recorded"` | `streamsets/recorded` since the cursor, every `sampling_interval` | `streamsets/value` |
| `"snapshot"` | none (`pushes_point` is false) | `streamsets/value` |

Points with `subscribe = false` are always polled.

### 5.1 Snapshot reads

All requested points of one call go into `streamsets/value` requests, chunked (§3.1). Every
sample of the call carries the call's read time as `ts` (contract §5.4); the PI timestamp is in
`addr.timestamp`. `addr` also carries `web_id`, `path` and `point_type`. An unresolved point is
`bad` with `"not found: <path>"`. A per-item `Errors` entry is a `bad` sample for that point only.
A 200 response with a top-level `WebException` is a failed request.

### 5.2 Channels

One WebSocket per chunk of the device's pushed WebIds, opened with
`includeInitialValues = channel.include_initial_values` and `heartbeatRate`. The
`Authorization` header is sent on the upgrade request. A message has the shape of `GetEnd`: the
streams that changed, each with its events. The events of one stream are sorted by timestamp
before delivery; an event older than the point's cursor (a history edit) is dropped with a
`debug` log. Pushed samples carry the PI timestamp as `source_ts`.

- **Heartbeat.** After `heartbeat_rate` server polling intervals without data, the server sends
  an empty message: an empty text frame and a message with an empty `Items` both count. The
  heartbeat period is `heartbeat_rate × polling_interval`. Channels do not answer WebSocket
  pings, so none are sent.
- **Errors.** `{"Errors": [...]}` on the channel (a data source behind PI Web API failed) is logged
  at `warn` and the channel is kept: the link reports the PI Web API connection, which still works,
  and degrading it would make the runtime reconnect. The affected points' values arrive as system
  states (`bad` samples) from PI itself.
- **Fallback.** An upgrade answered with any status other than 101, while the device's REST
  requests succeed, switches the device to `recorded` until the next reconnect, with one `warn`
  naming the status. A transport error during the upgrade is a link failure.

### 5.3 Recorded polling

Every `sampling_interval`, `streamsets/recorded` from each point's cursor to `*` with
`maxCount = backfill.page_size` and `boundaryType = Inside`. There is no continuation link: a
full page is followed by a request that starts at its last timestamp, and dedup (§5.4) drops the
repeated events. A page whose events all share one timestamp is requested again with twice the
`maxCount`, up to 150000 (the server's default `MaxReturnedItemsPerCall`).

### 5.4 Cursor and backfill

Each pushed point has a **cursor**: the timestamp of the last event handed to the runtime and
the values delivered at that timestamp (Rust: accepted by the sample channel; C: drained from the push ring).

1. `subscribe` opens the channel (or starts the recorded loop) and returns. The catch-up runs in
   the device's task (Rust) or worker thread (C), never inside `subscribe`, which is bounded by
   `operation_timeout`.
2. Live events are buffered while the catch-up reads `recorded` from
   `max(cursor, now − backfill.max_age)` in pages, up to `backfill.max_events` per point. The
   skipped part of a longer gap is logged once at `warn` with the point and the range.
3. Archived events are delivered in time order with `replayed = true` (contract §5.3 rule 7),
   then the buffered live events.
4. **Dedup.** An event is delivered only if its timestamp is after the cursor's, or equal to it
   with a value not yet delivered at that timestamp (PI can archive several events at one time).
5. **Backpressure.** The next page is requested only when the runtime has taken the previous one
   (Rust: the bounded sample channel; C: the ring is below half its bound). The C ring never
   drops during a catch-up.
6. **Live buffer.** When more than `backfill.live_buffer` live events arrive during a catch-up,
   the channel is closed, the catch-up continues with `recorded` until it has caught up, and the
   channel is reopened.
7. A point without a cursor starts live (with `include_initial_values`, one start value).

Cursors are kept in `<state_dir>/<service_name>/<device>.json`:

```json
{ "version": 1, "points": { "reactor_temp": { "web_id": "F1DP…", "ts": "2026-11-02T08:00:00.123Z", "values": [81.5] } } }
```

The file is written by atomic rename at most every `cursor_flush` and on disconnect. A missing or
unreadable file means no cursors (a `warn` for an unreadable one). A cursor whose `web_id` no
longer matches is dropped. A point no longer configured is dropped on the next write.

### 5.5 Reporting policy (`report`)

The runtime applies `report`. Pushed points can be read on demand (`streamsets/value`), so a
heartbeat (`max_interval`) applies; it carries the read time. Replayed samples run the policy on
source time (contract §5.3 rule 7). The packaged config shows `report = { max_interval = "30m" }`
as a comment only, and recommends `batch = { group_by = "time" }` (the default) for measurement
output, which never moves a backfilled time.

### 5.6 Value mapping (`map`)

Digital states are the common case: with `datatype = "int32"` the code arrives and a `map` can
label it; with `datatype = "string"` PI's own state name arrives and no map is needed.

## 6. Write flow

`execute("write")` on a point with `access` `write` or `read_write`:

1. Unresolved point → `failed`, `"not found: <path>"`.
2. Encode the value (after the SDK reversed `map` and `transform`) for the point's datatype: a
   number, string or boolean. For a digital point, a state name (string) or code (integer) is
   looked up in the cached digital set and sent as
   `{"Name": <name>, "Value": <code>, "IsSystem": false}`; a state the set lacks fails without a
   request (`"<value> is not a state of <set>"`).
3. `POST streams/{webId}/value?updateOption=<update_option>&bufferOption=<buffer_option>` with
   `{"Value": …}` (no `Timestamp`: server time) and `X-Requested-With: XMLHttpRequest`.
4. 202 or 204 → `successful`. Any other answer, a timeout or a transport error → `failed`, with
   the status and PI's `Errors` (or the error) in the reason. A write never changes the link
   state; the device's reads do. Never retried.

## 7. Status and liveness

| Situation | Link |
| --- | --- |
| Base URL answers and the credentials are accepted | `connected` |
| Some points unresolved | `connected` (those points are `bad`) |
| 5xx, timeout, TLS or DNS failure, WebSocket closed | `degraded` → reconnect with backoff |
| No channel message or heartbeat for 3 heartbeat periods (3 `sampling_interval`s in recorded mode) | `degraded` → reconnect |
| 401/403 on the base URL | `disconnected`, reason `"unauthorized"` |

The `reason` names the HTTP status or the transport error, including the TLS cause (for example
`"certificate has no subject alternative name"`). `connect` checks the base URL with
`GET {base_url}/` and the credentials, then resolves the points.

**C threading.** Snapshot reads and writes run on the runtime thread; one `read_points` call runs
its chunks in parallel on a curl multi handle, so it takes at most one `request_timeout`. All
push traffic, including backfill, runs on one worker thread per subscribed device, which feeds a
growable ring (the OPC UA bounds) drained by `drain_subscriptions`. A configuration's devices are
served one at a time, so a site with several PI Web API servers uses one configuration file per
server.

**Build differences** (listed in `impl/c/README.md`): in the C build `tls.ca_file` replaces the
system trust store instead of adding to it; transport and TLS error texts are curl's and mbedTLS's
wording; a refused WebSocket upgrade is logged with its status but without PI's `Errors` text; the
PI unit is set once as the point's unit (the C SDK has no per-sample unit); strings, `raw` and
`error` are truncated to the C SDK's buffer sizes.

## 8. Acceptance vectors

[`decode-vectors.json`](../../connectors/aveva-pi/conformance/decode-vectors.json) holds the decode
rules of §4 as `{ name, point_type, datatype, mode, value, expect }` entries, generated by
`connectors/aveva-pi/conformance/tools/gen_decode_vectors.py`. Both builds run every vector.
`expect` is `{ quality, value?, raw?, error?, flags? }`. The e2e suite
(`connectors/aveva-pi/tests/aveva_pi_e2e.robot`) covers the read, push, backfill, write and
status scenarios of the openspec change against the simulator.

## 9. Verified PI Web API behaviour

Checked against the PI Web API reference and admin guide (docs.aveva.com) in task 1.1 of the
change. "Community" means reported on PI Square or the AVEVA feedback site, not documented.

| Behaviour | Source | Status |
| --- | --- | --- |
| `heartbeatRate` is a count of server polling intervals (`ChannelPollingInterval`, default 1000 ms) after which an empty message is sent; ≤ 0 means none (the default) | reference `topics/channels.html`, `controllers/stream/actions/getchannel.html`; admin guide page 1023111 | documented |
| The heartbeat message's exact JSON | — | not documented: both an empty frame and empty `Items` are accepted |
| Channels since 2017 R2; no RFC 6455 ping/pong | AVEVA feedback PIDEVTECH-I-105 | community (AVEVA answer) |
| Channel message = `GetEnd` shape; values not guaranteed in time order; edits `Substituted = true`; deletes `Good = false` "No Data"; source failure `{"Errors": [...]}` | reference `topics/channels.html` | documented |
| No setting disables Channels; no documented status for a refused upgrade | admin guide page 1023116 | documented (absence) |
| Basic/Bearer on the upgrade work for non-browser clients | admin guide page 1023111; feedback PIDEVTECH-I-90 | community; verify Bearer on a real server |
| `recorded`: `maxCount` default 1000; server cap `MaxReturnedItemsPerCall` 150000 (exceeded → 400); no continuation link; `boundaryType` Inside/Outside/Interpolated | reference `controllers/streamset/actions/getrecordedadhoc.html`, `topics/boundary-type.html`; admin guide pages 1023030, 1023116 | documented |
| Several events at one timestamp in a `recorded` page | — | not documented: dedup and the doubled `maxCount` cover it |
| `streamsets/value` shape and `selectedFields` (`Items.WebId;Items.Value.Timestamp`) | reference `topics/selected-fields.html` | documented |
| Flags are booleans (strings in some examples); a value may carry `Errors`; a 200 may carry `WebException` | reference `topics/error-handling.html`, `topics/value-types.html` | documented |
| Write answers 202 (buffered) or 204 (applied); `updateOption`/`bufferOption` values and defaults | reference `controllers/stream/actions/updatevalue.html` | documented |
| Digital write body `{"Name","Value","IsSystem"}` | PI Square "PI Web API query examples" | community |
| `X-Requested-With` needed on non-GET while `EnableCSRFDefense` (default on) | admin guide page 1023034 | documented |
| Request line + headers ≤ 16 KB (HTTP.sys `MaxRequestBytes`); longer → 400 | Microsoft HTTP.sys registry documentation | documented (HTTP.sys), inferred for PI |
| Lookup of a missing path: no documented status (404 seen in clients; 410 on the stream controller; in-value `Errors` on stream sets) | reference `topics/error-handling.html` | partly documented |
| WebIds are opaque and persistent; `F1DP…` PI point, `F1Ab…` AF attribute | reference `topics/webid.html`, `topics/webid-encoded-datatypes.html` | documented |
| System state codes: I/O Timeout 246, No Data 248, Pt Created 253, Shutdown 254, Bad Input 255, Comm Fail 313 | AF SDK `AFSystemStateCode` | documented |

## 10. Implementation notes

- Keep three layers per build: the HTTP client (`client`), the pure decoder (`value`) and the
  `Connector` glue. The decoder has no I/O and is fuzzed (Rust `aveva_pi_value`).
- Resolve every WebId on connect; never look a point up per poll.
- Never log a secret: not the `Authorization` header, not a file's content. Link-status `info` is
  `{ "base_url", "delivery" }`.
- C build: libcurl 8.23.0 from `impl/c/cmake/curl.cmake` on the shared in-tree mbedTLS 3.6.7
  (`impl/c/cmake/mbedtls.cmake`), HTTP/HTTPS/WS/WSS only. The `aveva-pi-curl` ctest checks the
  feature set. The zig arm64 cross build keeps the glibc 2.17 floor; a test binary with libcurl
  and mbedTLS is 694 KB (MinSizeRel, arm64) and needs only `libc.so.6`. No CA bundle path is
  compiled in: the module passes `tls.ca_file`, else the first of `/etc/ssl/certs/ca-certificates.crt`,
  `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem` that exists.
- The simulator (`connectors/aveva-pi/sim/`) implements the documented behaviour of §9 and the
  fault injection the e2e suite needs; it is our own code, not a recording of PI.
