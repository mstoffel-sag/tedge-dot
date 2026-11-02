//! Synthetic `bench` connector module for the performance harness (`bench/`,
//! `doc/performance.md`). It talks to no device: readings come from per-point value streams
//! (see [`stream`]), so the harness can drive the real runtime at any rate without a simulator
//! in the way. Built only with the `bench` feature, which no release enables.
//!
//! - Polled: `read_points` returns one good sample per point, after `latency` per block of
//!   `batch` points.
//! - Pushed: on a device with `push_rate` above 0, every point not set `subscribe = false` is
//!   pushed. A generator task delivers `push_rate` samples per second, picking the point from a
//!   per-device stream, with optional source-time jitter and grouped delivery.

pub mod config;
pub mod stream;

use async_trait::async_trait;
use config::{DeviceSpec, PointSpec};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use stream::{PointStream, Rng};
use tedge_dot_sdk::{
    Capabilities, ConfigError, Connector, ConnectorConfig, ConnectorError, DataType, DeviceId,
    LinkReport, LinkStatus, Mode, PointRef, Quality, Sample, SampleSink, Value,
};
use time::OffsetDateTime;

const PROTOCOL: &str = "bench";
const DATATYPES: [DataType; 4] = [DataType::Float64, DataType::Int32, DataType::Bool, DataType::String];

/// How often a continuous push generator wakes up; samples are spread evenly over the gap.
const PUSH_TICK: Duration = Duration::from_millis(10);

struct BenchPoint {
    id: String,
    datatype: DataType,
    unit: Option<String>,
    addr: serde_json::Value,
    stream: PointStream,
}

impl BenchPoint {
    fn sample(&self, device: &DeviceId, ts: OffsetDateTime) -> Sample {
        let value = self.stream.value(self.datatype);
        Sample {
            ts,
            device: device.clone(),
            protocol: PROTOCOL,
            point: self.id.clone(),
            mode: Mode::Typed,
            datatype: Some(self.datatype),
            raw: raw_bytes(&value, self.datatype),
            value: Some(value),
            raw_group: 1,
            quality: Quality::Good,
            unit: self.unit.clone(),
            addr: self.addr.clone(),
            seq: None,
            error: None,
            source_value: None,
            source_ts: None,
        }
    }
}

/// The value's bytes, big endian, so `raw` is always present (contract §5).
fn raw_bytes(value: &Value, datatype: DataType) -> Vec<u8> {
    match (value, datatype) {
        (Value::Number(n), DataType::Int32) => (*n as i32).to_be_bytes().to_vec(),
        (Value::Number(n), _) => n.to_be_bytes().to_vec(),
        (Value::Bool(b), _) => vec![*b as u8],
        (Value::Text(t), _) => t.as_bytes().to_vec(),
    }
}

struct Device {
    spec: DeviceSpec,
    points: Vec<BenchPoint>,
    index: HashMap<String, usize>,
}

#[derive(Default)]
pub struct BenchConnector {
    devices: HashMap<DeviceId, Arc<Mutex<Device>>>,
    push: HashMap<DeviceId, tokio::task::JoinHandle<()>>,
    no_push: bool,
}

pub fn factory() -> Box<dyn Connector> {
    Box::<BenchConnector>::default()
}

fn invalid(msg: String) -> ConfigError {
    ConfigError::Invalid(msg)
}

#[async_trait]
impl Connector for BenchConnector {
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError> {
        let mut devices = HashMap::new();
        for d in &config.devices {
            let spec = config::device_spec(&d.name, &d.protocol_address).map_err(invalid)?;
            let mut points = Vec::with_capacity(d.points.len());
            let mut index = HashMap::with_capacity(d.points.len());
            for p in &d.points {
                if p.resolved_mode(d.default_mode) != Mode::Typed {
                    return Err(invalid(format!(
                        "point '{}' on device '{}': the bench module supports typed points only",
                        p.id, d.name
                    )));
                }
                let datatype = p.datatype.filter(|dt| DATATYPES.contains(dt)).ok_or_else(|| {
                    invalid(format!(
                        "point '{}' on device '{}': datatype must be float64, int32, bool or string",
                        p.id, d.name
                    ))
                })?;
                let PointSpec { model, step, change } =
                    config::point_spec(&d.name, &p.id, &p.address).map_err(invalid)?;
                index.insert(p.id.clone(), points.len());
                points.push(BenchPoint {
                    id: p.id.clone(),
                    datatype,
                    unit: p.unit.clone(),
                    addr: if p.address.is_null() { serde_json::json!({}) } else { p.address.clone() },
                    stream: PointStream::new(&d.name, &p.id, model, step, change),
                });
            }
            devices.insert(d.name.clone(), Arc::new(Mutex::new(Device { spec, points, index })));
        }
        self.devices = devices;
        Ok(())
    }

    fn capabilities(&self) -> Capabilities {
        Capabilities {
            protocol: PROTOCOL,
            version: env!("CARGO_PKG_VERSION"),
            modes: vec![Mode::Typed],
            datatypes: DATATYPES.to_vec(),
            point_kinds: vec!["synthetic".into()],
            command_verbs: vec![],
            features: vec!["subscribe".into()],
            subscribe: true,
        }
    }

    fn disable_push(&mut self) {
        self.no_push = true;
    }

    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError> {
        Ok(self
            .devices
            .keys()
            .map(|d| LinkReport::new(d.clone(), LinkStatus::Connected, None))
            .collect())
    }

    async fn read_points(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
    ) -> Result<Vec<Sample>, ConnectorError> {
        let dev = self
            .devices
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?
            .clone();
        let latency = {
            let d = dev.lock().unwrap();
            let blocks = points.len().div_ceil(d.spec.batch.max(1));
            d.spec.latency * blocks as u32
        };
        if !latency.is_zero() {
            tokio::time::sleep(latency).await;
        }
        let ts = OffsetDateTime::now_utc();
        let mut d = dev.lock().unwrap();
        let mut out = Vec::with_capacity(points.len());
        for p in points {
            let Some(&i) = d.index.get(&p.id) else {
                return Err(ConnectorError::UnknownPoint {
                    device: device.clone(),
                    point: p.id.clone(),
                });
            };
            let pt = &mut d.points[i];
            pt.stream.advance();
            out.push(pt.sample(device, ts));
        }
        Ok(out)
    }

    fn pushes_point(&self, device: &DeviceId, _point: &PointRef) -> bool {
        !self.no_push
            && self
                .devices
                .get(device)
                .is_some_and(|d| d.lock().unwrap().spec.push_rate > 0.0)
    }

    async fn subscribe(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
        sink: SampleSink,
    ) -> Result<(), ConnectorError> {
        let dev = self
            .devices
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?
            .clone();
        let pushed: Vec<usize> = {
            let d = dev.lock().unwrap();
            points.iter().filter_map(|p| d.index.get(&p.id).copied()).collect()
        };
        if let Some(old) = self.push.remove(device) {
            old.abort();
        }
        if pushed.is_empty() {
            return Ok(());
        }
        let task = tokio::spawn(push_loop(device.clone(), dev, pushed, sink));
        self.push.insert(device.clone(), task);
        Ok(())
    }

    async fn check_subscription(&mut self, device: &DeviceId) -> Result<(), ConnectorError> {
        match self.push.get(device) {
            Some(task) if task.is_finished() => {
                Err(ConnectorError::Transport("push generator stopped".into()))
            }
            _ => Ok(()),
        }
    }

    async fn disconnect(&mut self) -> Result<(), ConnectorError> {
        for (_, task) in self.push.drain() {
            task.abort();
        }
        Ok(())
    }
}

/// The source time of a pushed sample generated at `generated`: `ts_jitter` earlier at most,
/// floored to `ts_resolution`.
fn source_time(generated: OffsetDateTime, spec: &DeviceSpec, jitter: &mut Rng) -> OffsetDateTime {
    let mut ts = generated;
    if !spec.ts_jitter.is_zero() {
        ts -= spec.ts_jitter.mul_f64(jitter.draw());
    }
    let res = spec.ts_resolution.as_nanos() as i128;
    let ns = ts.unix_timestamp_nanos();
    OffsetDateTime::from_unix_timestamp_nanos(ns - ns.rem_euclid(res)).unwrap_or(ts)
}

/// Deliver `push_rate` samples per second for the `pushed` points of one device until the sink
/// closes or the task is aborted. Each wake-up generates what is owed since the last one,
/// carrying the fraction over, with generation times spread evenly across the gap. With a
/// `publish_interval` the wake-ups are that far apart, so samples arrive in groups the way an
/// OPC UA subscription delivers one notification per publishing interval.
async fn push_loop(device: DeviceId, dev: Arc<Mutex<Device>>, pushed: Vec<usize>, sink: SampleSink) {
    let spec = dev.lock().unwrap().spec.clone();
    let tick = if spec.publish_interval.is_zero() { PUSH_TICK } else { spec.publish_interval };
    let mut pick = Rng::new(&device, "#push");
    let mut jitter = Rng::new(&device, "#ts");
    let mut timer = tokio::time::interval(tick);
    timer.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    timer.tick().await;
    let mut last = tokio::time::Instant::now();
    let mut carry = 0.0f64;
    let mut batch = Vec::new();
    loop {
        timer.tick().await;
        let now = tokio::time::Instant::now();
        let elapsed = now - last;
        last = now;
        let owed = spec.push_rate * elapsed.as_secs_f64() + carry;
        let n = owed.floor();
        carry = owed - n;
        let n = n as usize;
        if n == 0 {
            continue;
        }
        let wall = OffsetDateTime::now_utc();
        {
            let mut d = dev.lock().unwrap();
            for i in 0..n {
                let k = pushed[((pick.draw() * pushed.len() as f64) as usize).min(pushed.len() - 1)];
                let generated = wall - elapsed + elapsed.mul_f64((i + 1) as f64 / n as f64);
                let ts = source_time(generated, &spec, &mut jitter);
                let pt = &mut d.points[k];
                pt.stream.advance();
                // The generation time is what a protocol would report as the source time.
                let mut s = pt.sample(&device, ts);
                s.source_ts = Some(ts);
                batch.push(s);
            }
        }
        for s in batch.drain(..) {
            if sink.send(s).await.is_err() {
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(toml: &str) -> ConnectorConfig {
        let mut cfg: ConnectorConfig = toml::from_str(toml).unwrap();
        cfg.base_dir = None;
        cfg
    }

    fn points(n: usize) -> String {
        (0..n)
            .map(|i| format!("[[device.point]]\nid = \"p{i}\"\ndatatype = \"float64\"\naddress = {{}}\n"))
            .collect()
    }

    fn refs(n: usize) -> Vec<PointRef> {
        (0..n)
            .map(|i| PointRef {
                id: format!("p{i}"),
                mode: Mode::Typed,
                datatype: Some(DataType::Float64),
                endianness: tedge_dot_sdk::Endianness::Big,
                word_order: tedge_dot_sdk::WordOrder::Big,
                access: tedge_dot_sdk::Access::Read,
                unit: None,
                transform: Default::default(),
                interval: None,
            })
            .collect()
    }

    fn connector(device_extra: &str, n: usize) -> BenchConnector {
        let cfg = config(&format!(
            "[connector]\nprotocol = \"bench\"\n[[device]]\nname = \"plc0\"\nprotocol_address = {{ {device_extra} }}\n{}",
            points(n)
        ));
        let mut c = BenchConnector::default();
        c.configure(&cfg).unwrap();
        c
    }

    #[tokio::test]
    async fn polled_read_returns_every_point() {
        let mut c = connector("", 100);
        let s = c.read_points(&"plc0".into(), &refs(100)).await.unwrap();
        assert_eq!(s.len(), 100);
        assert!(s.iter().all(|s| s.quality == Quality::Good && s.value.is_some()));
        assert!(s.iter().all(|x| x.ts == s[0].ts), "one read time per call");
    }

    #[tokio::test]
    async fn latency_per_block() {
        let mut c = connector("latency = \"2ms\", batch = 500", 2000);
        let t = std::time::Instant::now();
        c.read_points(&"plc0".into(), &refs(2000)).await.unwrap();
        assert!(t.elapsed() >= Duration::from_millis(8), "{:?}", t.elapsed());
    }

    #[tokio::test]
    async fn no_latency_by_default() {
        let mut c = connector("", 10_000);
        let t = std::time::Instant::now();
        c.read_points(&"plc0".into(), &refs(10_000)).await.unwrap();
        assert!(t.elapsed() < Duration::from_millis(200), "{:?}", t.elapsed());
    }

    #[test]
    fn push_needs_a_rate() {
        let c = connector("", 1);
        assert!(!c.pushes_point(&"plc0".into(), &refs(1)[0]));
        let c = connector("push_rate = 10", 1);
        assert!(c.pushes_point(&"plc0".into(), &refs(1)[0]));
    }

    async fn collect(c: &mut BenchConnector, n: usize, window: Duration) -> Vec<(Sample, OffsetDateTime)> {
        let (tx, mut rx) = tokio::sync::mpsc::channel(100_000);
        c.subscribe(&"plc0".into(), &refs(n), tx).await.unwrap();
        let mut out = Vec::new();
        let end = tokio::time::Instant::now() + window;
        while let Ok(Some(s)) = tokio::time::timeout_at(end, rx.recv()).await {
            out.push((s, OffsetDateTime::now_utc()));
        }
        c.disconnect().await.unwrap();
        out
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn push_rate_within_five_percent() {
        let mut c = connector("push_rate = 10000", 1000);
        // Warm up past the first tick, then count one second.
        let got = collect(&mut c, 1000, Duration::from_millis(2100)).await;
        let start = got[0].1 + time::Duration::milliseconds(500);
        let end = start + time::Duration::seconds(1);
        let in_window = got.iter().filter(|(_, at)| *at >= start && *at < end).count();
        assert!((9_500..=10_500).contains(&in_window), "{in_window} samples in 1 s");
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn jitter_resolution_and_grouping() {
        let mut c = connector(
            "push_rate = 2000, ts_jitter = \"50ms\", ts_resolution = \"10ms\", publish_interval = \"100ms\"",
            100,
        );
        let got = collect(&mut c, 100, Duration::from_millis(1000)).await;
        assert!(got.len() > 1000, "{} samples", got.len());
        for (s, at) in &got {
            assert_eq!(s.ts.unix_timestamp_nanos() % 10_000_000, 0, "ts on a 10 ms grid");
            let lag = *at - s.ts;
            assert!(lag <= time::Duration::milliseconds(150 + 20), "lag {lag}");
            assert!(lag >= time::Duration::ZERO, "ts after delivery: {lag}");
        }
        // Grouped: the gaps between arrivals are either tiny (same group) or about 100 ms.
        let groups = got.windows(2).filter(|w| w[1].1 - w[0].1 > time::Duration::milliseconds(50)).count();
        assert!((7..=11).contains(&groups), "{groups} group boundaries in 1 s");
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn timestamp_keys_leave_values_alone() {
        let values = |extra: &str| {
            let extra = extra.to_string();
            async move {
                let mut c = connector(&extra, 1);
                collect(&mut c, 1, Duration::from_millis(300))
                    .await
                    .into_iter()
                    .map(|(s, _)| s.value)
                    .collect::<Vec<_>>()
            }
        };
        let plain = values("push_rate = 200").await;
        let jittered = values("push_rate = 200, ts_jitter = \"50ms\", ts_resolution = \"10ms\"").await;
        let n = plain.len().min(jittered.len());
        assert!(n > 20);
        assert_eq!(plain[..n], jittered[..n]);
    }

    const CONFIG_ERRORS: &str =
        include_str!("../../../../../doc/contract/test-vectors/bench/config-errors.json");

    /// The invalid configs both builds refuse, and the messages (the C test runs them too).
    #[test]
    fn shared_config_errors() {
        let doc: serde_json::Value = serde_json::from_str(CONFIG_ERRORS).unwrap();
        for case in doc["cases"].as_array().unwrap() {
            let name = case["name"].as_str().unwrap();
            let cfg = config(&format!(
                "[connector]\nprotocol = \"bench\"\n[[device]]\nname = \"plc0\"\n{}\n{}\n",
                case["device"].as_str().unwrap(),
                case["points"].as_str().unwrap()
            ));
            let got = BenchConnector::default()
                .configure(&cfg)
                .err()
                .map(|e| e.to_string().trim_start_matches("invalid configuration: ").to_string());
            assert_eq!(got.as_deref(), case["rust"].as_str(), "case '{name}'");
        }
    }

    #[test]
    fn invalid_change_is_refused() {
        let cfg = config(
            "[connector]\nprotocol = \"bench\"\n[[device]]\nname = \"plc0\"\nprotocol_address = {}\n[[device.point]]\nid = \"p1\"\ndatatype = \"float64\"\naddress = { change = 1.5 }\n",
        );
        let err = BenchConnector::default().configure(&cfg).unwrap_err().to_string();
        assert_eq!(
            err,
            "invalid configuration: point 'p1' on device 'plc0' address: 'change' must be a number from 0 to 1"
        );
    }
}
