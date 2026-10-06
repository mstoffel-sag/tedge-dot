# OT Connector Conformance Suite

| Field | Value |
| --- | --- |
| Status | Implemented — [`impl/rust/crates/ot-conformance`](../../impl/rust/crates/ot-conformance/) |
| Applies to | every connector implementing the [OT Connector Contract](../contract/ot-connector-contract.md) |

The conformance suite is what makes a community of connectors trustworthy. A connector that
passes it behaves identically — at the contract boundary — to every other connector, so
flows, tooling, dashboards, and operators can rely on it. The suite is designed so a
contributor (human or AI agent) can prove a new connector correct **without hardware**.

## 1. Structure

The suite has three layers, run in order:

```text
 ┌────────────────────────────────────────────────────────────────────┐
 │ Layer 1 — Schema conformance (static, no connector running)        │
 │   validate sample/command/status/config payloads vs JSON Schemas   │
 ├────────────────────────────────────────────────────────────────────┤
 │ Layer 2 — Decode conformance (pure functions, no I/O)              │
 │   golden vectors: bytes + datatype + endianness/word_order → value │
 ├────────────────────────────────────────────────────────────────────┤
 │ Layer 3 — Behavioural conformance (connector ⇄ protocol simulator) │
 │   run the real connector against a simulator, assert MQTT traffic  │
 └────────────────────────────────────────────────────────────────────┘
```

Layers 1–2 are cheap, deterministic, and the bulk of the value. Layer 3 needs a per-protocol
simulator but reuses one harness.

## 2. Layer 1 — Schema conformance

Every message a connector emits MUST validate against the contract schemas:

- samples → [sample.schema.json](../contract/schemas/sample.schema.json)
- commands → [command.schema.json](../contract/schemas/command.schema.json)
- status/capabilities → [status.schema.json](../contract/schemas/status.schema.json)
- config → [config.schema.json](../contract/schemas/config.schema.json) + the connector's own
  protocol schema

The harness captures published payloads (Layer 3) and validates them, and also validates the
static example payloads embedded in each schema's `examples` array. A connector PR that breaks
the envelope fails here immediately.

## 3. Layer 2 — Decode conformance (golden vectors)

This is the heart of correctness for `typed` mode. Because all connectors decode through the
SDK's `decode_primitive`/`encode_primitive`, the golden vectors live **once** in the SDK and
every connector that advertises a datatype must pass the vectors for it.

Vectors are stored as data (JSON), not code, so they are language-neutral and AI-auditable.
The file lives at [`impl/rust/crates/sdk/conformance/vectors.json`](../../impl/rust/crates/sdk/conformance/vectors.json)
and is enforced on every `cargo test --manifest-path impl/rust/Cargo.toml` via `tedge_dot_sdk::conformance`:

```json
{
  "id": "float32-be-bigword-42_5",
  "datatype": "float32",
  "endianness": "big",
  "word_order": "big",
  "bytes": "422a0000",
  "expect": { "value": 42.5, "value_repr": "number" }
}
```

The Modbus reference vectors in
[modbus-connector-spec §9](../connectors/modbus-connector-spec.md#9-acceptance-test-vectors)
are the canonical seed set. The full SDK vector file MUST include, for each advertised
datatype:

| Case | Why |
| --- | --- |
| nominal value, big endian / big word | baseline |
| signed negative (int types) | two's-complement |
| both `word_order` values (multi-word types) | word swap |
| both `endianness` values (where meaningful) | byte swap |
| min/max representable | range edges |
| IEEE-754 specials for floats (`+0`, `-0`, `inf`, `nan` handling policy) | float edge cases |
| bit-field extraction (if `bitfield` advertised) | masking |
| encode→decode round-trip | write path symmetry |

A connector under test runs each applicable vector through its read path (decode) and, for
writable types, the round-trip through its write path (encode), and asserts the `expect`.

## 4. Layer 3 — Behavioural conformance

Layer 3 runs the **real connector binary** against a **protocol simulator** and a test MQTT
broker, then asserts the MQTT side of the contract.

```text
 ┌───────────────┐   protocol    ┌──────────────────┐   MQTT   ┌──────────────────┐
 │  Protocol     │◀─────────────▶│ tedge-dot        │◀────────▶│  test broker +   │
 │  simulator    │   (e.g. TCP)  │ (under test)     │          │  assertion probe │
 └───────────────┘               └──────────────────┘          └──────────────────┘
```

Reusable harness, per-protocol simulator. The harness brings its own test broker (a minimal
in-process MQTT 3.1.1 broker that records every message *with the publishing client
attached* — which makes the topic-discipline and retain-flag checks exact) and a built-in
`tokio-modbus` TCP simulator seeded from the manifest's seed file. Nothing external is
required: no mosquitto, no docker, no hardware. The standalone
[`connectors/modbus/sim`](../../connectors/modbus/sim/) image remains available for
container-based end-to-end tests.

The connector under test runs in-process by default (the protocol module under the real SDK
runtime — the identical code path the shipped binary links). An out-of-tree connector binary
is tested instead via `[harness] command` in its manifest — this is how the C build
([impl/c/](../../impl/c/)) is checked: `connectors/<proto>/conformance-c.toml` points the
harness at `impl/c/build/tedge-dot` (`just conformance-c <proto>`), and the static S1 check is
skipped for external connectors because only the live descriptor (B9) describes them.

### 3.1 Required behavioural checks

| # | Check | Pass criterion |
| --- | --- | --- |
| B1 | Startup | Capability descriptor (retained) + service health `up` published. |
| B2 | Sample publishing | For each configured point, a sample on `…/sample/<point>` validating Layer 1, with the value matching the simulator's seeded data (cross-checked via Layer 2 decode). |
| B3 | Modes | A `typed` point yields `value`+`value_repr`; a `raw` point yields `raw` only. |
| B4 | Quality | A simulated read failure yields a `bad` sample with `error`, not a dropped message. |
| B5 | Link status | `status/link` transitions to `connected`; to `disconnected`/`degraded` when the simulator drops — tested at three levels: an application outage (requests fail, transport up), a transport drop (the TCP session dies), and a **silent peer** (the session stays open and answers nothing, the case an unbounded protocol call hangs on forever). Recovery must restore `connected`; after a transport drop the connector must re-establish the session itself (reconnect with backoff), and with a silent peer it must keep reporting instead of going quiet. |
| B6 | Write verb | A `cmd/write/<id>` `init` drives `executing`→`successful`; the simulator observes the written value; round-trips through a subsequent read. |
| B7 | Access control | A write to a `read`-only point yields `failed` with a reason and no simulator write. |
| B8 | Hot reload | A config change (add a point, applied through the management `define-device` verb) is picked up without restart; the new point starts publishing. |
| B9 | Capability honesty | The connector never emits a datatype/mode/verb it did not advertise. |
| B10 | Topic discipline | The connector publishes only under its contract topics; never to `…/m/`, `…/e/`, `…/a/`. |
| B13 | Value map | A clone of a writable numeric point with a `map` (contract §4.3) publishes the label with the code as `source_value`; a write of a label reaches the simulator as its code and reads back as the label; a label with no inverse fails with no simulator write; and a device whose only point maps nothing it reads publishes `bad` samples while its link stays `connected`. |

### 3.2 Flow integration smoke test

Optionally, the harness pipes captured samples through the reference flows with
`tedge flows test` and asserts the standard measurement/alarm output — proving the
end-to-end driver→flow story for that connector.

## 5. Conformance manifest

Each connector ships a `conformance.toml` declaring what it claims, so the harness selects the
applicable vectors and behavioural checks. Every connector in this repository has one:

| Connector | Manifest | Behavioural layer |
| --- | --- | --- |
| Modbus | [connectors/modbus/conformance.toml](../../connectors/modbus/conformance.toml) | built-in `modbus-tcp` simulator — full B1–B10 |
| OPC UA | [connectors/opcua/conformance.toml](../../connectors/opcua/conformance.toml) | built-in `opcua` simulator (embedded `async-opcua` server) — full B1–B10, polled mode |
| CAN bus | [connectors/canbus/conformance.toml](../../connectors/canbus/conformance.toml) | skipped (vcan is Linux-only; covered by the e2e suite) |
| CANopen | [connectors/canopen/conformance.toml](../../connectors/canopen/conformance.toml) | skipped (vcan is Linux-only; covered by the e2e suite) |
| PROFIBUS-DP | [connectors/profibus/conformance.toml](../../connectors/profibus/conformance.toml) | skipped (no built-in simulator yet) |
| SNMP | [connectors/snmp/conformance.toml](../../connectors/snmp/conformance.toml) | skipped (no built-in `snmp-agent` simulator in the harness yet; the connector polls and writes, so B1–B10 would apply). Covered by the SNMP golden vectors both implementations read, and by the e2e suite against a pysnmp agent and net-snmp notifications |

Connectors without a built-in simulator still run the static layers **plus a static capability
agreement check**: the harness builds the protocol module in-process, applies the SDK's
management augmentation to its raw capability descriptor, and requires it to agree with the
manifest — so capability drift is caught in CI for every connector, broker or not. A declared
simulator `kind` the harness has no built-in implementation for (e.g. the Dockerised vcan
stacks) skips the behavioural layer with an explanatory note instead of failing.

```toml
[connector]
protocol  = "modbus"
modes     = ["raw", "typed"]
datatypes = ["bool", "int16", "uint16", "int32", "uint32", "float32", "float64"]
verbs     = ["write"]        # module verbs only: the SDK's write-batch and management verbs are implied
features  = ["polling", "bitfield"]
subscribe = false

[simulator]
kind  = "modbus-tcp"
image = "connectors/modbus/sim"
seed  = "conformance/seed/modbus.json"
```

The harness cross-checks the manifest against the live capability descriptor (B9): they MUST
agree.

## 6. Running it

```sh
# Everything (layers 1-3; no external broker, simulator or hardware needed):
cargo run --manifest-path impl/rust/Cargo.toml -p ot-conformance -- check --spec connectors/modbus/conformance.toml

# Layers 1 & 2 only — fast, static:
cargo run --manifest-path impl/rust/Cargo.toml -p ot-conformance -- check --spec connectors/modbus/conformance.toml --static

# Layer 3 only — behavioural (connector ⇄ simulator ⇄ test broker):
cargo run --manifest-path impl/rust/Cargo.toml -p ot-conformance -- check --spec connectors/modbus/conformance.toml --behavioural

# Machine-readable reports for CI / agents:
cargo run --manifest-path impl/rust/Cargo.toml -p ot-conformance -- check --spec connectors/modbus/conformance.toml \
    --junit conformance-report.xml --json conformance-report.json
```

`ot-conformance` is a small harness binary (part of the workspace,
[`impl/rust/crates/ot-conformance`](../../impl/rust/crates/ot-conformance/)). It exits non-zero on any failure and
emits a machine-readable report (JUnit + JSON) suitable for CI and for an AI agent to consume
and self-correct against. The full suite also runs as a workspace integration test
(`cargo test --manifest-path impl/rust/Cargo.toml -p ot-conformance`), so a connector PR cannot silently break conformance.

## 7. Definition of "conformant"

A connector is conformant for a release when, on that release:

1. Layer 1 passes for all emitted message types.
2. Layer 2 passes for every datatype in its manifest (read and, where writable, round-trip).
3. Layer 3 checks B1–B10 pass against the declared simulator.
4. The manifest and the live capability descriptor agree.

Only conformant connectors are eligible for the [registry](../community/community-model.md)
"verified" tier.
