## Why

Many industrial sites already collect their process data in an AVEVA PI System, so a device often
has to read from the PI historian rather than from the PLC behind it. Today that needs the
community [thin-edge-aveva-pi-connector](https://github.com/Cumulocity-IoT/thin-edge-aveva-pi-connector),
a Python container with its own configuration files, its own MQTT publishing and its own batch
ledger to stay under the Cumulocity 16 KB message limit. It sends two HTTP requests per tag on
every poll (a point lookup and an `attime` read), turns TLS verification off, cannot write, and
loses the data of any outage.

tedge-dot already solves configuration, sample publishing, measurement batching, reporting policy,
device parameters, link status and liveness once for every protocol. A PI Web API connector gets
all of that for free, and it is the first connector that talks to a standard REST/JSON API rather
than an OT fieldbus. It therefore doubles as the reference example of how to bring any HTTP-based
system into tedge-dot.

## What Changes

- New protocol module **`aveva-pi`** in both builds: Rust crate `connector-aveva-pi` (feature `aveva-pi`) and C
  module `impl/c/connectors/aveva-pi` (CMake option `TDOT_AVEVA_PI`). It talks to the AVEVA PI Web API over
  HTTPS.
- **Addressing**: a device is one PI Web API endpoint (and optionally a default PI Data Archive or
  AF database). A point addresses a PI point by tag name or full path, an AF attribute by path, or
  any stream by WebId. WebIds are resolved once per connection and cached, not looked up per poll.
- **Batched snapshot polling**: all due points of a device are read with one
  `GET streamsets/value` request (chunked at a configurable number of WebIds), instead of two
  requests per tag.
- **Push delivery**: points are delivered through the PI Web API **Channels** WebSocket
  (`streamsets/channel`), with source timestamps. When a server refuses Channels, the device falls
  back to **recorded polling** (`GET streamsets/recorded` since the last delivered event), which
  also returns every archived event exactly once. A device can pick either, or plain snapshot
  polling.
- **Outage backfill**: after a link outage or a restart, pushed points catch up from their last
  delivered timestamp with `streamsets/recorded`, so the gap is filled with the archived values
  and their original timestamps. A per-device cursor file keeps the position across restarts. The
  catch-up is bounded by a maximum age and a maximum event count, runs after `subscribe` returns,
  and is paced by the runtime (backpressure), so a long outage neither times out nor drops values.
- **SDK change (both builds)**: a pushed sample can be marked **replayed**. The reporting policy
  measures `min_interval` and `debounce` on a replayed sample's source time, so a backfill burst is
  reduced the way the live stream would have been, instead of collapsing to two values. New rule
  in contract §5.3 and shared report test vectors.
- **Writes**: the standard `write` and `write-batch` verbs write a value with
  `POST streams/{webId}/value`, so PI points can be cloud device parameters like on any other
  connector.
- **Value model**: PI point types map to contract datatypes. Digital states map to the state code
  or the state name, and system states (`I/O Timeout`, `Shutdown`, `Pt Created`, ...) or a
  `Good = false` value produce a `bad` sample with the state as the reason. The PI engineering unit
  is used when the point has no `unit`.
- **Security**: TLS is verified by default against the system store or a configured CA bundle.
  Authentication is HTTP Basic, a Bearer token, or a client certificate (mutual TLS). Passwords,
  tokens and keys are read from local files, and these keys are local-only settings that a
  management command cannot change.
- **Simulator and tests**: a PI Web API simulator (Python, our own code) with Basic, Bearer and
  mTLS auth, Channels, recorded data, writes and fault injection. A Robot e2e suite runs against
  both builds, a conformance manifest, a cloud e2e stack, and golden JSON-decode vectors shared by
  both builds.
- **Docs and packaging**: `doc/connectors/aveva-pi-connector-spec.md`, a short guide on writing a
  connector for an HTTP/REST system, packaged and demo configs, a release highlight, and a
  migration note for users of the community plugin.
- **New third-party dependencies**: Rust `reqwest` (rustls) and `tokio-tungstenite` (rustls). C:
  libcurl built statically from source with WebSockets on the mbedTLS 3.6 the OPC UA build already
  fetches (curl license, MIT-style).

## Capabilities

### New Capabilities

- `aveva-pi-web-api-connection`: how the connector reaches a PI Web API server: endpoint, TLS
  verification, Basic, Bearer and client-certificate authentication, secrets in local files,
  WebId resolution and caching, request limits and timeouts, and link status.
- `aveva-pi-data-delivery`: how values reach the sample envelope: snapshot polling, Channels push,
  recorded polling, outage backfill with a persisted cursor, timestamps, datatype and digital-state
  mapping, and quality.
- `aveva-pi-writes`: how `write` and `write-batch` write a value to a PI stream, including digital
  states, timestamps and the errors returned.
- `replayed-samples`: how both SDKs treat pushed samples that a module replays from history: the
  flag, the reporting policy on source time, and no heartbeat reads during a replay. Reusable by any
  module that replays history.

### Modified Capabilities

<!-- none in openspec/specs: the reporting policy lives in contract §5.3, which this change
     extends through the new `replayed-samples` capability. -->

## Impact

- **Rust**: new crate `impl/rust/crates/connector-aveva-pi`, feature `aveva-pi` in `impl/rust/Cargo.toml` and
  in the release feature list; the binary's module registry; `ot-conformance` gains an `aveva-pi`
  simulator kind.
- **C**: new module `impl/c/connectors/aveva-pi`, CMake option `TDOT_AVEVA_PI`, libcurl via `FetchContent`
  (static, HTTP/HTTPS/WS/WSS only) reusing the fetched mbedTLS; zig cross-build in
  `impl/c/cross/` keeps the glibc 2.17 floor; `impl/c/README.md` parity table.
- **Tests**: `connectors/aveva-pi/` (simulator, compose stack, Robot suite, conformance manifests),
  `cloud/aveva-pi/`, the `IMPL` switch in each, and `just c-describe-parity` over the new configs.
- **Packaging**: `packaging/config/aveva-pi.toml` (recommends one configuration file per PI server
  for the C build), `demo/config/aveva-pi.toml`, `.goreleaser.yaml` and
  `impl/c/packaging/nfpm.yaml` entries, a state directory `/var/lib/tedge-dot/aveva-pi` owned by
  `tedge`, simulator image `ghcr.io/thin-edge/tedge-dot/aveva-pi-sim` and the CI image mirror list.
- **Docs**: connector spec, REST connector guide, `connectors/README.md`, docs site page, release
  highlight in `changes/aveva-pi-connector.md`.
- **SDK and contract**: `Sample::replayed` (Rust) and `tdot_sample_t.replayed` (C), the report
  state on source time for replayed samples in `report.rs` and `report.c`, a new rule in contract
  §5.3, and new vectors in `doc/contract/test-vectors/report/`. Existing connectors are unchanged.
