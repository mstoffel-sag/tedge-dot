//! Layer 3 — behavioural conformance.
//!
//! Runs the real connector against the built-in protocol simulator and the in-process test
//! broker, then asserts the MQTT side of the contract: checks B1–B10 of
//! `doc/conformance/conformance-suite.md` §3.1, plus schema validation of every captured
//! payload (the dynamic half of Layer 1).

use crate::broker::{topic_matches, BrokerHandle, Record};
use crate::host::{rewrite_config, Host, TempDir};
use crate::layer1::{Kind, Schemas};
use crate::manifest::Manifest;
use crate::report::Layer;
use crate::sim::{self, PointSpec, Simulator};
use std::collections::BTreeSet;
use std::time::Duration;
use tedge_dot_sdk::{
    decode_primitive, extract_bitfield, model::hex_grouped, Access, ConnectorConfig, DataType,
    Endianness, Mode, Transform, Value as SdkValue, WordOrder,
};

const STARTUP_TIMEOUT: Duration = Duration::from_secs(15);
const SAMPLE_TIMEOUT: Duration = Duration::from_secs(15);
const COMMAND_TIMEOUT: Duration = Duration::from_secs(15);
const LINK_TIMEOUT: Duration = Duration::from_secs(20);
/// Generous: the connector must first hit its own request bound (a conformance config leaves the
/// default, e.g. 5s for Modbus) before it can report anything.
const SILENT_PEER_TIMEOUT: Duration = Duration::from_secs(45);

/// A point of the connector configuration, resolved for assertions.
#[derive(Debug, Clone)]
struct Point {
    device: String,
    id: String,
    mode: Mode,
    datatype: Option<DataType>,
    endianness: Endianness,
    word_order: WordOrder,
    access: Access,
    unit: Option<String>,
    transform: Transform,
    meta: Option<serde_json::Value>,
    address: serde_json::Value,
    /// `start_bit`/`bit_count` from the address object, when the point is a bit-field.
    bitfield: Option<(u32, u32)>,
}

impl Point {
    fn spec(&self) -> PointSpec {
        PointSpec {
            address: self.address.clone(),
            datatype: self.datatype,
            mode: self.mode,
        }
    }

    fn sample_topic(&self, protocol: &str) -> String {
        format!(
            "te/device/{}/ot/{}/sample/{}",
            self.device, protocol, self.id
        )
    }

    fn cmd_topic(&self, protocol: &str, verb: &str, id: &str) -> String {
        format!(
            "te/device/{}/ot/{}/cmd/{verb}/{id}",
            self.device, protocol
        )
    }
}

fn resolve_points(config: &ConnectorConfig) -> Vec<Point> {
    let mut points = Vec::new();
    for device in &config.devices {
        for p in &device.points {
            let bitfield = match (
                p.address.get("start_bit").and_then(|v| v.as_u64()),
                p.address.get("bit_count").and_then(|v| v.as_u64()),
            ) {
                (Some(sb), Some(bc)) => Some((sb as u32, bc as u32)),
                _ => None,
            };
            points.push(Point {
                device: device.name.clone(),
                id: p.id.clone(),
                mode: p.resolved_mode(device.default_mode),
                datatype: p.datatype,
                endianness: Endianness::parse(p.endianness.as_deref()),
                word_order: WordOrder::parse(p.word_order.as_deref()),
                access: Access::parse(p.access.as_deref()),
                unit: p.unit.clone(),
                transform: p.transform.unwrap_or_default(),
                meta: p.meta.clone(),
                address: p.address.clone(),
                bitfield,
            });
        }
    }
    points
}

/// The expected decoded value for a point given the simulator's current bytes — the same
/// pipeline the connector must implement: primitive/bit-field decode, then transform.
///
/// Conformance configs must declare a `datatype` on every typed point (protocols like Modbus
/// tolerate omitting it on bit tables, but the harness needs it to compute ground truth).
fn expected_value(point: &Point, bytes: &[u8], _raw_group: usize) -> Result<Option<SdkValue>, String> {
    if point.mode == Mode::Raw {
        return Ok(None);
    }
    let value = if let Some((start_bit, bit_count)) = point.bitfield {
        let n = extract_bitfield(bytes, point.endianness, point.word_order, start_bit, bit_count);
        SdkValue::Number(n as f64)
    } else {
        let dt = point
            .datatype
            .ok_or_else(|| format!("typed point '{}' must declare a datatype", point.id))?;
        decode_primitive(bytes, dt, point.endianness, point.word_order)
            .map_err(|e| format!("cannot decode expected value for '{}': {e}", point.id))?
    };
    Ok(Some(point.transform.apply(value)))
}

fn sdk_value_to_json(v: &SdkValue) -> serde_json::Value {
    match v {
        SdkValue::Bool(b) => serde_json::json!(b),
        SdkValue::Number(n) => serde_json::json!(n),
        SdkValue::Text(t) => serde_json::json!(t),
    }
}

/// JSON value equality with a tight relative tolerance for numbers.
fn json_value_eq(a: &serde_json::Value, b: &serde_json::Value) -> bool {
    match (a.as_f64(), b.as_f64()) {
        (Some(x), Some(y)) => {
            x == y || (x - y).abs() <= 1e-9 * x.abs().max(y.abs()).max(1.0)
        }
        _ => a == b,
    }
}

struct Ctx<'a> {
    broker: &'a BrokerHandle,
    sim: &'a dyn Simulator,
    schemas: &'a Schemas,
    protocol: String,
    service: String,
    /// MQTT client id the SDK runtime uses; publishes from it are "the connector's".
    client: String,
    points: Vec<Point>,
}

impl Ctx<'_> {
    fn caps_topic(&self) -> String {
        format!("te/device/main/service/{}/ot/capabilities", self.service)
    }

    fn health_topic(&self) -> String {
        format!("te/device/main/service/{}/status/health", self.service)
    }

    fn link_topic(&self, device: &str) -> String {
        format!("te/device/{}/ot/{}/status/link", device, self.protocol)
    }

    async fn wait_connector_record(
        &self,
        from: usize,
        timeout: Duration,
        what: &str,
        pred: impl Fn(&Record) -> bool,
    ) -> Result<Record, String> {
        let client = self.client.clone();
        self.broker
            .wait_for(from, timeout, what, |r| r.client == client && pred(r))
            .await
    }
}

/// How long a connector is given to (wrongly) answer a command that is not its own.
const OWNERSHIP_SETTLE: Duration = Duration::from_secs(3);

/// B11 — command ownership (contract §6.5): a command for a device the connector's configuration
/// does not define is left unanswered. Every instance of a protocol on a broker receives it, and a
/// `failed` from one that does not own the device would overwrite the owner's retained result.
async fn check_b11_unowned_device_ignored(ctx: &Ctx<'_>, layer: &mut Layer) {
    let topic = format!(
        "te/device/conf-b11-not-configured/ot/{}/cmd/write/conf-b11",
        ctx.protocol
    );
    let mark = ctx.broker.mark();
    let request = serde_json::json!({ "status": "init", "point": "any", "value": 1 });
    ctx.broker.publish(&topic, request.to_string().as_bytes(), true);
    tokio::time::sleep(OWNERSHIP_SETTLE).await;
    let answered: Vec<String> = ctx
        .broker
        .records_from(mark)
        .into_iter()
        .filter(|r| r.client == ctx.client && r.topic == topic)
        .map(|r| r.json().map(|j| j.to_string()).unwrap_or_default())
        .collect();
    // The request is retained: clear it rather than leave it for later checks.
    ctx.broker.publish(&topic, b"", true);
    // Silence only counts from a connector that is still working: samples keep coming after the
    // command was given its time (a fresh mark — one published before a crash does not count).
    let settled = ctx.broker.mark();
    let alive = ctx
        .wait_connector_record(settled, SAMPLE_TIMEOUT, "a sample after the ownership check", |r| {
            r.topic.contains("/sample/")
        })
        .await;
    layer.check(
        "B11-ownership",
        "a command for a device the connector does not own is left unanswered",
        if let Err(reason) = alive {
            Err(format!("{reason} — the connector stopped publishing, so its silence proves nothing"))
        } else if answered.is_empty() {
            Ok(None)
        } else {
            Err(format!(
                "published {} transition(s) for a device it does not own: {}",
                answered.len(),
                answered.join(", ")
            ))
        },
    );
}

/// Run the behavioural layer. Returns the behavioural checks plus the captured-traffic schema
/// checks as separate report layers.
pub async fn run(manifest: &Manifest, schemas: &Schemas) -> Result<Vec<Layer>, String> {
    let mut layer = Layer::new("Layer 3 — behavioural conformance");

    let simulator = manifest
        .simulator
        .as_ref()
        .ok_or("behavioural layer requires a [simulator] section in the manifest")?;
    let config_template = manifest
        .harness
        .config
        .as_ref()
        .ok_or("behavioural layer requires `[harness] config` in the manifest")?;

    let sim = sim::build(&simulator.kind, &manifest.resolve(&simulator.seed)).await?;
    let broker = BrokerHandle::start().await?;
    let temp = TempDir::new()?;
    let (config_path, config) = rewrite_config(
        &manifest.resolve(config_template),
        temp.path(),
        broker.port(),
        sim.as_ref(),
    )?;

    let protocol = config.connector.protocol.clone();
    if protocol != manifest.connector.protocol {
        return Err(format!(
            "manifest protocol '{}' does not match config protocol '{protocol}'",
            manifest.connector.protocol
        ));
    }
    let service = config.connector.service_name();
    let ctx = Ctx {
        broker: &broker,
        sim: sim.as_ref(),
        schemas,
        client: format!("{service}-{protocol}"),
        protocol,
        service,
        points: resolve_points(&config),
    };

    let start_mark = broker.mark();
    let host = Host::start(
        &ctx.protocol,
        &manifest.harness.command,
        &config_path,
    )
    .await?;

    // B1 is load-bearing: without a started connector nothing else can run.
    let caps = match check_b1_startup(&ctx, &mut layer, start_mark).await {
        Some(caps) => caps,
        None => {
            host.stop().await;
            return Ok(vec![layer]);
        }
    };

    check_b9_manifest_agreement(manifest, &caps, &mut layer);
    check_b5_link_connected(&ctx, &mut layer, start_mark).await;
    check_b2_b3_b4_samples(&ctx, &mut layer, start_mark).await;
    check_b2_seq_monotonic(&ctx, &mut layer, start_mark).await;
    check_b6_write_roundtrip(&ctx, &mut layer).await;
    check_b7_access_control(&ctx, &mut layer).await;
    check_b8_hot_reload(&ctx, &mut layer, &config_path).await;
    check_b12_reporting_policy(&ctx, &mut layer, &config_path).await;
    check_b13_value_map(&ctx, &mut layer, &config_path).await;
    check_b11_unowned_device_ignored(&ctx, &mut layer).await;
    check_b5_link_drop_and_recovery(&ctx, &mut layer).await;
    check_b5_silent_peer(&ctx, &mut layer).await;

    // Shut down and verify the final health transition.
    let stop_mark = broker.mark();
    host.stop().await;
    let health_topic = ctx.health_topic();
    layer.check(
        "B1-shutdown",
        "service health transitions to 'down' on shutdown",
        ctx.wait_connector_record(stop_mark, STARTUP_TIMEOUT, "health down", |r| {
            r.topic == health_topic
                && r.json()
                    .ok()
                    .and_then(|j| j.get("status").and_then(|s| s.as_str()).map(|s| s == "down"))
                    .unwrap_or(false)
        })
        .await
        .map(|_| None),
    );

    check_b9_capability_honesty(&ctx, &caps, &mut layer, start_mark);
    check_b10_topic_discipline(&ctx, &mut layer, start_mark);

    let captured = validate_captured_traffic(&ctx, start_mark);
    Ok(vec![layer, captured])
}

/// B1 — startup: retained capability descriptor + service health `up`.
async fn check_b1_startup(ctx: &Ctx<'_>, layer: &mut Layer, from: usize) -> Option<serde_json::Value> {
    let caps_topic = ctx.caps_topic();
    let caps_record = ctx
        .wait_connector_record(from, STARTUP_TIMEOUT, "capability descriptor", |r| {
            r.topic == caps_topic && r.retain
        })
        .await;

    let caps = match caps_record {
        Ok(record) => match record.json() {
            Ok(json) => {
                layer.pass("B1-capabilities", "retained capability descriptor published", None);
                Some(json)
            }
            Err(e) => {
                layer.fail("B1-capabilities", "retained capability descriptor published", e);
                None
            }
        },
        Err(e) => {
            layer.fail(
                "B1-capabilities",
                "retained capability descriptor published",
                format!("{e}; the connector did not start against the harness broker"),
            );
            None
        }
    };

    let health_topic = ctx.health_topic();
    layer.check(
        "B1-health",
        "retained service health 'up' published",
        ctx.wait_connector_record(from, STARTUP_TIMEOUT, "health up", |r| {
            r.topic == health_topic
                && r.retain
                && r.json()
                    .ok()
                    .and_then(|j| j.get("status").and_then(|s| s.as_str()).map(|s| s == "up"))
                    .unwrap_or(false)
        })
        .await
        .map(|_| None),
    );

    caps
}

/// The SDK runtime adds these to every connector's live capability descriptor; they are
/// treated as implicitly declared by every manifest.
const SDK_VERBS: [&str; 3] = ["set-config", "define-device", "remove-device"];
const SDK_FEATURE: &str = "management";
/// The SDK runtime implements `write-batch` on top of a module's `write`, so every manifest
/// that declares `write` implicitly declares `write-batch` too.
const SDK_BATCH_VERB: &str = "write-batch";

/// Compare a manifest's claims against a capability descriptor that already carries the SDK
/// management augmentation (the live retained descriptor, or raw module capabilities passed
/// through [`augment_caps`]). Empty result = agreement.
pub(crate) fn manifest_caps_mismatches(
    manifest: &Manifest,
    caps: &serde_json::Value,
) -> Vec<String> {
    let mut mismatches = Vec::new();
    let set = |key: &str| -> BTreeSet<String> {
        caps.get(key)
            .and_then(|v| v.as_array())
            .map(|a| {
                a.iter()
                    .filter_map(|x| x.as_str().map(str::to_string))
                    .collect()
            })
            .unwrap_or_default()
    };

    let compare = |what: &str, claimed: BTreeSet<String>, live: BTreeSet<String>, out: &mut Vec<String>| {
        if claimed != live {
            out.push(format!(
                "{what}: manifest claims {claimed:?}, capability descriptor says {live:?}"
            ));
        }
    };

    if caps.get("protocol").and_then(|p| p.as_str()) != Some(manifest.connector.protocol.as_str()) {
        mismatches.push(format!(
            "protocol: manifest '{}' vs descriptor {:?}",
            manifest.connector.protocol,
            caps.get("protocol")
        ));
    }
    compare(
        "modes",
        manifest.connector.modes.iter().cloned().collect(),
        set("modes"),
        &mut mismatches,
    );
    compare(
        "datatypes",
        manifest.connector.datatypes.iter().cloned().collect(),
        set("datatypes"),
        &mut mismatches,
    );
    let mut claimed_verbs: BTreeSet<String> = manifest.connector.verbs.iter().cloned().collect();
    claimed_verbs.extend(SDK_VERBS.iter().map(|s| s.to_string()));
    if claimed_verbs.contains("write") {
        claimed_verbs.insert(SDK_BATCH_VERB.to_string());
    }
    compare("command_verbs", claimed_verbs, set("command_verbs"), &mut mismatches);
    let mut claimed_features: BTreeSet<String> =
        manifest.connector.features.iter().cloned().collect();
    claimed_features.insert(SDK_FEATURE.to_string());
    compare("features", claimed_features, set("features"), &mut mismatches);
    if !manifest.connector.point_kinds.is_empty() {
        compare(
            "point_kinds",
            manifest.connector.point_kinds.iter().cloned().collect(),
            set("point_kinds"),
            &mut mismatches,
        );
    }
    let live_subscribe = caps.get("subscribe").and_then(|s| s.as_bool()).unwrap_or(false);
    if live_subscribe != manifest.connector.subscribe {
        mismatches.push(format!(
            "subscribe: manifest {} vs descriptor {live_subscribe}",
            manifest.connector.subscribe
        ));
    }
    mismatches
}

/// Apply the SDK runtime's management augmentation to a raw module capability descriptor, so
/// it compares like the live retained one.
pub(crate) fn augment_caps(caps: &mut serde_json::Value) {
    let add = |list: &mut serde_json::Value, items: &[&str]| {
        if let Some(array) = list.as_array_mut() {
            for item in items {
                if !array.iter().any(|x| x == item) {
                    array.push(serde_json::json!(item));
                }
            }
        }
    };
    add(&mut caps["command_verbs"], &SDK_VERBS);
    let has_write = caps["command_verbs"]
        .as_array()
        .map(|a| a.iter().any(|v| v == "write"))
        .unwrap_or(false);
    if has_write {
        add(&mut caps["command_verbs"], &[SDK_BATCH_VERB]);
    }
    add(&mut caps["features"], &[SDK_FEATURE]);
}

/// B9 (static half) — the manifest and the live capability descriptor MUST agree.
fn check_b9_manifest_agreement(manifest: &Manifest, caps: &serde_json::Value, layer: &mut Layer) {
    let mismatches = manifest_caps_mismatches(manifest, caps);
    if mismatches.is_empty() {
        layer.pass(
            "B9-manifest",
            "conformance manifest agrees with the live capability descriptor",
            None,
        );
    } else {
        layer.fail(
            "B9-manifest",
            "conformance manifest agrees with the live capability descriptor",
            mismatches.join("\n"),
        );
    }
}

/// B5 (first half) — every configured device's link transitions to `connected` at startup.
async fn check_b5_link_connected(ctx: &Ctx<'_>, layer: &mut Layer, from: usize) {
    let devices: BTreeSet<String> = ctx.points.iter().map(|p| p.device.clone()).collect();
    for device in devices {
        let topic = ctx.link_topic(&device);
        layer.check(
            &format!("B5-connected-{device}"),
            &format!("link status for '{device}' becomes 'connected' (retained)"),
            ctx.wait_connector_record(from, STARTUP_TIMEOUT, "link connected", |r| {
                r.topic == topic
                    && r.retain
                    && r.json()
                        .ok()
                        .and_then(|j| {
                            j.get("status").and_then(|s| s.as_str()).map(|s| s == "connected")
                        })
                        .unwrap_or(false)
            })
            .await
            .map(|_| None),
        );
    }
}

/// B2/B3/B4 — every configured point publishes a contract-conformant sample whose value
/// matches the simulator's seeded data; typed vs raw shape; seeded-invalid points go `bad`.
async fn check_b2_b3_b4_samples(ctx: &Ctx<'_>, layer: &mut Layer, from: usize) {
    let mut saw_typed_value = false;
    let mut saw_raw_only = false;
    let mut saw_bad = false;

    for point in &ctx.points {
        let topic = point.sample_topic(&ctx.protocol);
        let invalid = ctx.sim.is_invalid(&point.spec());
        let record = ctx
            .wait_connector_record(from, SAMPLE_TIMEOUT, "sample", |r| r.topic == topic)
            .await;
        let id = format!("B2-sample-{}", point.id);
        let name = format!(
            "point '{}' publishes a conformant {} sample",
            point.id,
            if invalid { "bad-quality" } else { "good" }
        );
        let outcome = match record {
            Ok(record) => assert_sample(ctx, point, invalid, &record),
            Err(e) => Err(e),
        };
        if let Ok(detail) = &outcome {
            if invalid {
                saw_bad = true;
            } else if point.mode == Mode::Typed {
                saw_typed_value = true;
            } else {
                saw_raw_only = true;
            }
            let _ = detail;
        }
        layer.check(&id, &name, outcome);
    }

    // B3 — both modes exercised with the right envelope shape.
    if ctx.manifest_has_mode("typed") {
        report_mode_probe(layer, "B3-typed", "a typed point yields value + value_repr", saw_typed_value);
    }
    if ctx.manifest_has_mode("raw") {
        report_mode_probe(layer, "B3-raw", "a raw point yields raw only (no value)", saw_raw_only);
    }
    // B4 — a simulated read failure yields a bad sample, not silence.
    report_mode_probe(
        layer,
        "B4-bad-quality",
        "a seeded read failure yields quality 'bad' with an error, not a dropped message",
        saw_bad,
    );
}

fn report_mode_probe(layer: &mut Layer, id: &str, name: &str, ok: bool) {
    if ok {
        layer.pass(id, name, None);
    } else {
        layer.fail(
            id,
            name,
            "no point in the conformance config exercised this behaviour (fix the config or the connector)".into(),
        );
    }
}

impl Ctx<'_> {
    fn manifest_has_mode(&self, mode: &str) -> bool {
        // the config decides which modes are exercised; only require what the config contains
        self.points.iter().any(|p| match mode {
            "typed" => p.mode == Mode::Typed && !self.sim.is_invalid(&p.spec()),
            "raw" => p.mode == Mode::Raw && !self.sim.is_invalid(&p.spec()),
            _ => false,
        })
    }
}

/// Assert one captured sample against the contract and the simulator's ground truth.
fn assert_sample(
    ctx: &Ctx<'_>,
    point: &Point,
    invalid: bool,
    record: &Record,
) -> Result<Option<String>, String> {
    let json = record.json()?;
    ctx.schemas
        .validate(Kind::Sample, &json)
        .map_err(|e| format!("sample schema violation: {e}"))?;
    if record.retain {
        return Err("samples must not be retained".into());
    }

    let mut errors = Vec::new();
    let field = |k: &str| json.get(k).cloned().unwrap_or(serde_json::Value::Null);

    if field("device") != serde_json::json!(point.device) {
        errors.push(format!("device echo: {:?}", field("device")));
    }
    if field("point") != serde_json::json!(point.id) {
        errors.push(format!("point echo: {:?}", field("point")));
    }
    if field("protocol") != serde_json::json!(ctx.protocol) {
        errors.push(format!("protocol echo: {:?}", field("protocol")));
    }
    let expected_mode = match point.mode {
        Mode::Raw => "raw",
        Mode::Typed => "typed",
    };
    if field("mode") != serde_json::json!(expected_mode) {
        errors.push(format!("mode: expected '{expected_mode}', got {:?}", field("mode")));
    }
    let ts = field("ts");
    let ts_str = ts.as_str().unwrap_or_default();
    if !(ts_str.len() >= 24 && ts_str.ends_with('Z') && ts_str.as_bytes().get(19) == Some(&b'.')) {
        errors.push(format!(
            "ts must be RFC 3339 with millisecond precision and 'Z': got '{ts_str}'"
        ));
    }

    if invalid {
        if field("quality") != serde_json::json!("bad") {
            errors.push(format!("quality: expected 'bad', got {:?}", field("quality")));
        }
        if field("error").as_str().map(str::is_empty).unwrap_or(true) {
            errors.push("a bad sample must carry a non-empty 'error'".into());
        }
        if !field("value").is_null() {
            errors.push("a bad sample must not carry a 'value'".into());
        }
    } else {
        if field("quality") != serde_json::json!("good") {
            errors.push(format!("quality: expected 'good', got {:?}", field("quality")));
        }
        match ctx.sim.point_data(&point.spec()) {
            Ok(data) => {
                let expected_raw = hex_grouped(&data.bytes, data.raw_group);
                if field("raw") != serde_json::json!(expected_raw) {
                    errors.push(format!(
                        "raw: expected '{expected_raw}', got {:?}",
                        field("raw")
                    ));
                }
                match expected_value(point, &data.bytes, data.raw_group) {
                    Ok(Some(expected)) => {
                        let expected_json = sdk_value_to_json(&expected);
                        if !json_value_eq(&field("value"), &expected_json) {
                            errors.push(format!(
                                "value: expected {expected_json}, got {}",
                                field("value")
                            ));
                        }
                        if field("value_repr") != serde_json::json!(expected.repr()) {
                            errors.push(format!(
                                "value_repr: expected '{}', got {:?}",
                                expected.repr(),
                                field("value_repr")
                            ));
                        }
                    }
                    Ok(None) => {
                        if !field("value").is_null() || !field("value_repr").is_null() {
                            errors.push("raw mode must not carry value/value_repr".into());
                        }
                    }
                    Err(e) => errors.push(e),
                }
            }
            Err(e) => errors.push(format!("simulator has no data for the point: {e}")),
        }
        if let Some(unit) = &point.unit {
            if field("unit") != serde_json::json!(unit) {
                errors.push(format!("unit echo: expected '{unit}', got {:?}", field("unit")));
            }
        }
        if let Some(meta) = &point.meta {
            if &field("meta") != meta {
                errors.push(format!(
                    "meta echo: expected {meta}, got {}",
                    field("meta")
                ));
            }
        }
    }

    if errors.is_empty() {
        Ok(None)
    } else {
        Err(errors.join("\n"))
    }
}

/// B2 (seq) — per-point `seq` increases monotonically across consecutive samples.
async fn check_b2_seq_monotonic(ctx: &Ctx<'_>, layer: &mut Layer, from: usize) {
    let Some(point) = ctx
        .points
        .iter()
        .find(|p| p.mode == Mode::Typed && !ctx.sim.is_invalid(&p.spec()))
    else {
        layer.skip("B2-seq", "per-point seq is monotonic", "no good typed point configured".into());
        return;
    };
    let topic = point.sample_topic(&ctx.protocol);
    // wait until at least 3 samples of the point were captured
    let mut seen = 0usize;
    let mut cursor = from;
    for _ in 0..3 {
        match ctx
            .wait_connector_record(cursor, SAMPLE_TIMEOUT, "sample for seq check", |r| {
                r.topic == topic
            })
            .await
        {
            Ok(r) => {
                cursor = r.seq + 1;
                seen += 1;
            }
            Err(_) => break,
        }
    }
    if seen < 3 {
        layer.fail(
            "B2-seq",
            "per-point seq is monotonic",
            format!("only {seen} samples of '{}' observed", point.id),
        );
        return;
    }
    let seqs: Vec<i64> = ctx
        .broker
        .records_from(from)
        .into_iter()
        .filter(|r| r.client == ctx.client && r.topic == topic)
        .filter_map(|r| r.json().ok())
        .filter_map(|j| j.get("seq").and_then(|s| s.as_i64()))
        .collect();
    let monotonic = seqs.windows(2).all(|w| w[1] > w[0]);
    if seqs.len() >= 3 && monotonic {
        layer.pass("B2-seq", "per-point seq is monotonic", Some(format!("observed {seqs:?}")));
    } else {
        layer.fail(
            "B2-seq",
            "per-point seq is monotonic",
            format!("observed seq values {seqs:?}"),
        );
    }
}

/// The probe value the harness writes for B6, per datatype.
fn write_probe(point: &Point) -> Option<(serde_json::Value, &'static str, SdkValue)> {
    match point.datatype {
        Some(DataType::Bool) => Some((serde_json::json!(true), "boolean", SdkValue::Bool(true))),
        Some(DataType::Float32) | Some(DataType::Float64) => Some((
            serde_json::json!(99.5),
            "number",
            SdkValue::Number(99.5),
        )),
        Some(DataType::Int8) | Some(DataType::Uint8) => {
            Some((serde_json::json!(42), "number", SdkValue::Number(42.0)))
        }
        Some(DataType::String) => Some((
            serde_json::json!("conformance-probe"),
            "string",
            SdkValue::Text("conformance-probe".to_string()),
        )),
        Some(DataType::Bytes) => None,
        Some(_) => Some((serde_json::json!(12345), "number", SdkValue::Number(12345.0))),
        None => None,
    }
}

/// B6 — a `write` init drives `executing` → `successful`, the simulator observes the written
/// value, and it round-trips through a subsequent read.
async fn check_b6_write_roundtrip(ctx: &Ctx<'_>, layer: &mut Layer) {
    let writable: Vec<&Point> = ctx
        .points
        .iter()
        .filter(|p| p.access.can_write() && p.mode == Mode::Typed && p.bitfield.is_none())
        .collect();
    if writable.is_empty() {
        layer.fail(
            "B6-write",
            "write verb round-trips",
            "the conformance config declares no writable typed point".into(),
        );
        return;
    }

    for point in writable {
        let id = format!("B6-write-{}", point.id);
        let name = format!("write to '{}' executes and round-trips", point.id);
        let Some((json_value, repr, sdk_value)) = write_probe(point) else {
            layer.skip(&id, &name, "no probe value for the datatype".into());
            continue;
        };
        let topic = point.cmd_topic(&ctx.protocol, "write", &format!("conf-{}", point.id));
        let mark = ctx.broker.mark();
        ctx.broker.publish(
            &topic,
            serde_json::json!({
                "status": "init",
                "point": point.id,
                "value": json_value,
                "value_repr": repr,
            })
            .to_string()
            .as_bytes(),
            true,
        );

        let result = async {
            ctx.wait_connector_record(mark, COMMAND_TIMEOUT, "status 'executing'", |r| {
                r.topic == topic
                    && r.retain
                    && r.json().ok().map(|j| j["status"] == "executing").unwrap_or(false)
            })
            .await?;
            ctx.wait_connector_record(mark, COMMAND_TIMEOUT, "status 'successful'", |r| {
                r.topic == topic
                    && r.retain
                    && r.json().ok().map(|j| j["status"] == "successful").unwrap_or(false)
            })
            .await?;

            // the simulator must have seen a protocol write with the new value
            let writes = ctx.sim.write_count(&point.spec())?;
            if writes == 0 {
                return Err("connector reported success but the simulator saw no write".into());
            }
            let data = ctx
                .sim
                .point_data(&point.spec())
                .map_err(|e| format!("simulator: {e}"))?;
            let now = expected_value(point, &data.bytes, data.raw_group)?
                .map(|v| sdk_value_to_json(&v))
                .unwrap_or(serde_json::Value::Null);
            let want = sdk_value_to_json(&sdk_value);
            if !json_value_eq(&now, &want) {
                return Err(format!(
                    "simulator holds {now} after the write, expected {want}"
                ));
            }

            // and the new value must round-trip through a subsequent read
            let sample_topic = point.sample_topic(&ctx.protocol);
            ctx.wait_connector_record(mark, SAMPLE_TIMEOUT, "read-back sample", |r| {
                r.topic == sample_topic
                    && r.json()
                        .ok()
                        .map(|j| json_value_eq(&j["value"], &want))
                        .unwrap_or(false)
            })
            .await?;
            Ok(None)
        }
        .await;
        layer.check(&id, &name, result);
    }
}

/// B7 — a write to a read-only point fails with a reason and never reaches the simulator.
async fn check_b7_access_control(ctx: &Ctx<'_>, layer: &mut Layer) {
    let Some(point) = ctx
        .points
        .iter()
        .find(|p| p.access == Access::Read && p.mode == Mode::Typed && !ctx.sim.is_invalid(&p.spec()))
    else {
        layer.fail(
            "B7-access",
            "write to a read-only point is rejected",
            "the conformance config declares no read-only typed point".into(),
        );
        return;
    };

    let topic = point.cmd_topic(&ctx.protocol, "write", "conf-denied");
    let mark = ctx.broker.mark();
    let before = ctx.sim.write_count(&point.spec()).unwrap_or(0);
    ctx.broker.publish(
        &topic,
        serde_json::json!({
            "status": "init",
            "point": point.id,
            "value": 1,
            "value_repr": "number",
        })
        .to_string()
        .as_bytes(),
        true,
    );

    let result = async {
        let failed = ctx
            .wait_connector_record(mark, COMMAND_TIMEOUT, "status 'failed'", |r| {
                r.topic == topic
                    && r.json().ok().map(|j| j["status"] == "failed").unwrap_or(false)
            })
            .await?;
        let json = failed.json()?;
        if json
            .get("reason")
            .and_then(|r| r.as_str())
            .map(str::is_empty)
            .unwrap_or(true)
        {
            return Err("failed status must carry a non-empty 'reason'".into());
        }
        let after = ctx.sim.write_count(&point.spec())?;
        if after != before {
            return Err(format!(
                "the simulator observed {} write(s) despite the denial",
                after - before
            ));
        }
        Ok(None)
    }
    .await;
    layer.check(
        "B7-access",
        &format!("write to read-only point '{}' fails and never reaches the device", point.id),
        result,
    );
}

/// B8 — adding a point through the management interface is picked up without a restart:
/// the new point starts publishing samples.
async fn check_b8_hot_reload(ctx: &Ctx<'_>, layer: &mut Layer, config_path: &std::path::Path) {
    const NEW_POINT: &str = "b8-hot-reload";
    let result = async {
        // Clone the running device definition from the (rewritten) config and add a point
        // that mirrors an existing good typed point under a new id.
        let text = std::fs::read_to_string(config_path)
            .map_err(|e| format!("read {}: {e}", config_path.display()))?;
        let doc: toml::Value = toml::from_str(&text).map_err(|e| format!("parse config: {e}"))?;
        let device_toml = doc
            .get("device")
            .and_then(|d| d.as_array())
            .and_then(|a| a.first())
            .ok_or("config has no [[device]]")?;
        let mut device: serde_json::Value =
            serde_json::to_value(device_toml).map_err(|e| format!("device to JSON: {e}"))?;

        let template = ctx
            .points
            .iter()
            .find(|p| p.mode == Mode::Typed && !ctx.sim.is_invalid(&p.spec()) && p.bitfield.is_none())
            .ok_or("no good typed point to mirror")?;
        let points = device
            .get_mut("point")
            .and_then(|p| p.as_array_mut())
            .ok_or("device has no points array")?;
        let mut clone = points
            .iter()
            .find(|p| p["id"] == serde_json::json!(template.id))
            .cloned()
            .ok_or("template point not found in config")?;
        clone["id"] = serde_json::json!(NEW_POINT);
        points.push(clone);

        let device_name = device["name"].as_str().unwrap_or_default().to_string();
        // Management verbs are addressed to the connector service (contract §6.3).
        let topic = format!(
            "te/device/main/service/{}/ot/cmd/define-device/conf-b8",
            ctx.service
        );
        let mark = ctx.broker.mark();
        ctx.broker.publish(
            &topic,
            serde_json::json!({ "status": "init", "device": device })
                .to_string()
                .as_bytes(),
            true,
        );
        ctx.wait_connector_record(mark, COMMAND_TIMEOUT, "define-device 'successful'", |r| {
            r.topic == topic
                && r.json().ok().map(|j| j["status"] == "successful").unwrap_or(false)
        })
        .await?;

        // the new point must start publishing without any restart
        let sample_topic = format!(
            "te/device/{}/ot/{}/sample/{}",
            device_name, ctx.protocol, NEW_POINT
        );
        let sample = ctx
            .wait_connector_record(mark, SAMPLE_TIMEOUT, "sample from the new point", |r| {
                r.topic == sample_topic
            })
            .await?;
        let json = sample.json()?;
        if json["quality"] != "good" {
            return Err(format!("new point published quality {:?}", json["quality"]));
        }
        Ok(Some(format!("point '{NEW_POINT}' live after define-device")))
    }
    .await;
    layer.check(
        "B8-hot-reload",
        "a config change (added point) is picked up without restart",
        result,
    );
}

/// B12 — the reporting policy (contract §5.3) is applied by the runtime: a point with
/// `report = { on_change = true, max_interval }` publishes its first reading, withholds the
/// unchanged readings after it (without spending `seq` numbers on them), publishes a fresh
/// reading as a heartbeat once `max_interval` has passed, and the capability descriptor lists
/// its policy. The point is a clone of a good typed point whose value the simulator holds
/// still, added with define-device like B8, so it runs on every connector: a subscribe-capable
/// one delivers the clone by push, and then the heartbeat is the runtime's on-demand read.
async fn check_b12_reporting_policy(ctx: &Ctx<'_>, layer: &mut Layer, config_path: &std::path::Path) {
    const POINT: &str = "b12-report";
    const QUIET: Duration = Duration::from_secs(2);
    const HEARTBEAT: &str = "4s";
    const IDS: [(&str, &str); 3] = [
        ("B12-report-on-change", "unchanged readings are withheld by report.on_change"),
        ("B12-report-heartbeat", "report.max_interval publishes a fresh reading, with the next seq"),
        ("B12-report-descriptor", "the capability descriptor lists the point's reporting policy"),
    ];
    let skip = |layer: &mut Layer, reason: String| {
        for (id, name) in IDS {
            layer.skip(id, name, reason.clone());
        }
    };

    // A template whose value does not move: two captured samples with the same value.
    let records = ctx.broker.records_from(0);
    let steady = ctx.points.iter().find(|p| {
        if p.mode != Mode::Typed || ctx.sim.is_invalid(&p.spec()) || p.bitfield.is_some() || p.access != Access::Read {
            return false;
        }
        let topic = p.sample_topic(&ctx.protocol);
        let values: Vec<serde_json::Value> = records
            .iter()
            .filter(|r| r.client == ctx.client && r.topic == topic)
            .filter_map(|r| r.json().ok())
            .filter(|j| j["quality"] == "good")
            .map(|j| j["value"].clone())
            .collect();
        values.len() >= 2 && values[values.len() - 1] == values[values.len() - 2]
    });
    let Some(template) = steady else {
        skip(layer, "no read-only good typed point with a steady value was observed".into());
        return;
    };

    let caps_topic = ctx.caps_topic();
    let pushed = records
        .iter()
        .rev()
        .filter(|r| r.client == ctx.client && r.topic == caps_topic)
        .find_map(|r| r.json().ok())
        .is_some_and(|caps| caps["subscribe"] == true);

    let setup = async {
        let text = std::fs::read_to_string(config_path)
            .map_err(|e| format!("read {}: {e}", config_path.display()))?;
        let doc: toml::Value = toml::from_str(&text).map_err(|e| format!("parse config: {e}"))?;
        let device_toml = doc
            .get("device")
            .and_then(|d| d.as_array())
            .and_then(|a| a.iter().find(|d| d.get("name").and_then(|n| n.as_str()) == Some(template.device.as_str())))
            .ok_or("the template's device is not in the config")?;
        let original: serde_json::Value =
            serde_json::to_value(device_toml).map_err(|e| format!("device to JSON: {e}"))?;
        let mut device = original.clone();
        let points = device
            .get_mut("point")
            .and_then(|p| p.as_array_mut())
            .ok_or("device has no points array")?;
        let mut clone = points
            .iter()
            .find(|p| p["id"] == serde_json::json!(template.id))
            .cloned()
            .ok_or("template point not found in config")?;
        clone["id"] = serde_json::json!(POINT);
        clone["poll_interval"] = serde_json::json!("250ms");
        // Pushed when the connector can: a static node then never pushes again, so the
        // heartbeat can only be the runtime's on-demand read.
        if pushed {
            clone["subscribe"] = serde_json::json!(true);
        }
        clone["report"] = serde_json::json!({ "on_change": true, "max_interval": HEARTBEAT });
        points.push(clone);
        Ok::<_, String>((original, device))
    }
    .await;
    let (original, device) = match setup {
        Ok(pair) => pair,
        Err(e) => {
            for (id, name) in IDS {
                layer.fail(id, name, e.clone());
            }
            return;
        }
    };

    let define = |device: &serde_json::Value, id: &str| {
        let topic = format!("te/device/main/service/{}/ot/cmd/define-device/{id}", ctx.service);
        let mark = ctx.broker.mark();
        ctx.broker.publish(
            &topic,
            serde_json::json!({ "status": "init", "device": device }).to_string().as_bytes(),
            true,
        );
        (topic, mark)
    };
    let (topic, mark) = define(&device, "conf-b12");
    let defined = ctx
        .wait_connector_record(mark, COMMAND_TIMEOUT, "define-device 'successful'", |r| {
            r.topic == topic && r.json().ok().map(|j| j["status"] == "successful").unwrap_or(false)
        })
        .await;
    if let Err(e) = defined {
        for (id, name) in IDS {
            layer.fail(id, name, e.clone());
        }
        return;
    }

    let sample_topic = format!("te/device/{}/ot/{}/sample/{POINT}", template.device, ctx.protocol);
    let first = ctx
        .wait_connector_record(mark, SAMPLE_TIMEOUT, "first sample of the reported point", |r| {
            r.topic == sample_topic
        })
        .await;
    let quiet_and_heartbeat = match first {
        Err(e) => Err(e),
        Ok(first) => {
            let first_seq = first.json().ok().and_then(|j| j["seq"].as_i64());
            tokio::time::sleep(QUIET).await;
            let during: Vec<Record> = ctx
                .broker
                .records_from(first.seq + 1)
                .into_iter()
                .filter(|r| r.client == ctx.client && r.topic == sample_topic)
                .collect();
            let on_change = if during.is_empty() {
                Ok(Some(format!("no sample in the {}s after the first reading", QUIET.as_secs())))
            } else {
                Err(format!("{} unchanged reading(s) were published", during.len()))
            };
            let heartbeat = ctx
                .wait_connector_record(first.seq + 1, SAMPLE_TIMEOUT, "heartbeat sample", |r| {
                    r.topic == sample_topic
                })
                .await
                .and_then(|r| {
                    let json = r.json()?;
                    if json["quality"] != "good" {
                        return Err(format!("heartbeat published quality {:?}", json["quality"]));
                    }
                    let seq = json["seq"].as_i64();
                    match (first_seq, seq) {
                        (Some(a), Some(b)) if b == a + 1 => Ok(Some(format!(
                            "seq {a} then {b} ({})",
                            if pushed { "pushed: an on-demand read" } else { "polled" }
                        ))),
                        (a, b) => Err(format!("seq {a:?} then {b:?}: withheld readings must not consume seq")),
                    }
                });
            Ok((on_change, heartbeat))
        }
    };
    match quiet_and_heartbeat {
        Ok((on_change, heartbeat)) => {
            layer.check(IDS[0].0, IDS[0].1, on_change);
            layer.check(IDS[1].0, IDS[1].1, heartbeat);
        }
        Err(e) => {
            layer.fail(IDS[0].0, IDS[0].1, e.clone());
            layer.fail(IDS[1].0, IDS[1].1, e);
        }
    }

    let descriptor = ctx
        .broker
        .records_from(mark)
        .into_iter()
        .rev()
        .filter(|r| r.client == ctx.client && r.topic == caps_topic)
        .find_map(|r| r.json().ok())
        .ok_or_else(|| "no capability descriptor was republished after define-device".to_string())
        .and_then(|caps| {
            let listed = caps["reports"]["points"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|p| p["device"] == serde_json::json!(template.device) && p["point"] == POINT)
                .map(|p| p["report"].clone())
                .ok_or_else(|| format!("reports.points has no entry for '{POINT}': {}", caps["reports"]))?;
            if listed["on_change"] == true && listed["max_interval"] == HEARTBEAT {
                Ok(Some(format!("{listed}")))
            } else {
                Err(format!("reports entry is {listed}"))
            }
        });
    layer.check(IDS[2].0, IDS[2].1, descriptor);

    // Put the device back as it was, so the fast-polled clone does not run through the
    // outage checks that follow.
    let (topic, mark) = define(&original, "conf-b12-restore");
    let _ = ctx
        .wait_connector_record(mark, COMMAND_TIMEOUT, "define-device 'successful'", |r| {
            r.topic == topic && r.json().ok().map(|j| j["status"] == "successful").unwrap_or(false)
        })
        .await;
}

/// B13 — the value map (contract §4.3) is applied by the runtime, both ways: a clone of a
/// writable numeric point, mapped code-to-label, publishes labels with the code as
/// `source_value`; a write of a label reaches the simulator as its code and reads back as the
/// label; a label with no inverse fails without a protocol write; and a value the map has no
/// output for is a bad sample that does not count against the link — shown on a device of its
/// own whose only point is unmatched, which must still report `connected`.
async fn check_b13_value_map(ctx: &Ctx<'_>, layer: &mut Layer, config_path: &std::path::Path) {
    const POINT: &str = "b13-map";
    const UNMAPPED: &str = "b13-unmapped";
    const LONE: &str = "b13-lone-device";
    const IDS: [(&str, &str); 4] = [
        ("B13-map-read", "a mapped point publishes the label, with the code as source_value"),
        ("B13-map-write", "a label write reaches the device as its code and reads back as the label"),
        ("B13-map-refused", "a label with no inverse fails without reaching the device"),
        ("B13-map-unmatched", "an unmatched value is a bad sample that leaves the link connected"),
    ];
    let fail_all = |layer: &mut Layer, reason: String| {
        for (id, name) in IDS {
            layer.fail(id, name, reason.clone());
        }
    };

    let template = ctx.points.iter().find(|p| {
        p.mode == Mode::Typed
            && p.access.can_write()
            && p.bitfield.is_none()
            && !ctx.sim.is_invalid(&p.spec())
            && matches!(
                p.datatype,
                Some(DataType::Int8 | DataType::Uint8 | DataType::Int16 | DataType::Uint16 | DataType::Int32 | DataType::Uint32 | DataType::Float32 | DataType::Float64)
            )
    });
    let Some(template) = template else {
        for (id, name) in IDS {
            layer.skip(id, name, "no writable numeric typed point to clone".into());
        }
        return;
    };
    // Two codes the datatype holds exactly, in engineering units: the B6 probe and a neighbour.
    let (probe, target) = match template.datatype {
        Some(DataType::Float32 | DataType::Float64) => (99.5, 100.5),
        Some(DataType::Int8 | DataType::Uint8) => (42.0, 41.0),
        _ => (12345.0, 12344.0),
    };
    let code = |n: f64| -> serde_json::Value {
        if template.datatype.is_some_and(DataType::is_integer) {
            serde_json::json!(n as i64)
        } else {
            serde_json::json!(n)
        }
    };

    let setup = async {
        let text = std::fs::read_to_string(config_path)
            .map_err(|e| format!("read {}: {e}", config_path.display()))?;
        let doc: toml::Value = toml::from_str(&text).map_err(|e| format!("parse config: {e}"))?;
        let device_toml = doc
            .get("device")
            .and_then(|d| d.as_array())
            .and_then(|a| a.iter().find(|d| d.get("name").and_then(|n| n.as_str()) == Some(template.device.as_str())))
            .ok_or("the template's device is not in the config")?;
        let original: serde_json::Value =
            serde_json::to_value(device_toml).map_err(|e| format!("device to JSON: {e}"))?;
        let template_point = original["point"]
            .as_array()
            .and_then(|points| points.iter().find(|p| p["id"] == serde_json::json!(template.id)))
            .cloned()
            .ok_or("template point not found in config")?;
        let mut mapped = template_point.clone();
        mapped["id"] = serde_json::json!(POINT);
        mapped["poll_interval"] = serde_json::json!("250ms");
        mapped["subscribe"] = serde_json::json!(false);
        mapped["map"] = serde_json::json!({
            "cases": [
                { "eq": code(probe), "to": "probe" },
                { "eq": code(target), "to": "target" },
                { "min": -1e12, "max": 1e12, "to": "other" },
            ],
        });
        let mut device = original.clone();
        device["point"]
            .as_array_mut()
            .ok_or("device has no points array")?
            .push(mapped);

        // A device of its own whose only point maps nothing it can read.
        let mut unmapped = template_point;
        unmapped["id"] = serde_json::json!(UNMAPPED);
        unmapped["poll_interval"] = serde_json::json!("250ms");
        unmapped["subscribe"] = serde_json::json!(false);
        unmapped["access"] = serde_json::json!("read");
        unmapped["map"] = serde_json::json!({ "cases": [{ "eq": code(7.0), "to": "never" }] });
        let mut lone = original.clone();
        lone["name"] = serde_json::json!(LONE);
        lone["point"] = serde_json::json!([unmapped]);
        if let Some(table) = lone.as_object_mut() {
            table.remove("points_from");
        }
        Ok::<_, String>((original, device, lone))
    }
    .await;
    let (original, device, lone) = match setup {
        Ok(devices) => devices,
        Err(e) => return fail_all(layer, e),
    };

    let command = |verb: &str, payload: serde_json::Value, id: &str| {
        let topic = format!("te/device/main/service/{}/ot/cmd/{verb}/{id}", ctx.service);
        let mark = ctx.broker.mark();
        ctx.broker.publish(&topic, payload.to_string().as_bytes(), true);
        (topic, mark)
    };
    let terminal = |topic: String, mark: usize| async move {
        ctx.wait_connector_record(mark, COMMAND_TIMEOUT, "a terminal status", |r| {
            r.topic == topic
                && r.json()
                    .ok()
                    .map(|j| j["status"] == "successful" || j["status"] == "failed")
                    .unwrap_or(false)
        })
        .await
        .and_then(|r| r.json())
    };
    let (topic, mark) = command(
        "define-device",
        serde_json::json!({ "status": "init", "device": device }),
        "conf-b13",
    );
    match terminal(topic, mark).await {
        Ok(j) if j["status"] == "successful" => {}
        Ok(j) => return fail_all(layer, format!("define-device failed: {}", j["reason"])),
        Err(e) => return fail_all(layer, e),
    }

    // Read: the label for whatever the register holds, and the code beside it.
    let sample_topic = format!("te/device/{}/ot/{}/sample/{POINT}", template.device, ctx.protocol);
    let read = ctx
        .wait_connector_record(mark, SAMPLE_TIMEOUT, "a sample of the mapped point", |r| {
            r.topic == sample_topic
        })
        .await
        .and_then(|r| r.json())
        .and_then(|j| {
            let source = j["source_value"].as_f64().ok_or_else(|| format!("no numeric source_value: {j}"))?;
            let label = if source == probe {
                "probe"
            } else if source == target {
                "target"
            } else {
                "other"
            };
            if j["quality"] == "good"
                && j["value"] == label
                && j["value_repr"] == "string"
                && j["source_value_repr"] == "number"
                && j["datatype"] == serde_json::to_value(template.datatype).unwrap_or_default()
            {
                Ok(Some(format!("{} -> {}", j["source_value"], j["value"])))
            } else {
                Err(format!("sample {j}"))
            }
        });
    layer.check(IDS[0].0, IDS[0].1, read);

    // Write a label: the simulator sees its code, and the point reads back as the label.
    let write = |value: serde_json::Value, id: &str| {
        let topic = format!("te/device/{}/ot/{}/cmd/write/{id}", template.device, ctx.protocol);
        let mark = ctx.broker.mark();
        ctx.broker.publish(
            &topic,
            serde_json::json!({ "status": "init", "point": POINT, "value": value, "value_repr": "string" })
                .to_string()
                .as_bytes(),
            true,
        );
        (topic, mark)
    };
    let (topic, mark) = write(serde_json::json!("target"), "conf-b13-target");
    let written = async {
        let result = terminal(topic, mark).await?;
        if result["status"] != "successful" {
            return Err(format!("write failed: {}", result["reason"]));
        }
        if result["value"] != "target" {
            return Err(format!("the result echoes {} instead of the label", result["value"]));
        }
        let data = ctx.sim.point_data(&template.spec())?;
        let now = expected_value(template, &data.bytes, data.raw_group)?
            .map(|v| sdk_value_to_json(&v))
            .unwrap_or(serde_json::Value::Null);
        if !json_value_eq(&now, &serde_json::json!(target)) {
            return Err(format!("the simulator holds {now}, expected the code {target}"));
        }
        ctx.wait_connector_record(mark, SAMPLE_TIMEOUT, "the label read back", |r| {
            r.topic == sample_topic && r.json().ok().is_some_and(|j| j["value"] == "target")
        })
        .await?;
        Ok(Some(format!("\"target\" -> {target}")))
    }
    .await;
    layer.check(IDS[1].0, IDS[1].1, written);

    // A label the map cannot write: refused before the protocol.
    let writes_mid = ctx.sim.write_count(&template.spec()).unwrap_or(0);
    let (topic, mark) = write(serde_json::json!("other"), "conf-b13-refused");
    let refused = async {
        let result = terminal(topic, mark).await?;
        if result["status"] != "failed" {
            return Err(format!("a write of a read-only label ended {}", result["status"]));
        }
        let reason = result["reason"].as_str().unwrap_or_default().to_string();
        if !reason.contains("accepted values") {
            return Err(format!("the reason does not list the accepted values: {reason}"));
        }
        let after = ctx.sim.write_count(&template.spec())?;
        if after != writes_mid {
            return Err(format!("the simulator saw {} write(s)", after - writes_mid));
        }
        Ok(Some(reason))
    }
    .await;
    layer.check(IDS[2].0, IDS[2].1, refused);

    // An unmatched value on a device of its own: every sample bad, the link connected.
    let (topic, mark) = command(
        "define-device",
        serde_json::json!({ "status": "init", "device": lone }),
        "conf-b13-lone",
    );
    let unmatched = async {
        let result = terminal(topic, mark).await?;
        if result["status"] != "successful" {
            return Err(format!("define-device failed: {}", result["reason"]));
        }
        let lone_topic = format!("te/device/{LONE}/ot/{}/sample/{UNMAPPED}", ctx.protocol);
        let mut bad = 0;
        let mut last = mark;
        while bad < 3 {
            let r = ctx
                .wait_connector_record(last, SAMPLE_TIMEOUT, "a sample of the unmatched point", |r| {
                    r.topic == lone_topic
                })
                .await?;
            last = r.seq + 1;
            let j = r.json()?;
            let error = j["error"].as_str().unwrap_or_default();
            if j["quality"] != "bad" || !error.contains("no mapping for value") || j.get("source_value").is_none() {
                return Err(format!("sample {j}"));
            }
            bad += 1;
        }
        let link_topic = ctx.link_topic(LONE);
        let statuses: Vec<String> = ctx
            .broker
            .records_from(mark)
            .into_iter()
            .filter(|r| r.client == ctx.client && r.topic == link_topic)
            .filter_map(|r| r.json().ok())
            .filter_map(|j| j["status"].as_str().map(str::to_string))
            .collect();
        match statuses.last() {
            Some(s) if s == "connected" && statuses.iter().all(|s| s == "connected") => {
                Ok(Some(format!("{bad} bad samples, link {statuses:?}")))
            }
            _ => Err(format!("link status went {statuses:?} on unmatched values")),
        }
    }
    .await;
    layer.check(IDS[3].0, IDS[3].1, unmatched);

    // Leave things as they were: the code B6 left, the device without its clone, no lone device.
    let (topic, mark) = write(serde_json::json!("probe"), "conf-b13-restore-value");
    let _ = terminal(topic, mark).await;
    let (topic, mark) = command(
        "remove-device",
        serde_json::json!({ "status": "init", "device": LONE }),
        "conf-b13-remove",
    );
    let _ = terminal(topic, mark).await;
    let (topic, mark) = command(
        "define-device",
        serde_json::json!({ "status": "init", "device": original }),
        "conf-b13-restore",
    );
    let _ = terminal(topic, mark).await;
}

/// B5 (second half) — outage handling, in two escalating flavours:
///
/// 1. **application-level**: the device answers but every request fails — the link must go
///    `degraded`/`disconnected` and recover to `connected` when requests succeed again;
/// 2. **transport-level**: the TCP session actually dies and new connections are refused —
///    same link transitions, but recovery additionally requires the runtime's
///    reconnect-with-backoff to re-establish the transport, proven by data flowing again.
async fn check_b5_link_drop_and_recovery(ctx: &Ctx<'_>, layer: &mut Layer) {
    let device = match ctx.points.first() {
        Some(p) => p.device.clone(),
        None => return,
    };

    // -- application-level outage --
    let mark = ctx.broker.mark();
    ctx.sim.set_outage(true);
    check_link_transition(
        ctx,
        layer,
        &device,
        mark,
        "B5-drop",
        "link transitions to degraded/disconnected when the device stops answering",
        &["degraded", "disconnected"],
    )
    .await;
    let mark = ctx.broker.mark();
    ctx.sim.set_outage(false);
    check_link_transition(
        ctx,
        layer,
        &device,
        mark,
        "B5-recovery",
        "link transitions back to connected when the device answers again",
        &["connected"],
    )
    .await;

    // -- transport-level drop --
    let mark = ctx.broker.mark();
    if let Err(e) = ctx.sim.set_transport(false).await {
        layer.fail("B5-transport-drop", "transport can be dropped", e);
        return;
    }
    check_link_transition(
        ctx,
        layer,
        &device,
        mark,
        "B5-transport-drop",
        "link transitions to degraded/disconnected when the transport dies",
        &["degraded", "disconnected"],
    )
    .await;

    let mark = ctx.broker.mark();
    if let Err(e) = ctx.sim.set_transport(true).await {
        layer.fail("B5-transport-recovery", "transport can be restored", e);
        return;
    }
    check_link_transition(
        ctx,
        layer,
        &device,
        mark,
        "B5-transport-recovery",
        "the connector re-establishes a dead transport and the link returns to connected",
        &["connected"],
    )
    .await;
    // recovery is only real when samples flow again over the new session
    let good_sample = ctx
        .wait_connector_record(mark, LINK_TIMEOUT, "good sample after transport recovery", |r| {
            r.topic.contains("/sample/")
                && r.json()
                    .ok()
                    .map(|j| j["quality"] == "good")
                    .unwrap_or(false)
        })
        .await;
    layer.check(
        "B5-transport-dataflow",
        "samples flow again after the transport is re-established",
        good_sample.map(|_| None),
    );
}

/// Wait for the device's retained link status to reach one of `accepted`, scanning records
/// from `mark` (taken before the outage/recovery trigger to avoid missing fast transitions).
#[allow(clippy::too_many_arguments)]
async fn check_link_transition(
    ctx: &Ctx<'_>,
    layer: &mut Layer,
    device: &str,
    mark: usize,
    id: &str,
    name: &str,
    accepted: &[&str],
) {
    let topic = ctx.link_topic(device);
    let result = ctx
        .wait_connector_record(mark, LINK_TIMEOUT, name, |r| {
            r.topic == topic
                && r.retain
                && r.json()
                    .ok()
                    .and_then(|j| {
                        j.get("status")
                            .and_then(|s| s.as_str())
                            .map(|s| accepted.contains(&s))
                    })
                    .unwrap_or(false)
        })
        .await;
    layer.check(
        id,
        name,
        result.map(|r| {
            r.json()
                .ok()
                .and_then(|j| j.get("status").and_then(|s| s.as_str()).map(|s| format!("observed '{s}'")))
        }),
    );
}

/// B9 (dynamic half) — capability honesty: the connector never emitted a mode or datatype it
/// did not advertise.
fn check_b9_capability_honesty(
    ctx: &Ctx<'_>,
    caps: &serde_json::Value,
    layer: &mut Layer,
    from: usize,
) {
    let advertised = |key: &str| -> BTreeSet<String> {
        caps.get(key)
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect())
            .unwrap_or_default()
    };
    let modes = advertised("modes");
    let datatypes = advertised("datatypes");

    let mut violations = Vec::new();
    for record in ctx.broker.records_from(from) {
        if record.client != ctx.client || !record.topic.contains("/sample/") {
            continue;
        }
        let Ok(json) = record.json() else { continue };
        if let Some(mode) = json.get("mode").and_then(|m| m.as_str()) {
            if !modes.contains(mode) {
                violations.push(format!("sample on '{}' uses unadvertised mode '{mode}'", record.topic));
            }
        }
        if let Some(dt) = json.get("datatype").and_then(|d| d.as_str()) {
            if !datatypes.contains(dt) {
                violations.push(format!(
                    "sample on '{}' uses unadvertised datatype '{dt}'",
                    record.topic
                ));
            }
        }
    }
    violations.truncate(5);
    if violations.is_empty() {
        layer.pass(
            "B9-honesty",
            "the connector only emitted advertised modes and datatypes",
            None,
        );
    } else {
        layer.fail(
            "B9-honesty",
            "the connector only emitted advertised modes and datatypes",
            violations.join("\n"),
        );
    }
}

/// B10 — topic discipline: the connector publishes only under its contract topics; never to
/// measurement/event/alarm topics or anywhere else.
fn check_b10_topic_discipline(ctx: &Ctx<'_>, layer: &mut Layer, from: usize) {
    let allowed = [
        format!("te/device/main/service/{}/status/health", ctx.service),
        format!("te/device/main/service/{}/ot/capabilities", ctx.service),
        format!("te/device/+/ot/{}/status/link", ctx.protocol),
        format!("te/device/+/ot/{}/sample/+", ctx.protocol),
        format!("te/device/+/ot/{}/cmd/+/+", ctx.protocol),
        format!("te/device/main/service/{}/ot/cmd/+/+", ctx.service),
    ];
    let mut violations: Vec<String> = ctx
        .broker
        .records_from(from)
        .into_iter()
        .filter(|r| r.client == ctx.client)
        .filter(|r| !allowed.iter().any(|f| topic_matches(f, &r.topic)))
        .map(|r| r.topic)
        .collect();
    violations.sort();
    violations.dedup();
    if violations.is_empty() {
        layer.pass(
            "B10-topics",
            "the connector published only under its contract topics",
            None,
        );
    } else {
        layer.fail(
            "B10-topics",
            "the connector published only under its contract topics",
            format!("off-contract topics: {}", violations.join(", ")),
        );
    }
}

/// B5 (third flavour) — the peer accepts but answers nothing.
///
/// This is the failure a request timeout exists for: a half-open socket after the device (or its
/// container) vanished without a RST. A connector whose protocol calls are unbounded blocks in
/// its poll loop here and goes completely silent — no samples, no link status, no health — and
/// only a restart recovers it. The requirement is therefore not just "the link degrades" but
/// "the connector keeps running and keeps reporting".
async fn check_b5_silent_peer(ctx: &Ctx<'_>, layer: &mut Layer) {
    if ctx.points.is_empty() {
        return;
    }

    let mark = ctx.broker.mark();
    if let Err(reason) = ctx.sim.set_stalled(true).await {
        layer.skip(
            "B5-silent-peer",
            "an unanswered request is reported instead of hanging the connector",
            reason,
        );
        return;
    }

    // Whatever it reports — a bad sample or a degraded link — it must report *something*.
    let reported = ctx
        .wait_connector_record(mark, SILENT_PEER_TIMEOUT, "a report while the peer is silent", |r| {
            let Ok(json) = r.json() else { return false };
            let bad_sample = r.topic.contains("/sample/")
                && json.get("quality").and_then(|q| q.as_str()) == Some("bad");
            let link_down = r.topic.ends_with("/status/link")
                && matches!(
                    json.get("status").and_then(|s| s.as_str()),
                    Some("degraded") | Some("disconnected")
                );
            bad_sample || link_down
        })
        .await;
    if let Err(reason) = reported {
        let _ = ctx.sim.set_stalled(false).await;
        layer.fail(
            "B5-silent-peer",
            "an unanswered request is reported instead of hanging the connector",
            format!("{reason} — the connector went silent, which is what an unbounded protocol call does"),
        );
        return;
    }

    // Still alive: more reports keep coming rather than the loop being stuck on the first one.
    let alive_mark = ctx.broker.mark();
    let still_running = ctx
        .wait_connector_record(alive_mark, SILENT_PEER_TIMEOUT, "a further report", |r| {
            r.topic.contains("/sample/") || r.topic.ends_with("/status/link")
        })
        .await;
    layer.check(
        "B5-silent-peer",
        "an unanswered request is reported instead of hanging the connector",
        still_running.map(|_| Some("reported and kept polling".to_string())),
    );

    // Resuming must get data flowing again on its own.
    let mark = ctx.broker.mark();
    if let Err(e) = ctx.sim.set_stalled(false).await {
        layer.fail("B5-silent-peer-recovery", "the transport can be resumed", e);
        return;
    }
    let recovered = ctx
        .wait_connector_record(mark, LINK_TIMEOUT, "good sample after the peer answers again", |r| {
            r.topic.contains("/sample/")
                && r.json()
                    .ok()
                    .and_then(|j| j.get("quality").and_then(|q| q.as_str()).map(|q| q == "good"))
                    .unwrap_or(false)
        })
        .await;
    layer.check(
        "B5-silent-peer-recovery",
        "samples flow again once the peer answers",
        recovered.map(|_| None),
    );
}

/// The dynamic half of Layer 1: every payload the connector published validates against the
/// contract schema for its topic class.
type TopicClassifier = Box<dyn Fn(&str) -> bool>;

fn validate_captured_traffic(ctx: &Ctx<'_>, from: usize) -> Layer {
    let mut layer = Layer::new("Layer 1 — schema conformance (captured traffic)");
    let classes: [(&str, Kind, TopicClassifier); 4] = [
        (
            "samples",
            Kind::Sample,
            Box::new({
                let f = format!("te/device/+/ot/{}/sample/+", ctx.protocol);
                move |t: &str| topic_matches(&f, t)
            }),
        ),
        (
            "command transitions",
            Kind::Command,
            Box::new({
                let device = format!("te/device/+/ot/{}/cmd/+/+", ctx.protocol);
                let service = format!("te/device/main/service/{}/ot/cmd/+/+", ctx.service);
                move |t: &str| topic_matches(&device, t) || topic_matches(&service, t)
            }),
        ),
        (
            "status (health/link)",
            Kind::Status,
            Box::new({
                let health = format!("te/device/main/service/{}/status/health", ctx.service);
                let link = format!("te/device/+/ot/{}/status/link", ctx.protocol);
                move |t: &str| t == health || topic_matches(&link, t)
            }),
        ),
        (
            "capability descriptor",
            Kind::Status,
            Box::new({
                let caps = format!("te/device/main/service/{}/ot/capabilities", ctx.service);
                move |t: &str| t == caps
            }),
        ),
    ];

    for (what, kind, matches) in classes {
        let records: Vec<Record> = ctx
            .broker
            .records_from(from)
            .into_iter()
            .filter(|r| r.client == ctx.client && matches(&r.topic))
            .collect();
        let id = format!("L1/captured-{}", what.split_whitespace().next().unwrap_or(what));
        let name = format!("all captured {what} validate against the contract schema");
        if records.is_empty() {
            layer.fail(&id, &name, format!("no {what} were captured at all"));
            continue;
        }
        let mut failures: Vec<String> = records
            .iter()
            .filter_map(|r| {
                ctx.schemas
                    .validate_bytes(kind, &r.payload)
                    .err()
                    .map(|e| format!("{}: {e}", r.topic))
            })
            .collect();
        let total = records.len();
        if failures.is_empty() {
            layer.pass(&id, &name, Some(format!("{total} message(s) validated")));
        } else {
            let count = failures.len();
            failures.truncate(5);
            layer.fail(
                &id,
                &name,
                format!("{count}/{total} message(s) violate the schema:\n{}", failures.join("\n")),
            );
        }
    }
    layer
}
