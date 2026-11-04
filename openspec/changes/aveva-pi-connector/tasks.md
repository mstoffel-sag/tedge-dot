## 1. Spec and de-risking

- [x] 1.1 Check each PI Web API assumption against the PI Web API reference (and a real instance when one is available): the `heartbeatRate` unit and heartbeat message shape on `streamsets/channel`, the status code of the WebSocket upgrade when Channels are disabled, Bearer tokens on the upgrade request, `streamsets/recorded` order and continuation with `maxCount` (and what happens at exactly `maxCount` events), the digital-state write body, and URL length limits (414). Record each with its source in a "Verified behaviour" section of the connector spec. Adjust design D3, D5, D7 and D10 where the reference disagrees.
- [x] 1.2 Write `doc/connectors/aveva-pi-connector-spec.md` from the template: scope table, capability descriptor JSON, `[connection]` / `protocol_address` / `address` tables with TOML examples, decoding table (design D6), delivery modes and backfill (D3–D5), write flow (D7), link status (D12), C threading and the one-configuration-per-server layout (D9), acceptance vectors, and the 1.1 record. Configuration is documented in tables, like the SNMP and OPC UA specs (no per-protocol schema file). Verify every spec requirement maps to a section.
- [x] 1.3 C build de-risk: fetch libcurl ≥ 8.11 with `FetchContent`, static, HTTP/HTTPS/WS/WSS only, on the fetched mbedTLS 3.6 (`TDOT_AVEVA_PI_VENDORED_CURL`). Build a throwaway `curl_ws` smoke binary natively and with the zig cross-build for arm64. Verify the glibc floor stays 2.17 (`objdump -T`) and record the size added.
- [x] 1.4 Write `connectors/aveva-pi/conformance/decode-vectors.json` (every PI point type, digital and system states, Good/Questionable/Substituted, wrong-type, raw mode) with an independent Python generator in `conformance/tools/`. Verify the generator reproduces the file.

## 2. SDK: replayed samples (both builds)

- [x] 2.1 Contract: add the replay rule to §5.3 (time-based keys on source time for replayed samples, hand-over to the runtime clock, no heartbeat reads during a replay) and a sentence to §5.4. Add replay vectors to `doc/contract/test-vectors/report/vectors.json` (min_interval over a burst, debounce over a burst, hand-over with a held reading, quality change inside a replay, replayed without source time).
- [x] 2.2 Rust: `Sample::replayed`, report state on source time for replayed samples in `report.rs`, heartbeat suppression in the runtime, the missing-source-time `warn`. Pass the new vectors and extend the report proptests (a replay publishes exactly what the live state machine would on a source-time clock).
- [x] 2.3 C: `tdot_sample_t.replayed`, the same in `impl/c/sdk/src/report.c` and the runtime. Pass the same vectors in `impl/c/tests/report.c`.
- [x] 2.4 `just test`, `just c-test` and `just lint` pass; existing connectors' e2e suites are re-run in 7.6.

## 3. PI Web API simulator

- [x] 3.1 `connectors/aveva-pi/sim/` (Python, aiohttp): seed file, in-memory archive with generated values, WebIds in PI format, `dataservers`, `points`, `attributes`, `enumerationsets`, `streamsets/value`, `streamsets/recorded` (paging with `maxCount`), `streams/{id}/value` POST, `selectedFields`, following the 1.1 record. Unit-test it with pytest.
- [x] 3.2 Channels WebSocket `streamsets/channel` with `includeInitialValues` and `heartbeatRate`.
- [x] 3.3 Auth: none, Basic, Bearer (rotatable token file), mTLS. TLS certificates generated at start into a shared volume, plus one server certificate without a subject alternative name. CSRF header check on POST.
- [x] 3.4 Control API `/_sim/...`: pause (accept, never answer), 503, 414 above N WebIds, close channels, disable Channels (upgrade refused), outage window with archiving continuing, insert past events, a burst rate high enough to fill the live buffer, request counters per endpoint.
- [x] 3.5 `Dockerfile`, image `ghcr.io/thin-edge/tedge-dot/aveva-pi-sim`, add the base image to `.github/mirror-images.txt`, publish job entry. `just check-mirrored-images` passes.

## 4. Rust connector

- [x] 4.1 Crate `connector-aveva-pi` (feature `aveva-pi`, in `default` and the release feature list), module registration, `reqwest` (rustls) and `tokio-tungstenite` (rustls) dependencies. Check that the hyphenated protocol id works everywhere it is used (module registry, topics, schema file name, CI matrix keys, flows). `cargo build --features aveva-pi` and the musl cross-build succeed.
- [x] 4.2 Config parsing and validation (`config.rs`): `[connection]` with device overrides, address forms, inline secrets refused, durations, `backfill` keys including `live_buffer`, `local_only_settings`. Unit tests for every load-error scenario in `aveva-pi-web-api-connection`.
- [x] 4.3 HTTP client (`client.rs`): one client per `(base_url, auth)`, TLS options, Basic, Bearer with re-read after 401, client certificate, timeouts, CSRF header, chunking with 414 halving, PI `Errors` extraction, TLS errors that name the cause. Unit tests against a local test server.
- [x] 4.4 WebId resolution and cache (`resolve.rs`): points, attributes, data server, digital sets, `resolve_retry`. Tests: no lookups per poll, unknown tag.
- [x] 4.5 Pure decoder (`value.rs`) passing `decode-vectors.json`. Add proptest properties (never panics, `good` ⇒ value present) to the crate's tests and a cargo-fuzz target `aveva_pi_value`, and add it to `just fuzz-all`.
- [x] 4.6 `connect`, `read_points` (snapshot, read time, `addr` echo, units from PI), link status mapping (D12).
- [x] 4.7 Push: `pushes_point`, `subscribe` that returns once the channel is up and starts one task per device (abort-on-drop), Channels client, recorded polling, Channels→recorded fallback, `check_subscription` from heartbeats.
- [x] 4.8 Cursor and backfill (`cursor.rs`), run in the device task: per-point cursor advanced on `sink.send`, atomic file writes with flush interval, version and `web_id` check, catch-up paging paced by the sink, `replayed` on catch-up samples, bounded live buffer with the recorded detour, bounds, dedup, ordering of catch-up before buffered live events. Unit tests for dedup, bounds, the live-buffer detour, the corrupt-file case, and a catch-up longer than `operation_timeout`.
- [x] 4.9 `execute("write")`: datatype encoding, digital name/code with set check, error mapping (D7). Unit tests per `aveva-pi-writes` scenario.
- [x] 4.10 `just test` and `just lint` pass.

## 5. C connector

- [x] 5.1 `impl/c/connectors/aveva-pi/` module, CMake option `TDOT_AVEVA_PI` (ON), libcurl from 1.3 linked, `local_only_settings`, config parsing mirroring 4.2 with ctest cases.
- [x] 5.2 HTTP layer on curl: easy handles for single requests, one multi handle per `read_points` call so its chunks run in parallel within one `request_timeout`; TLS options, auth methods, token re-read, timeouts, CSRF header, chunking with 414 halving, TLS errors that name the cause.
- [x] 5.3 WebId resolution and cache, mirroring 4.4.
- [x] 5.4 Decoder (`pi_value.c`) passing the same `decode-vectors.json` in ctest.
- [x] 5.5 `connect_device`, `read_points` (batched snapshot), link status with `reason`, `write_point` mirroring 4.9.
- [x] 5.6 Push worker thread per device (D9): Channels via `curl_ws_recv`/`curl_ws_send`, recorded polling, fallback, growable ring with the OPC UA bounds and a high-water wait, `subscribe_device` / `drain_subscriptions` (cursor advanced on drain), clean join on disconnect.
- [x] 5.7 Cursor persistence and backfill in the worker, mirroring 4.8 (`replayed`, pacing, live buffer), with ctest cases including a catch-up larger than the ring's bound with no drops.
- [ ] 5.8 `just c-test` passes on macOS and in a Debian container as root. Run the module's ctest under ASan/UBSan and TSan (worker thread, ring, cursor). Update the parity table in `impl/c/README.md`.

## 6. Stack, e2e and conformance

- [x] 6.1 `connectors/aveva-pi/`: `docker-compose.yaml` (broker, simulator, a second simulator for the stall case, connector, flows; no fixed host ports), `Dockerfile.connector`, `connector.toml`, `entrypoint.sh`, `secrets/`. `just e2e-up aveva-pi` and `just e2e-up aveva-pi c` start.
- [x] 6.2 `connectors/aveva-pi/tests/aveva_pi_e2e.robot`: descriptor, health, link status, snapshot read, unknown tag, digital state name/code, system-state bad sample, questionable flag, units from PI, channel push, recorded mode, Channels fallback, silent channel, outage backfill, backfill under `min_interval` (replayed samples spread over source time), large backfill with no loss, live-buffer detour, max-age bound, restart catch-up, each auth method, TLS rejection with the cause named (including the certificate without a SAN), wrong password, write numeric/digital/unknown state/403/503, 414 chunking, a stalled server delaying another device of the same configuration by at most one `request_timeout`, measurement output. Tag the `flows` cases. Measurement output runs as a second connector instance (`connector-measure.toml`, device `pim`, its own simulator), including an outage replayed with PI times. **Not yet covered:** a stalled server delaying another device of the same configuration (C).
- [x] 6.3 `just test-e2e aveva-pi` and `just test-e2e-c aveva-pi` pass with no PI-specific skips.
- [x] 6.4 `ot-conformance`: `connectors/aveva-pi/conformance.toml` with the module compiled into the harness (feature `aveva-pi`), so the static layers and S1 capability agreement run. Like SNMP, the simulator is Docker-only, so the behavioural layer is skipped and covered by the e2e suite in both builds. `just conformance aveva-pi` passes.
- [x] 6.5 `cloud/aveva-pi/` stack and suite (device registration, measurements, backfilled measurements with their original times, a parameter write round-trip), both `IMPL`s. `just test-cloud aveva-pi` and `just test-cloud-c aveva-pi` pass. Built (13 tests: service, child, `c8y_AvevaPiDevice` twin, measurements, PI units, PI time, DTM definition, parameter fragment, write to PI, write refused with 403, outage and restart catch-up); `just test-cloud aveva-pi` and `just test-cloud-c aveva-pi` pass 13/13 against a live tenant. Parameter points carry `report.max_interval`: PI pushes an unchanging setpoint only at subscribe, before the c8y mapper may be running, so the twin needs the heartbeat.
- [x] 6.6 Add `aveva-pi` to the CI e2e, e2e-c, conformance and cloud matrices.
- [x] 6.7 PR validation beyond the matrices: a `python` CI job (`just test-aveva-pi-python`: the simulator's tests and the migration script's, against the simulator, with a golden output the Rust connector loads and both builds compare in describe parity), and a `c-sanitize` CI job (`just c-sanitize-aveva-pi`: the module's ctest under ASan+UBSan and TSan).

## 7. Packaging, demo and docs

- [x] 7.1 `packaging/config/aveva-pi.toml` (no devices, commented auth/TLS examples, `report` and `output` comments, `batch = { group_by = "time" }` recommended, one configuration file per PI server for the C build), `demo/config/aveva-pi.toml` against the simulator, add the simulator to `demo/docker-compose.yaml`, and add both configs to `.goreleaser.yaml` and `impl/c/packaging/nfpm.yaml`. Create `/var/lib/tedge-dot/aveva-pi` owned by `tedge` in both packages. `just check-manifest-parity` and `just c-describe-parity` pass.
- [x] 7.2 `doc/connectors/rest-connectors.md`: how to write a connector for an HTTP/REST system, using the PI module as the worked example (client/decoder/glue split, resolution caching, batching, push via WebSocket, cursor-based backfill run outside `subscribe`, backpressure, replayed samples, secrets as local-only settings, simulator with fault injection).
- [x] 7.3 Migration guide from the community plugin in `doc/migration/`: config mapping, a script that reads each tag's `PointType` from PI Web API and writes the points with default datatypes, series naming change, ledger replaced by measurement output, enabling Basic or OIDC where only Kerberos is on, and replacing certificates that rustls/mbedTLS reject. Add the AVEVA trademark disclaimer to the connector spec and the docs site page.
- [x] 7.4 Update `connectors/README.md`, the root `README.md` protocol list, `doc/roadmap.md` (Kerberos and server-supplied datatypes as follow-ups), and the docs site connector page.
- [x] 7.5 Release highlight in `changes/aveva-pi-connector.md` (see `changes/README.md`), including the replayed-samples SDK change. Do not edit `packaging/release-notes.md`.
- [x] 7.6 Final check: `just test`, `just lint`, `just c-test`, `just check-capability-tags`, `just check-release-versions`, `just test-flows`, and both e2e suites pass. Run `openspec validate aveva-pi-connector`.
