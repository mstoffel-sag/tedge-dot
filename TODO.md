# TODO

## In flight / next

* [ ] Ship profibus in the `tedge-dot-rs` package: the `profibus` cargo feature is excluded
      from the goreleaser builds because its serial dependency (`serialport` via `profirust`)
      has a native libudev build script that does not cross-compile with cargo-zigbuild.
      Options: disable the libudev feature upstream, vendor a libudev stub per target, or build
      the Linux packages natively per architecture. (`tedge-dot-c` already ships PROFIBUS, over
      `tcp://` only.)

* [ ] Close the remaining C parity gaps listed in `impl/c/README.md`. Each has a capability
      name already wired into the test tagging (`C_MISSING_CAPABILITIES` in the justfile), so
      implementing one means removing it from that list and adding the test that was waiting
      for it:
      - `canbus-fd` — classic frames only today; the Rust build has a `canbus-fd` feature.
      - `profibus-serial` — the C module speaks `tcp://` only (no serial PHY, no FDL token
        timing), so it cannot yet drive a multi-master RS-485 bus.
      - `opcua-basic128rsa15` — open62541 aborts a Basic128Rsa15 handshake against the OPC
        Foundation UA-.NETStandard reference server with `BadDecodingError`, where async-opcua
        connects to the same endpoint, so the fault is in the policy implementation rather than
        the server or the configuration. Found by `just test-interop-c opcua`. Deprecated and
        opt-in only (open62541 itself warns its encryption is broken), and `Basic256` works in
        both builds, so this is recorded rather than worked around. Worth a reduced test case
        and an upstream issue before spending more on it.
      - `snmpv3-sha2` — the C build links net-snmp's own crypto (`--with-openssl=internal`), which
        covers MD5/SHA-1 authentication and DES/AES-128 privacy; SHA-224…512 and AES-192/256 need
        a real OpenSSL, which would cost the small, dependency-free package. A device configured
        with one of them loads but stays `disconnected`, naming the capability.
      Also: CAN bus push delivery (the C module renders the push-based bus as drain-into-cache
      polling — same samples, worse latency, so it is not tagged), and the 255/256-byte cap on
      string/raw values (`tdot_value_t.str`, `TDOT_RAW_MAX`).

* [ ] **Address an OPC UA node by namespace URI, not only by index.** `device.point.address`
      takes `node_id = "ns=2;s=..."` or `namespace` + `identifier`
      (`impl/rust/crates/connector-opcua/src/config.rs`); there is no `nsu=` / `namespace_uri`
      form. A server's namespace array is ordered by the server, not by the specification, so a
      firmware update that registers one more namespace silently shifts every index and breaks a
      working configuration — the points resolve to nothing, or worse, to different nodes. The
      UA-.NETStandard reference server documents its own indices as "typical, not guaranteed",
      which is why the interop suite resolves the index at startup
      (`connectors/opcua/interop/resolve-ns.py`) instead of hardcoding it: that proves the
      connector works, but not that an operator could cope. Additive change: accept
      `namespace_uri` alongside `namespace`, resolve it from `Server.NamespaceArray` once per
      session, and re-resolve on reconnect.

* [ ] **OPC UA structured values follow-ups** (openspec change `opcua-custom-datatypes`, Rust
      only; thin-edge/tedge-dot#57):
      - **C port.** The C build lists `opcua-structures` as missing (parity table in
        `impl/c/README.md`). It needs a walker held to `doc/contract/test-vectors/opcua-struct/`,
        resolution through open62541 (`UA_Client_readDatatypeDefinitionAttribute`), per-tick shared
        reads like the SNMP module's. (The C SDK has `bytes` now, and a top-level ByteString is
        already read as `bytes`.)
      - Writes of a structure field or array element (read-modify-write of the body, or a write
        with `IndexRange` for an element).
      - Whole arrays, arrays whose length is known only at runtime (one sample per element: the
        same "rows at runtime" question as SNMP table walks), and multi-dimensional fields.
      - Servers without `DataTypeDefinition` (OPC UA 1.03 `DataTypeDictionary`): parse the
        `.bsd`, or let a point library declare the layout. Raw points plus a flow cover them today.
      - Make `datatype` optional on field points (the definition states the type), and an
        `inspect` CLI command that prints a structure's fields as `[[device.point]]` blocks.
      - Automatic decoding in a flow: publish resolved definitions on a retained typedef topic for
        a generic `ot-struct` flow (design D9).
      - File `doc/upstream/async-opcua-index-range-applied-twice.md`.
* [ ] **The e2e harness can run a stale connector binary.** `_prebuild-stack-images` discards the
      build output, and the Rust connector images compile with a cargo `target` cache mount shared
      between checkouts; an OPC UA run once used a binary built before the last source change,
      so a fix appeared not to work until `docker compose build --no-cache`. The same step once
      hung for 27 minutes: every image had finished building, but `docker buildx bake` never
      exited, and with its output discarded the suite just looked slow. Show the build output,
      bound the step with a timeout, and make sure a source change reaches the binary.
* [ ] SNMP connector follow-ups (`doc/connectors/snmp-connector-spec.md`; polling, SET writes,
      v1/v2c/v3 traps and informs shipped):
      - **File the `snmp2` issues upstream** and drop the patches as they are released: six drafts
        wait in `doc/upstream/snmp2-*.md` and the patched copy is `impl/rust/vendor/snmp2`
        (with its `TEDGE-DOT-PATCH.md`), the same arrangement as `vendor/async-opcua-crypto`.
      - **Table walks** (GETBULK over a column, one sample per row): deliberately deferred — rows
        exist only at runtime while the contract's point list is static (link status `points`,
        the capability descriptor, "one sample per point per read"), so it needs an additive RFC.
        Sketch: one point per column (`address = { column = "…", label = "…" }`) publishing a
        sample per row told apart by `addr.index`, with `ot-measurement`/`ot-alarm` naming the
        series per row.
      - MIB name resolution (`sysUpTime.0` instead of the numeric OID): `snmp2` has a `mibs`
        feature needing `libnetsnmp`, the C build links net-snmp already — but the packages would
        have to ship MIB files.
      - A catch-all for notifications from unconfigured senders (today dropped at debug level),
        which would let a discovery flow onboard equipment with `define-device`.
      - Name resolution only at connect: a DHCP address change is picked up on the next
        reconnect, not while the link is up.
      - Track net-snmp releases: the C build compiles a pinned tarball (version and SHA-256 in
        `impl/c/CMakeLists.txt`), so a security fix there means bumping the pin.
      - **Two devices on one host, different ports** are refused: `check_unique_hosts` compares
        the host literal alone, though an SNMP agent is addressed by host *and* port. The
        restriction exists because notifications are routed by source address, so it should
        apply to devices that have notification points, not to polled-only ones. Changing it
        means the spec (§3.2) and both implementations together, which is why it is not in the
        connector's first PR.
      - **v3 notifications through a forwarder** work in Rust but are refused by the C module
        ("v3 through a forwarder is not supported"), and no test covers it — the e2e forwarder
        relays v2c only. Either implement it in C or record it as a tagged capability gap.
      - **GETBULK for scalars** buys no round trip over a multi-varbind GET, and its
        GETNEXT semantics turn a missing instance into "the agent answered X instead of Y"
        rather than a plain noSuchObject. Reconsider `bulk = true` as the v2c/v3 default.
      - The receiver's `LocalEngine` state is not rolled back when a forwarded v3 notification
        is tried against a device it turns out not to belong to (`listener.rs`), unlike the
        per-device security state beside it.
      - **Rust/C parity gaps found by a review of both implementations, none covered by a test.**
        Each needs its own change and its own test; they are behaviour changes, not slips:
        - **Inform vs trap dispatch disagrees.** Rust decides from the msgFlags *reportable* bit
          (`listener.rs`), C from whether the message's engine ID is ours (`connector_snmp.c`).
          A v3 Trap that wrongly sets the bit is published by C but answered by Rust with a
          `usmStatsUnknownEngineIDs` Report; an inform with the bit clear is acknowledged by C
          and dropped unacknowledged by Rust.
        - **A poll timeout ends the C device's link, not just its samples.** C returns a
          transport verdict, so its runtime publishes `disconnected` and calls
          `disconnect_device` — which releases the listener, so a device with both object and
          trap points stops receiving notifications during a polling outage. Rust reports
          `degraded` and keeps the listener. The outage test accepts either status and stops a
          device that has no trap points, so it cannot see this.
        - **C pays a timeout per batch.** Rust marks the cycle's remaining batches bad once one
          fails (spec §5.1 requires this); C issues every batch, costing
          `request_timeout × (1 + retries)` each, stalling the whole tick. The outage test stops
          a 4-point device, which is a single batch.
        - **v1 error-status retry scope.** Rust drops the point named by `error-index` and
          retries the rest for any non-zero status; C only for `noSuchName`, so a `genErr`
          marks the whole batch bad and degrades the link.
        - **Raw-mode exception varbinds.** A notification varbind carrying
          `noSuchObject`/`noSuchInstance`/`endOfMibView` for a `mode = "raw"` point is a good
          sample with empty raw on Rust and a bad sample on C. Rust's own polled path reports
          it bad, so Rust also disagrees with itself.
        - **A `level` below what the passwords imply** is rejected by Rust (the whole config
          fails) and accepted by C, which then runs that device at the lower level — a shared
          config file is not portable, in the direction that lowers security.
        - **Numeric bounds differ**: C enforces `retries` 0–100, `max_varbinds` 1–256, `port`
          1–65535; Rust enforces only `max_varbinds ≠ 0`. A file one build runs, the other
          refuses to start on.
      - **`malformed[7]` does not test what it says.** It is named "SNMPv3 message" with the
        reason "SNMPv3 needs credentials", but its bytes are a v2c-shaped datagram whose version
        field is simply set to 3: `020103` is followed by `0406 7075626c6963` (an OCTET STRING
        community) where RFC 3412 requires msgGlobalData, a `0x30` SEQUENCE — every genuine v3
        vector in the file has `0x30` there. It is still correctly rejected, so no valid input is
        refused, but it pins "a datagram claiming version 3 is refused" rather than "a well-formed
        v3 message with no credentials is refused". Replacing its hex with a properly structured
        v3 message carrying an unknown user would make it test its own premise.
      - **`a_values_canonical_octets_decode_back_to_it`** (`connector-snmp/tests/properties.rs`)
        builds both sides of its comparison with `VarValue::from_library`, so a wrong mapping in
        that function cancels out — the same defect that was just fixed in
        `encode_then_decode_round_trips`. It is partly saved by constructing the BER element from
        an independent tag table, which would catch a type whose tag changed, but not a value that
        decodes to the wrong number.
      - **The C conformance harness does not assert the usmStats counter's VALUE on a discovery
        probe.** The probe itself is now checked (Report produced, msgID, request-id, one
        varbind, counter OID, counter32 type), but net-snmp keeps usmStats process-wide and the
        message vectors run before it, so the number depends on what the harness did earlier.
        The Rust side pins it at 1 because it drives its own `LocalEngine`. Making the C side
        equivalent means either resetting the library's statistics between phases or running the
        probe in its own process; neither is worth it for one integer, but it is the one
        assertion the two harnesses do not share.
      - **The C build's USM tables grow with every engine ID a spoofed source claims.**
        Authenticating a v3 notification needs keys localized to the engine the message *claims*,
        so `v3_prepare` (`impl/c/connectors/snmp/connector_snmp.c`) installs them before the
        message can be verified — and net-snmp's user table and engine-time cache are
        process-wide and never shrink. A source spoofing a configured device's address (the only
        routing check) and sending forged engine IDs with the device's user name — which every
        v3 message carries in the clear — grows the process without bound and slows the linear
        `usm_get_user` scan on every message. Not fixed, because the obvious cleanup is worse:
        `free_enginetime()` frees the whole 1-of-23 hash bucket without comparing engine IDs, so
        it discards the boots and time of unrelated engines (this connector's own among them),
        and `usm_remove_user()` deletes an entry another device's session owns and that net-snmp
        never rebuilds (it returns early once `SNMP_FLAGS_USER_CREATED` is set). A safe fix needs
        a per-engine removal primitive upstream, or a design that does not install until the
        level check has passed. The Rust build is unaffected: it localizes keys itself and rolls
        the state back on any failure.
      - **The e2e case `An Unauthenticated v3 Trap Is Dropped And The Device Keeps Working`
        does not discriminate the fix it was written for.** Every v3 trap device in
        `connectors/snmp/connector.toml` pins `engine_id`, and the simulator sends the
        unauthenticated trap from that same engine, so the engine-learning branch is never
        reached and the case passes with or without the receiver-side fix — it currently proves
        only that a below-level notification is dropped, which is worth having but is not the
        security property. Discriminating it needs a trap device whose engine is *unpinned* at
        the moment the case runs (so the case must also precede any authenticated trap from that
        device), which means a new device and notification sender in the stack. The property
        itself is covered at unit level by
        `an_unauthenticated_notification_teaches_an_authenticated_user_nothing`
        (`impl/rust/vendor/snmp2/src/tedge_dot_tests.rs`), which does fail without the fix.
      - **Push recovery ends when a re-subscribe fails** (`retain_pushed`, `impl/rust/crates/sdk/src/runtime.rs`):
        a device drops out of `pushed_devices` the moment it has no subscribed points, which is
        exactly when a failed re-subscribe means recovery should keep trying — so a mixed device
        whose subscription dies and fails to come back loses both retry paths at once (its polls
        keep succeeding, so `reconnects` stays clear too) and its push-only points go quiet until
        a restart. Not fixed here: keeping every configured device instead reconnects a healthy
        polled-only device once a minute forever, which the existing test
        `push_recovery_ends_when_a_device_falls_back_to_polling` pins deliberately. Telling the
        two apart needs `Connector::pushes_point` threaded into `retain_pushed` (both call sites),
        a change to shared runtime behaviour that wants its own PR.
      - **`subscribe = false` on a point a module can only push** is scheduled for polling, where
        `read_points` silently drops it: no sample, no bad sample, no warning. Not fixed in the
        runtime: having `push_points` override an explicit `subscribe = false` is worse than the
        drop, since it silently contradicts the operator. Both SNMP implementations already
        reject the combination in their own validation (`config.rs`, `connector_snmp.c`), which is
        where it belongs; the gap is only that the SDK does not *require* a module to do so.
      - **A transport error ends the whole device cycle in the C runtime**, for every connector:
        the poll loop in `impl/c/sdk/src/runtime.c` stops at the first read returning non-zero,
        so the remaining due points never publish the bad samples they already hold — and a
        consumer that simply stops hearing about a point cannot tell a stalled device from a
        value that has not changed. The SNMP module works around it for itself by holding the
        verdict back to the last point of a failed batch (`transport_pending` in
        `connector_snmp.c`); modbus, opcua and canbus still lose the rest of the tick. The Rust
        runtime publishes every sample of a failed batch before it judges the link, so fixing
        this in the C SDK would restore parity and let the SNMP workaround go. Only the SNMP
        e2e suite covers the path at all (`A Stopped Agent Gives Bad Samples And The Link
        Recovers`), which is why it went unnoticed.

* [ ] The `.apk` packages carry versions apk-tools rejects, for BOTH implementations and for
      real releases, not just snapshots: `apk version -c` reports `0.0.1-alpha.2` (this
      repository's existing tag format) and `0.0.0_pre.<sha>` (what nfpm derives from the
      snapshot version) as invalid, because apk's grammar allows `_pre1` but not `_pre.<hash>`
      and no bare `-alpha.2`. Valid forms are e.g. `0.0.1_alpha2` or `0.0.0~<sha>`. Fixing it
      means either an apk-specific version override in both packaging configs or a change to
      the tag convention. Nothing in CI installs an apk, which is why it has gone unnoticed —
      a `apk add --allow-untrusted` smoke on the built package would catch it.

* [ ] A simulator hook to delete an OPC UA subscription server-side while leaving the session
      up. It is the one push-failure path neither implementation can be tested against today
      (see the note in `impl/c/README.md`): open62541 reports the client as healthy throughout,
      so a regression there would be silent. `connectors/opcua/sim/` would need an endpoint or
      a method the suite can call.

* [ ] Fuzz the C parsers. The validation policy below requires a fuzz target for anything
      parsing external input; the Rust SDK has four (`just fuzz-all`), the C build has none,
      so its TOML loader (tomlc99) and DBC parser are only covered by the shared golden
      vectors and the e2e suites. libFuzzer via clang would reuse the same corpora.

* [ ] Cloud Fieldbus increments 3 + 4 (see `doc/rfc/0002-cloud-fieldbus-integration.md`;
      increments 1 + 2 shipped and verified live 2026-07-02): generalise the device-type
      translator per protocol, and the export path / UI-placeholder reconciliation (needs a
      tenant-side actor — a device cannot own or delete the UI-created managed object).
* [ ] Conformance suite implementation (`doc/conformance/conformance-suite.md` is spec'd,
      harness not built yet).
* [ ] File the upstream async-opcua issues (drafts ready in
      `doc/upstream/async-opcua-stranded-sample.md` and
      `doc/upstream/async-opcua-null-session-nonce.md`); drop `vendor/async-opcua-crypto`
      and the `[patch.crates-io]` entry once the nonce fix ships.
* [ ] c8y-fieldbus-import deferred items (script header TODOs): alarm/event/status mappings
      (gap G4), RTU serial-port resolution from `[connection.serial]`, signed and
      multi-register bit fields.
* [ ] Device parameters (RFC 0003, prototype implemented): persist the
      last-commanded value of write-only parameters across mapper restarts; derive
      `meta.parameter.set` from the Cloud Fieldbus device type name in `c8y-fieldbus-import`;
      optional Modbus FC16 fast path for `write-batch` on contiguous registers; optional
      `c8y_ParameterUpdate` audit event flow on top of the twin; gateway-level connector
      settings (poll_interval) as a tedge-parameter-plugin set script issuing `set-config`.
* [ ] Legacy write-payload compatibility (gap G2): accept explicit-address
      (`register`/`coil`/`address`/`ipAddress`) and name-based `metrics[]` payloads for
      `c8y_SetRegister`/`c8y_SetCoil`, not only `{point, value}`.

## Connector candidates

* ethercat — https://github.com/ethercrab-rs/ethercrab (MIT/Apache-2.0)
* EtherNet/IP — https://github.com/sergiogallegos/rust-ethernet-ip
* BACnet — spec sketch exists in `doc/connectors/_template-connector-spec.md`
* DNP3 — https://github.com/stepfunc/dnp3 — NOT possible (non-OSS license)

Score each implementation (functionality + maintainability incl. upstream library activity)
before promoting it past experimental.

## Validation policy

* Connectors must be validated with unit + integration tests and e2e simulator tests, and the
  tests must be proven by running them (see `doc/testing.md`).
* Shared SDK decode logic requires property-based tests; parsers of external input require a
  fuzz target.
