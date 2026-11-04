## Context

tedge-dot connectors so far speak fieldbus or OT protocols (Modbus, OPC UA, CAN, CANopen,
PROFIBUS, SNMP). The AVEVA PI Web API is a REST/JSON API over HTTPS in front of a PI Data Archive
and a PI Asset Framework (AF) server. Every value it returns carries a timestamp and quality
flags, because PI is a historian: the latest value of a tag may be minutes or hours old, and the
archive holds every event since.

The community plugin this replaces (MIT, Python) does per poll and per tag
`GET points?nameFilter=<tag>` followed by `GET <RecordedData link>attime?time=<now>`, with
`verify=False`. Batching to stay under the 16 KB Cumulocity limit is done by a hand-kept ledger
file. tedge-dot's measurement output (contract §5.5) already does this batching.

Both builds must ship the connector (Rust/C parity is enforced by running the same e2e,
conformance and cloud suites against each). The C runtime is one thread per configuration with a
blocking poll loop. It receives pushed values through `subscribe_device()` and a non-blocking
`drain_subscriptions()` per tick, and it already fetches mbedTLS 3.6 statically for OPC UA.

PI Web API endpoints this design uses (PI Web API 2019 SP1 and later):

| Purpose | Request |
| --- | --- |
| Resolve a PI point | `GET points?path=\\<server>\<tag>&selectedFields=WebId;Name;PointType;EngineeringUnits;DigitalSetName` |
| Resolve an AF attribute | `GET attributes?path=\\<af>\<db>\<element>|<attribute>&selectedFields=WebId;Name;Type;DefaultUnitsNameAbbreviation` |
| Resolve a data server | `GET dataservers?path=\\<server>` |
| Digital state set | `GET dataservers/{webId}/enumerationsets?...` / `GET enumerationsets/{webId}/values` |
| Current values | `GET streamsets/value?webId=..&webId=..&selectedFields=Items.WebId;Items.Value` |
| Archived events | `GET streamsets/recorded?webId=..&startTime=..&endTime=*&maxCount=..&boundaryType=Inside` |
| Push | WebSocket `streamsets/channel?webId=..&includeInitialValues=..&heartbeatRate=..` |

Behaviour checked against the PI Web API reference and admin guide (docs.aveva.com, task 1.1);
the connector spec records each with its source. The points that shaped this design:

- `heartbeatRate` counts the server's channel polling intervals (`ChannelPollingInterval`,
  default 1000 ms): after that many intervals without data, the server sends an empty message.
  Channels exist since 2017 R2 and do not answer RFC 6455 ping frames.
- A channel message has the shape of `GetEnd`: only the streams that changed, values not
  guaranteed to be in time order. Edits arrive with `Substituted = true`, deletes as `Good = false`
  with the state "No Data", and a data-source failure as `{"Errors": [...]}`.
- There is no server setting that disables Channels. A refused upgrade comes from a proxy or an
  older server, with no documented status code.
- `recorded` returns at most `maxCount` events (default 1000, server cap
  `MaxReturnedItemsPerCall` = 150000, exceeded → 400) with **no continuation link**. A client
  pages by restarting at the last timestamp. Events that share a timestamp are not documented.
- Flags (`Good`, `Questionable`, ...) are JSON booleans in the reference and strings in some
  examples. A value can be `{"Errors": [...]}` with `Value = null`, and a 200 response can carry a
  top-level `WebException`.
- `POST streams/{webId}/value` answers 202 (buffered) or 204 (applied); 400 and 409 are errors.
  CSRF defence (`EnableCSRFDefense`, on by default) needs `X-Requested-With` on every non-GET.
  The digital write body is only community-documented: the full `{Name, Value, IsSystem}` object.
- PI Web API runs on HTTP.sys: the request line plus headers may be 16 KB at most, and longer
  requests get **400**, not 414.
- A missing path is not documented as 404; clients see 404 on lookups, 410 on the stream
  controller, and an in-value `Errors` entry on stream sets.
| Write | `POST streams/{webId}/value?updateOption=..&bufferOption=..` with `{"Timestamp":..,"Value":..}` |

## Goals / Non-Goals

**Goals:**

- One connector, two builds, same behaviour, proven by the same suites.
- No per-poll lookups: WebIds are resolved once per connection, and one request reads many points.
- No silent data loss on a PI or network outage for pushed points.
- Secure by default: verified TLS, secrets in local files that management commands cannot change.
- Serve as the reference example for HTTP/REST connectors: clean separation of the HTTP client,
  the JSON value decoder and the `Connector` glue, plus a short written guide.

**Non-Goals:**

- Kerberos/NTLM (Windows integrated) authentication. It needs GSSAPI/SSPI and a domain-joined
  host. Sites that use it can enable Basic or OIDC on PI Web API. Recorded as a follow-up, and as
  an adoption risk (see Risks).
- Discovery or browsing (turning AF element templates into point libraries). Points stay listed
  in the configuration file, as for every other connector.
- Writing PI history (bulk `recorded` writes) or annotations. A write sets one value.
- Static AF attribute values (no PI Point data reference). They have no stream to write or
  subscribe to.
- Event frames and AF analyses.
- Edits and deletions of archived values that are older than the delivery cursor. Channels send
  them, but republishing corrected history is out of scope; they are dropped with a `debug` log.
- The community plugin's `tag_batches.json` ledger. Measurement output (§5.5) replaces it.

## Decisions

### D1. Protocol id `aveva-pi`, device = PI Web API endpoint

`protocol = "aveva-pi"`, service `tedge-dot-aveva-pi`, topics `te/device/<d>/ot/aveva-pi/...`. The id names the vendor because a bare `pi` is ambiguous (Raspberry Pi, the PI controller,
the number), and it matches how the product and the community plugin are known. A `[[device]]` is one
PI Web API base URL plus optional `data_server` (default PI Data Archive for bare tag names) and
`af_database` (default prefix for relative attribute paths). One PI system feeding several
thin-edge child devices is expressed as several `[[device]]` entries with the same `base_url`; they
share one HTTP client and connection pool per `(base_url, auth)` pair.

Alternative: one device per PI server, points split by `meta`. Rejected because thin-edge devices
are the unit of grouping in the cloud, and the contract already lets several devices share a
connection.

### D2. Point address: one of `tag`, `path` or `web_id`

```toml
address = { tag = "SINUSOID" }                                  # in the device's data_server
address = { path = '\\PISRV01\SINUSOID' }                        # full PI point path
address = { path = '\\AFSRV\Plant\Line1\Pump01|Discharge Pressure' }  # AF attribute ('|')
address = { web_id = "F1DPmN..." }                               # pre-resolved stream
```

Exactly one key. A `path` containing `|` is an AF attribute, else a PI point. `tag` needs the
device's `data_server`. Resolution runs on connect, in one pass, with up to `resolve_concurrency`
requests in flight. A point that cannot be resolved (any 4xx other than 401/403, or an empty result; the
reference documents no status for a missing path) gets a `bad` sample `"not found: <path>"` on every read and is retried every `resolve_retry` (default `5m`). It does
not take the link down. The resolved WebId, PI point type, digital set and engineering unit are
cached per connection and echoed in `addr` (`web_id`, `path`, `point_type`).

Alternative: resolve lazily on first read. Rejected because it spreads lookups into the first
poll cycles and makes the first batched read partial.

### D3. Three delivery modes per device; Channels by default

`protocol_address.delivery`:

| Value | Pushed points | `read_points` (heartbeat, CLI, `subscribe = false`) |
| --- | --- | --- |
| `"channel"` (default) | Channels WebSocket, one per device, with backfill | `streamsets/value` |
| `"recorded"` | the module polls `streamsets/recorded` since its cursor every effective `sampling_interval` | `streamsets/value` |
| `"snapshot"` | none; `pushes_point` answers false and the SDK polls | `streamsets/value` |

`capabilities().subscribe = true`. `pushes_point` returns false for `snapshot` devices and for
points the runtime already keeps off push (`subscribe = false`).

`"channel"` falls back to `"recorded"` for that device, with one `warn` log naming the status,
when the WebSocket upgrade is answered with any HTTP status other than 101 while the device's REST
requests succeed (connect has already proven the base URL and the credentials). PI Web API has no
switch that disables Channels, so this is a proxy that blocks WebSockets or an old server. A
transport error during the upgrade is a link failure, not a reason to fall back. It keeps the
fallback until the next reconnect. `"recorded"` is also gap-free on its own, so it is the right
choice behind proxies that block WebSockets.

`"snapshot"` reproduces the community plugin's behaviour (current value, read time) with one
request per device per poll.

Alternative: snapshot polling with source timestamps. Rejected: an unchanged snapshot polled every
few seconds would publish the same old timestamp again and again (duplicate measurements in the
cloud), and events between two polls would be lost.

### D4. Timestamps

- Snapshot reads (`read_points`) use the **read time**, as contract §5.4 requires for polled
  reads. The PI timestamp is echoed as `addr.timestamp`, so a flow can still use it.
- Pushed values (channel, recorded, backfill) carry their **PI timestamp as source time**
  (`source_ts` / `source_ts_ms`).

### D5. Backfill and the delivery cursor

Each device keeps a **cursor** per pushed point: the timestamp of the last event handed to the
runtime, plus that event's value. In Rust "handed" means accepted by the bounded sample channel;
in C it means taken from the push ring by `drain_subscriptions`. It drives three things:

1. **Backfill on (re)subscribe.** `subscribe` (Rust) and `subscribe_device` (C) only open the
   channel, or start the recorded loop, and start the device's push task or worker thread. They
   return as soon as the WebSocket is up. The catch-up runs **in that task or thread**, never inside
   the `subscribe` call: Rust bounds every module call with `operation_timeout` (30 s by default),
   and a catch-up inside `subscribe` would time out on a long outage, count as a transport error
   and restart, so it would never finish. The task buffers what the channel sends, reads
   `streamsets/recorded` from each point's cursor to now in pages of `backfill.page_size`
   (default 1000) events, delivers those in time order marked as **replayed** (D15), then delivers
   the buffered live events, dropping any whose timestamp is not after the cursor. This order
   leaves no gap between catch-up and live data. The live buffer is bounded by
   `backfill.live_buffer` (default 10000 events per device). When it fills, the task closes the
   channel and continues in recorded mode from the cursor until it has caught up, then reopens
   the channel. Nothing is dropped.
2. **Bounds.** Catch-up starts at `max(cursor, now - backfill.max_age)` (default `max_age = "1h"`)
   and stops after `backfill.max_events` events per point (default 10000). The rest is reported
   once at `warn` with the point and the skipped time range. `backfill = false` disables
   catch-up.
3. **Dedup.** An event is delivered only if its timestamp is after the cursor's, or equal to it
   with a value not yet delivered at that timestamp. PI can store several events at one timestamp;
   the cursor keeps every value delivered at its timestamp, so those are kept and the overlap
   between pages and between catch-up and live data is dropped.
4. **Paging.** `recorded` has no continuation link, so the next page starts at the last
   timestamp of the previous one (`boundaryType = Inside`), and dedup drops the repeated events.
   When a whole page shares one timestamp, the module repeats the request with twice the
   `maxCount` (up to 150000, the server's default cap) so paging cannot stall.
5. **Order.** A channel message is not guaranteed to be in time order, so the events of each
   stream in one message are sorted by timestamp before delivery. An event older than the cursor
   (an edit of history, which PI sends with `Substituted = true`) is dropped with a `debug` log:
   history edits are a non-goal.

**Backpressure, not loss.** The catch-up is paced by the consumer. In Rust, `sink.send().await`
on the runtime's bounded channel (256 samples) blocks the task, so the next page is requested only
when the runtime has taken the previous one. In C, the worker waits on a condition variable while
the push ring is above its high-water mark (half of its bound) and requests the next page only
after `drain_subscriptions` has brought it below. The ring's growth bound (the OPC UA bound,
`max(65536, 4 × items)`) is therefore never reached by a catch-up. Live channel messages wait the
same way. If PI closes a channel that is read too slowly, the task reconnects and catches up from
the cursor.

Points with no cursor yet (first start) start live. With `includeInitialValues = true` on the
channel, they get one start value, so a fresh install does not replay an hour of history.

**Persistence.** Cursors are written to `<state_dir>/<service_name>/<device>.json` (default
`state_dir = "/var/lib/tedge-dot/aveva-pi"`) by atomic rename, at most every `cursor_flush` (default
`10s`), and on disconnect. A missing or unreadable file means no cursors (start live) and a `warn`.
A cursor for a point no longer configured is dropped on the next write. The file stores a format
version and the `web_id` per point id, so a point whose address changed starts live.

Why the module and not the SDK: only the module knows which protocol can replay history. The SDK
change this needs is limited to how the reporting policy treats replayed samples (D15).

Alternative: persist cursors in a retained MQTT message. Rejected: it couples startup ordering to
the broker and writes to the broker at event rate.

### D6. Value decoding: one pure function, shared vectors

PI returns `Value` as a JSON number, string, boolean, or for digital points an object
`{"Name","Value","IsSystem"}`, plus `Good`, `Questionable`, `Substituted`, `Annotated` and
`UnitsAbbreviation`. A pure function `decode(pi_value, point_type, datatype, mode) -> Sample parts`
is implemented in both builds and checked against `connectors/aveva-pi/conformance/decode-vectors.json`
(the same vectors in Rust unit tests and C ctest), plus a Rust proptest/fuzz target over arbitrary
JSON.

| PI point type | Default `datatype` | Notes |
| --- | --- | --- |
| Float16 / Float32 | `float32` | |
| Float64 | `float64` | |
| Int16 / Int32 | `int16` / `int32` | |
| Digital | `int32` | `Value.Value` (state code). With `datatype = "string"`: `Value.Name`. |
| String | `string` | |
| Timestamp | `string` | RFC 3339 as returned |
| Blob | — | `raw` mode only |

`datatype` stays mandatory in typed mode, as for every connector. A mismatch the value cannot
satisfy (`"abc"` as `float64`) is a `bad` sample with the reason. `raw` mode publishes the UTF-8
JSON text of `Value` as the raw bytes.

**PI's JSON quirks.** The flags are accepted as booleans or as the strings `"true"` /
`"false"`. A value that is `{"Errors": [...]}` (or has `Value = null` with an `Errors` entry) is a
`bad` sample with the joined messages as the error. A 200 response with a top-level
`WebException` is a failed request.

**Quality.** A system digital state (`IsSystem = true`) or `Good = false` → `bad`, with
`error = "<state name>"` (for example `"I/O Timeout"`). `Questionable` and `Substituted` stay
`good`, and the flags are echoed in `addr.flags` (only when set). The contract has no third
quality for "questionable", and dropping such values would lose data the historian accepted.

**Units.** When the point has no `unit`, the module sets the sample unit from PI
(`EngineeringUnits` / `UnitsAbbreviation`). A configured `unit` always wins.

### D7. Writes

`execute("write")` (and `write-batch`, which the SDK splits into writes) posts
`{"Value": v}` without `Timestamp` (server time), and treats 202 (buffered) and 204 (applied) as
success, to `streams/{webId}/value` with
`updateOption = "Replace"` and `bufferOption = "BufferIfPossible"` (both overridable in
`[connection]`). The header `X-Requested-With: XMLHttpRequest` is always sent, because PI Web API
CSRF defence rejects non-GET requests without it. Digital points accept a state code (number) or
a state name (string). Both are looked up in the point's cached digital set and sent as the full
`{"Name": .., "Value": .., "IsSystem": false}` object, the only form reported to work across
versions. HTTP 4xx → command `failed`
with PI's `Errors[]` text. 5xx or transport errors → `failed` with the status or error; as for
every connector, a write never changes the link state, the device's reads do. Writes are never
retried.

### D8. HTTP stack

**Rust**: `reqwest` with `rustls-tls` (no OpenSSL, so the musl/zig cross-builds stay as they
are), `tokio-tungstenite` with rustls for Channels, `serde_json`. One `reqwest::Client` per
`(base_url, auth)`. TLS: system roots plus optional `ca_file`, `insecure_skip_verify` (default
false, logged at `warn` on every connect), client `cert_file` / `key_file` as `reqwest::Identity`.

**C**: libcurl ≥ 8.11 (stable WebSocket API) built from the release tarball with `FetchContent`,
static, with `--with-mbedtls` on the already-fetched mbedTLS 3.6. Only HTTP, HTTPS, WS and WSS are
enabled. No libssh, nghttp2, brotli, zstd, idn or psl. cJSON for JSON. CMake option
`TDOT_AVEVA_PI_VENDORED_CURL` (default ON) mirrors `TDOT_OPCUA_VENDORED`. This keeps the zig glibc 2.17
floor and adds roughly 0.5 MB.

Alternative for C: a hand-written HTTP/1.1 + WebSocket client on mbedTLS. Rejected: the user's
standing preference is maintained libraries over our own protocol code (as for SNMP and J1939),
and HTTP edge cases (chunked encoding, redirects, proxies, keep-alive) are where hand-written
clients fail.

### D9. C threading model

The C runtime is one thread per configuration file, and it calls modules one device at a time with
blocking calls. A module that blocks delays every other device of the same configuration (the
OPC UA module has the same property). This design keeps the runtime thread for short calls only:

- **Push traffic runs on worker threads.** One worker thread per subscribed device owns that
  device's curl handles: the WebSocket or the recorded-poll loop, and every backfill request. It
  pushes decoded samples into a mutex-protected growable ring with the C OPC UA bounds and waits
  on the ring's high-water mark (D5). `drain_subscriptions` only empties the ring and updates the
  cursors, so it never blocks. It returns -1 once the worker has reported the link down.
  Disconnect signals the worker and joins it. The worker writes the cursor file.
- **Snapshot reads and writes stay on the runtime thread**, because the C SDK's `read_points` and
  `write_point` are synchronous. They are bounded so one call costs at most one
  `request_timeout`, whatever the number of points: the chunks of one `read_points` call run in
  parallel on one curl multi handle. With the default `channel` delivery these calls are rare
  (heartbeats, `subscribe = false` points, CLI reads and writes).
- **One configuration file per PI server** is the documented layout for the C build when a site
  has more than one PI Web API server, so a stalled server delays only its own devices. The
  packaged config says so.

Alternative: move snapshot reads to the worker thread and answer `read_points` from its latest
result. Rejected: the sample would no longer be a read made at the requested time, and heartbeat
reads must be fresh (contract §5.3 rule 4). Non-blocking reads for the whole C runtime are an SDK
change for every module and are out of scope.

The Rust build uses one tokio task per subscribed device, tied to an abort-on-drop handle (SDK §4.2
requirement), and implements `check_subscription` from the task's last-heartbeat time. Rust reads
are async, so a stalled server does not delay other devices.

### D10. Liveness

The channel is opened with `heartbeatRate = channel.heartbeat_rate` (default 5). The server sends
an empty message after that many of its polling intervals without data. The connector cannot read
the server's `ChannelPollingInterval`, so `channel.polling_interval` (default `1s`, the server
default) states it, and the heartbeat period is `heartbeat_rate × polling_interval`. An empty
text frame and a message with an empty `Items` array both count as a heartbeat. Channels do not
answer WebSocket pings, so the connector sends none. `check_subscription` (Rust) and
`drain_subscriptions` (C) fail the device when no message or heartbeat arrived within three
heartbeat periods, or three `sampling_interval`s in recorded mode. An `{"Errors": [...]}` message
on the channel (a data-source failure behind PI Web API) is logged at `warn` and the channel is
kept: degrading the link would make the runtime reconnect, and PI reports the affected points as
system states anyway.
Every HTTP request has `request_timeout` (default `10s`) and `connect_timeout` (default `5s`).

### D11. Request size

`streamsets/*` take WebIds in the query string, and HTTP.sys limits the request line plus
headers to 16 KB (a long Bearer token counts). Requests are chunked at `max_web_ids_per_request`
(default 50) and at `max_url_bytes` (default 8192), whichever is reached first. On a 414, or a
400 without a PI `Errors` body (HTTP.sys's answer to a long request), the module halves that
device's chunk size and logs once. and the chunks of one device are sent
concurrently up to `max_concurrent_requests` (default 4). A device with more points than one chunk
gets one Channel per chunk. `selectedFields` trims every response to the fields decoded.

### D12. Link status

| Situation | Link |
| --- | --- |
| Base URL answers, auth accepted | `connected` |
| Some points unresolved | `connected` (those points are `bad`) |
| 5xx, timeout, TLS or DNS failure, WebSocket closed | `degraded` → reconnect with backoff |
| 401/403 on the base URL | `disconnected`, reason `"unauthorized"` (backoff, no tight loop) |

The link status's `reason` carries the HTTP status or transport error in both builds.

### D13. Configuration shape

```toml
[connector]
protocol     = "aveva-pi"
service_name = "tedge-dot-aveva-pi"
poll_interval = "10s"
output       = "measurements"

[connection]
auth = { method = "basic", username = "piuser", password_file = "/etc/tedge/plugins/ot/secrets/aveva-pi.password" }
# auth = { method = "bearer", token_file = "..." }
# auth = { method = "certificate", cert_file = "...", key_file = "..." }
tls  = { ca_file = "/etc/tedge/plugins/ot/certs/aveva-pi-ca.pem", insecure_skip_verify = false }
request_timeout = "10s"
state_dir = "/var/lib/tedge-dot/aveva-pi"
backfill = { max_age = "1h", max_events = 10000, page_size = 1000, live_buffer = 10000 }
channel  = { heartbeat_rate = 5, polling_interval = "1s" }

[[device]]
name = "pi-plant1"
protocol_address = { base_url = "https://pi.example.com/piwebapi", data_server = "PISRV01", delivery = "channel" }

  [[device.point]]
  id = "reactor_temp"
  datatype = "float64"
  address = { tag = "REACTOR01.TEMP" }
```

`auth`, `tls`, `state_dir`, `backfill` and `channel` are allowed in `[connection]` and overridable in
`protocol_address`. Local-only settings, which management commands refuse: `auth.password_file`,
`auth.token_file`, `auth.cert_file`, `auth.key_file`, `tls.ca_file`, `tls.insecure_skip_verify`,
`state_dir`. A password or token inline in the TOML is rejected, as for SNMP. The token file is
re-read on every 401 before the link is marked unauthorized, so an external refresher can rotate
it.

### D14. Simulator

`connectors/aveva-pi/sim/`: Python, `aiohttp` (Apache-2.0), our own code. It holds an in-memory archive
seeded from `seed.json` (tags of every point type, digital sets including system states, AF
attributes) with generated values. It implements the endpoints in Context with `selectedFields`,
WebIds in PI's format, Basic/Bearer/mTLS (TLS certificates generated at start for the test), CSRF
header checks, and a control API (`/_sim/...`) for fault injection: pause (accept connections, never
answer), 503, close channels, disable Channels, insert archive events at past timestamps, and an
outage window during which archiving continues but the API is down. That last one is the backfill
test.

The `ot-conformance` harness runs the static layers and the capability agreement for `aveva-pi`.
As for SNMP, the simulator lives in Docker, so the behavioural layer is covered by the e2e suite,
which runs both builds against it. An in-process Rust simulator kind was considered and left
out: it would be a second simulator to keep in step with the first.

### D15. Replayed samples and the reporting policy (SDK change, both builds)

Backfilled events arrive in a burst: two hours of history can reach the runtime in seconds. The
time-based keys of the reporting policy (contract §5.3) measure time on the runtime clock, so they
would destroy that history. `min_interval` holds a change and **replaces** the held reading with
each newer one (`ReportState::offer` in `impl/rust/crates/sdk/src/report.rs`, and
`impl/c/sdk/src/report.c`), so a burst collapses to its first and last value. `debounce` waits for
a value to stay unchanged for a wall-clock duration, which a burst never does. `on_change` and
`deadband` only compare values and are unaffected.

The SDK therefore gets a **replayed** flag on a pushed sample (`Sample::replayed: bool` in Rust,
`int replayed` in `tdot_sample_t` in C, default false). A module sets it on history it delivers
after the fact, and only together with a source time. The runtime then:

- **Runs the reporting policy on source time for replayed samples.** The policy's clock for a
  replayed sample is its `source_ts`, so `min_interval` and `debounce` give the same result as
  they would have given live. Over two hours of 1-second history, `min_interval = "10s"` publishes
  about 720 samples, not 2. A held or debounced reading whose interval ends within the replay is
  published at that point of the replay, with its own `ts`.
- **Hands over to the runtime clock** at the first sample that is not replayed. A reading still
  held or debouncing at that moment keeps waiting, with its timer restarted on the runtime clock.
- **Does not read pushed points for a heartbeat** while a device is replaying. The heartbeat
  reference is the last publish, and a replay publishes.
- Keeps rule 1 of §5.3: the first sample after a reconnect is published, replayed or not.

This is the only SDK and contract change: a new rule in contract §5.3, the field in both SDKs,
and new report test vectors (`doc/contract/test-vectors/report/vectors.json`) that both SDKs run.
Any future module that replays history (an OPC UA HistoryRead, a CANopen or SNMP buffer) reuses
it. Replayed samples go into measurement output (§5.5) like any other pushed sample: with the
default `group_by = "time"`, nothing is moved or lost; under `"frame"` a burst may produce more
than 64 layers in one window, which are published early (no loss), and layer times can move by
1 ms steps. The packaged config recommends `"time"` for this connector.

Alternatives: replayed samples bypass `min_interval` and `debounce`. Rejected: a site that set
`min_interval` to limit cloud volume would get every archived event after an outage, which is the
flood the setting exists to prevent. Applying the policy inside the module: rejected, it would
duplicate the policy in a module and diverge from the SDK's rules.

## Risks / Trade-offs

- [The simulator encodes our reading of PI Web API, not the real product] → Keep the
  simulator's responses to fields documented in the PI Web API reference, record real response
  samples from a PI Web API instance into `connectors/aveva-pi/conformance/recorded/` when one is
  available, and run the decode vectors against them. The community plugin's users are the first
  field test.
- [WebSocket proxies or Channels disabled on the server] → automatic fallback to `recorded`
  (D3), and the e2e suite covers the fallback.
- [Long query strings with many WebIds] → chunking (D11), configurable. If a server rejects long
  URLs (HTTP 414), the module halves the chunk size for that device and logs once.
- [Backfill floods the cloud after a long outage] → `max_age` and `max_events` bounds. Measurement
  output batches the catch-up. The default one hour is conservative.
- [Backfilled values and the reporting policy] → time-based keys run on source time for replayed
  samples (D15). This needs an SDK change in both builds, covered by shared report test vectors.
- [A hard crash loses the samples between the module and the publish] → the cursor advances when
  a sample is handed to the runtime (Rust channel, C ring), so a crash can lose at most the
  samples in that buffer (256 in Rust, the ring's content in C). A clean stop drains first. A
  restart replays from the last flushed cursor, so the usual effect of a crash is duplicates,
  which dedup removes, not loss.
- [The C runtime is serial] → a stalled PI server delays other devices of the same configuration
  by up to one `request_timeout` per call (D9). Push traffic is unaffected. Mitigated by one
  configuration file per PI server and covered by an e2e case.
- [Heartbeat reads on pushed points use the read time] → a heartbeat sample carries the device's
  read time between samples carrying PI time. With clock skew between the device and PI, `ts` can
  go backwards by the skew. The docs tell consumers to order by `ts`, not by arrival, and the
  heartbeat is skipped during a replay (D15).
- [Several PI events at one timestamp] → they become several samples, and several Cumulocity
  measurements, with the same time. This is the archive's content, kept on purpose. Measurement
  output under `group_by = "time"` publishes a repeated point as a separate message, so no value is
  overwritten.
- [libcurl adds build surface to the C build and the zig cross-build] → do the static
  curl+mbedTLS cross-build de-risk first in the tasks (as was done for mbedTLS in OPC UA), and
  build with a minimal feature set.
- [Cursor file on flash storage] → writes at most every `cursor_flush`, atomic rename, small file.
- [Clock skew between the device and PI] → catch-up windows use PI timestamps only (cursor to
  `*`). The device clock is used only for `max_age`.
- [Kerberos is the only enabled authentication on many corporate PI Web API servers] → users of
  the community plugin already have Basic enabled. For new sites, the docs explain enabling Basic
  or OIDC (bearer) on PI Web API. Kerberos is the first follow-up if field use asks for it: libcurl
  supports SPNEGO through GSS-API in C, while Rust needs a separate crate, so it is a change of its
  own.
- [rustls and mbedTLS reject old PI certificates] → self-signed IIS certificates often have no
  subject alternative name, and some servers only offer TLS 1.0/1.1. The Python plugin hid this with
  `verify=False`. The connector names the cause in the link status and the log (for example
  `"certificate has no subject alternative name"`), the docs show how to issue a proper
  certificate, and `insecure_skip_verify` stays available, logged at `warn` on each connect. The
  e2e suite includes a certificate without a SAN so the message is tested in both builds.
- [Parts of PI Web API behaviour are undocumented] → task 1.1 checked the reference (see
  Context). Still unverified: the exact heartbeat message, Bearer tokens on the WebSocket upgrade
  (an AVEVA idea to support it was declined; community reports say non-browser clients can send
  it), events that share a timestamp in `recorded` pages, and the digital write body (community
  sources only). The connector accepts every plausible form of each, the simulator implements the
  documented one, and these are the first things to check against a real server.
- [Large tag lists need a `datatype` per point] → the contract makes `datatype` mandatory in typed
  mode, though PI knows each tag's type. A 500-tag migration means 500 datatypes. The migration doc
  ships a script that queries PI Web API for each tag's `PointType` and writes the points with the
  default datatypes of D6. Letting a module supply a point's datatype is an open question.
- [A hyphen in the protocol id] → no code checked so far rejects `aveva-pi` (MQTT topics, the
  flows, the registration flow's configurable twin fragment), but every use is not yet checked.
  Task 3.1 checks the C identifiers, CMake option names, CI matrix keys and schema file names.

## Migration Plan

New connector; nothing to migrate in tedge-dot. For community-plugin users, the docs give a
mapping: `pi_config.json` → `[connection]` + `protocol_address`, `datapoints.json` → points (a
`tedge-dot` helper is out of scope; a 10-line `jq` example is in the doc), the batch ledger →
`output = "measurements"`. Measurement series names differ (point ids instead of `TAG_NAME` with
`.` replaced), and the doc says so. Rollback is uninstalling or disabling the config file.

## Open Questions

- PI Web API version floor: Channels and `selectedFields` exist since 2015; `streamsets/recorded`
  paging with `maxCount` behaves as described since 2017. We state 2019 SP1+ as tested and supported
  until we have a real instance to test older versions.
- Whether to add OIDC client-credentials token fetching (instead of a token file) later. Not in
  this change.
- Whether a module may supply a point's datatype from the server (PI's `PointType`), which would be
  a contract change for typed mode. Not in this change; the migration script covers the common
  case.
- Kerberos (SPNEGO) authentication, as above.
