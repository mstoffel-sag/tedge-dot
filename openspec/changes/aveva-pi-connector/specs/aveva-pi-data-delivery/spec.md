## ADDED Requirements

### Requirement: Batched snapshot reads
`read_points` SHALL read all requested resolved points of a device with `streamsets/value`
requests (chunked as `aveva-pi-web-api-connection` requires), not one request per point. Every sample
of one call SHALL share the call's read time as `ts`. In the C build, the chunks of one call SHALL
be requested in parallel, so one call takes at most one `request_timeout`. Each sample's `addr` SHALL carry `web_id`,
`path`, `point_type` and the PI value's `timestamp`.

#### Scenario: One request per poll
- **WHEN** a `delivery = "snapshot"` device with 10 resolved points is polled
- **THEN** the simulator receives one `streamsets/value` request and the connector publishes 10 samples with equal `ts`

#### Scenario: One call costs one timeout in the C build
- **WHEN** a C build device reads 200 points (4 chunks) and the simulator pauses every request
- **THEN** the `read_points` call returns `bad` samples after one `request_timeout`, not four

#### Scenario: PI timestamp echoed
- **WHEN** a tag's snapshot value is archived at `2026-11-02T08:00:00Z` and polled at `2026-11-02T09:00:00Z`
- **THEN** the sample `ts` is the read time and `addr.timestamp` is `2026-11-02T08:00:00Z`

### Requirement: Delivery modes
`protocol_address.delivery` SHALL accept `channel` (default), `recorded` and `snapshot`. Under
`snapshot`, no point SHALL be pushed. Under `channel` and `recorded`, every readable point with
`subscribe` not false SHALL be pushed, and the other points SHALL be polled. Pushed samples SHALL
carry the PI event timestamp as source time.

#### Scenario: Snapshot device
- **WHEN** a device sets `delivery = "snapshot"`
- **THEN** the connector opens no WebSocket and its points follow `poll_interval`

#### Scenario: Opt-out point
- **WHEN** a point on a `channel` device sets `subscribe = false`
- **THEN** it is polled with `streamsets/value` and is not part of the channel

### Requirement: Channels push
Under `delivery = "channel"`, the connector SHALL open `streamsets/channel` WebSockets for the
device's pushed points (one per chunk of WebIds), with `heartbeatRate` set. It SHALL publish one
sample for every event received, with each stream's events of one message in time order. A
device whose WebSocket upgrade is answered with an HTTP status other than 101, while its REST
requests succeed, SHALL fall back to `recorded` delivery until its next reconnect, and SHALL log
one `warn` naming the status. An event older than the point's cursor SHALL be dropped.

#### Scenario: Value change pushed
- **WHEN** the simulator archives a new event for a subscribed tag
- **THEN** the connector publishes a sample with that event's value and timestamp within one second

#### Scenario: WebSocket refused
- **WHEN** the simulator answers the WebSocket upgrade with 403
- **THEN** the device's pushed points keep publishing via `streamsets/recorded`, and one `warn` is logged

### Requirement: Recorded polling
Under `delivery = "recorded"` (or after a fallback), the connector SHALL request
`streamsets/recorded` for the device's pushed points from their cursor to `*` every effective
`sampling_interval`, and SHALL publish every returned event after the cursor, so no event
archived between two polls is lost. A full page SHALL be followed by a request that starts at its
last timestamp, and a page whose events all share one timestamp SHALL be requested again with
twice the `maxCount`, up to 150000.

#### Scenario: More events than one page
- **WHEN** 2500 events were archived since the cursor and `backfill.page_size = 1000`
- **THEN** the connector publishes all 2500 events once each, in time order

#### Scenario: A page of one timestamp
- **WHEN** 1200 events share one timestamp and `backfill.page_size = 1000`
- **THEN** the connector publishes all 1200 events and moves on to later events

#### Scenario: Several events between polls
- **WHEN** the simulator archives 5 events for a tag within one sampling interval
- **THEN** the connector publishes 5 samples with the 5 event timestamps

### Requirement: Push liveness
A pushed device SHALL be reported failed (Rust `check_subscription`, C `drain_subscriptions`)
when no message or heartbeat arrived for three heartbeat periods (channel) or three sampling
intervals (recorded). The heartbeat period SHALL be `channel.heartbeat_rate` (default 5) times
`channel.polling_interval` (default `1s`), and the channel SHALL be opened with that
`heartbeatRate`. An empty text frame and a message with an empty `Items` array SHALL both count as
a heartbeat. The runtime then reconnects and re-subscribes.

#### Scenario: Empty heartbeat messages keep the channel up
- **WHEN** a subscribed tag does not change for ten heartbeat periods and the simulator sends empty heartbeat messages
- **THEN** the link stays `connected` and the channel is not reopened

#### Scenario: Data-source error on the channel
- **WHEN** the simulator sends `{"Errors":["[-10723] PINET: No Connection."]}` on the channel
- **THEN** the connector logs it at `warn`, the channel stays open, and later events are still published

#### Scenario: Silent channel
- **WHEN** the simulator keeps the WebSocket open but stops sending messages and heartbeats
- **THEN** the link becomes `degraded` within three heartbeat periods and the channel is reopened

### Requirement: Outage backfill
On each subscribe of a pushed device, for every point with a cursor, the connector SHALL read the
archived events from `max(cursor, now - backfill.max_age)` with `streamsets/recorded`, in pages of
`backfill.page_size`. The catch-up SHALL run in the device's push task (Rust) or worker thread (C)
after the subscribe call has returned, never inside it. The connector SHALL publish the archived
events in time order, marked as replayed, before any live event, and it SHALL NOT publish a live
event before the point's cursor, nor one at the cursor's timestamp whose value was already
delivered at that timestamp. At
most `backfill.max_events` events per point SHALL be replayed. Any skipped range SHALL be logged
once at `warn` with the point and the range. `backfill = false` SHALL disable catch-up. A point
without a cursor SHALL start live.

#### Scenario: Gap filled after outage
- **WHEN** the simulator's API is down for 2 minutes while it keeps archiving one event per 10 seconds for a subscribed tag, and then comes back
- **THEN** the connector publishes the 12 archived events with their original timestamps, followed by live events, with no duplicate timestamps

#### Scenario: Long catch-up does not restart the subscription
- **WHEN** a catch-up takes longer than `operation_timeout`
- **THEN** the subscription stays up, the link stays `connected`, and the catch-up completes once

#### Scenario: Backfill under a min_interval policy
- **WHEN** the archive holds one event per second for 10 minutes of outage and the point has `report = { min_interval = "10s" }`
- **THEN** the connector publishes about 60 replayed samples spread over the 10 minutes of source time, not 2

#### Scenario: Outage longer than max age
- **WHEN** the API was down for 3 hours and `backfill.max_age = "1h"`
- **THEN** only events from the last hour are replayed, and one `warn` names the skipped range

#### Scenario: First start
- **WHEN** a device subscribes for the first time and has no cursor file
- **THEN** no history is replayed, and each point publishes its current value once

### Requirement: Backfill is paced by the consumer
The connector SHALL request the next page of a catch-up only when the runtime has taken the
previous samples: in Rust by awaiting the bounded sample channel, in C by waiting while the push
ring is above half its bound. A catch-up SHALL NOT make the C push ring drop samples. Live events
that arrive during a catch-up SHALL be buffered up to `backfill.live_buffer` events per device;
when the buffer is full, the connector SHALL close the channel and continue from the cursor with
`streamsets/recorded` until it has caught up, then reopen the channel, without losing an event.

#### Scenario: Large catch-up in the C build
- **WHEN** a C build device with 50 pushed points catches up 10000 events each
- **THEN** all 500000 events are published and the push ring reports no dropped samples

#### Scenario: Live buffer full
- **WHEN** live events arrive faster than the catch-up proceeds and exceed `backfill.live_buffer`
- **THEN** the channel is closed and reopened after catch-up, and every archived event is published exactly once

### Requirement: Cursor persistence
The connector SHALL keep each pushed point's cursor in `<state_dir>/<service_name>/<device>.json`
with the point's `web_id` and a format version. It SHALL write the file by atomic rename at most
every `cursor_flush` (default 10s) and on disconnect. A missing or unreadable file, or a cursor
whose `web_id` no longer matches the point, SHALL mean no cursor for that point, with a `warn` for
an unreadable file.

#### Scenario: Restart catches up
- **WHEN** the connector is stopped for 5 minutes while the simulator keeps archiving, and is then started again
- **THEN** it replays the events archived while it was stopped

#### Scenario: Corrupt cursor file
- **WHEN** the cursor file contains invalid JSON
- **THEN** the connector logs a `warn`, starts live, and replaces the file on the next flush

### Requirement: Value decoding
Both builds SHALL decode a PI value to a sample with one shared rule set, checked against the
golden vectors in `connectors/aveva-pi/conformance/decode-vectors.json`. Numbers SHALL be decoded to the
point's `datatype`, and a value that does not fit it SHALL give a `bad` sample with the reason.
For a digital value, `datatype = "string"` SHALL give the state name and any integer datatype
SHALL give the state code. In `raw` mode, the sample's raw bytes SHALL be the UTF-8 JSON text of
`Value`. Flags SHALL be accepted as JSON booleans or the strings `"true"` and `"false"`. A value
carrying `Errors` SHALL give a `bad` sample with the messages as the error.

#### Scenario: Golden vectors
- **WHEN** the Rust unit tests and the C ctest run the decode vectors
- **THEN** both produce the expected sample for every vector

#### Scenario: Digital state name
- **WHEN** a digital tag's value is `{"Name":"Running","Value":1,"IsSystem":false}` and the point has `datatype = "string"`
- **THEN** the sample value is `"Running"`

#### Scenario: Wrong type
- **WHEN** a String tag returns `"abc"` for a point with `datatype = "float64"`
- **THEN** the sample is `bad` with an error naming the expected datatype

### Requirement: Quality mapping
A value with `Good = false`, or a digital value with `IsSystem = true`, SHALL give a `bad` sample
whose `error` is the system state name (else `"not good"`). `Questionable` and `Substituted`
values SHALL stay `good`, and the set flags SHALL be listed in `addr.flags`.

#### Scenario: System state
- **WHEN** a tag's value is `{"Name":"I/O Timeout","Value":246,"IsSystem":true}`
- **THEN** the sample is `bad` with `error = "I/O Timeout"`

#### Scenario: Questionable value
- **WHEN** a value has `Questionable = true`
- **THEN** the sample is `good` and `addr.flags` is `["questionable"]`

### Requirement: Units from PI
When a point has no configured `unit`, its samples SHALL carry the PI engineering unit (PI point
`EngineeringUnits`, AF attribute default units abbreviation, or the value's `UnitsAbbreviation`)
when PI reports a non-empty one. A configured `unit` SHALL always win.

#### Scenario: Unit from the server
- **WHEN** a tag has `EngineeringUnits = "degC"` and its point sets no `unit`
- **THEN** its samples carry `unit = "degC"`
