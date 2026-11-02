//! Concurrent reads (contract §5.6) under the real SDK runtime and the harness's broker: a module
//! that hands out readers has its devices read in parallel, one read per device at a time.

use async_trait::async_trait;
use ot_conformance::broker::BrokerHandle;
use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tedge_dot_sdk::concurrency::InFlight;
use tedge_dot_sdk::{
    Capabilities, ConfigError, Connector, ConnectorConfig, ConnectorError, DataType, DeviceId,
    DeviceReader, LinkReport, LinkStatus, Mode, PointRef, Quality, Sample, Value,
};

/// Reads in flight per device now, and the most ever seen.
#[derive(Default)]
struct Counts {
    now: Mutex<HashMap<String, usize>>,
    peak: AtomicUsize,
}

impl Counts {
    fn enter(&self, device: &str) {
        let mut now = self.now.lock().unwrap();
        let n = now.entry(device.to_string()).or_default();
        *n += 1;
        self.peak.fetch_max(*n, Ordering::SeqCst);
    }

    fn leave(&self, device: &str) {
        *self.now.lock().unwrap().get_mut(device).unwrap() -= 1;
    }
}

/// `slow` takes `slow_for` to read; every other device answers at once.
struct Mock {
    devices: Vec<String>,
    counts: Arc<Counts>,
    slow_for: Duration,
}

fn sample(device: &str, point: &str) -> Sample {
    Sample {
        ts: time::OffsetDateTime::now_utc(),
        device: device.into(),
        protocol: "bench",
        point: point.into(),
        mode: Mode::Typed,
        datatype: Some(DataType::Float64),
        raw: 1f64.to_be_bytes().to_vec(),
        value: Some(Value::Number(1.0)),
        raw_group: 1,
        quality: Quality::Good,
        unit: None,
        addr: serde_json::json!({}),
        seq: None,
        error: None,
        source_value: None,
        source_ts: None,
    }
}

struct MockReader {
    device: String,
    counts: Arc<Counts>,
    delay: Duration,
}

#[async_trait]
impl DeviceReader for MockReader {
    async fn read_points(&self, points: &[PointRef], _: &InFlight) -> Result<Vec<Sample>, ConnectorError> {
        self.counts.enter(&self.device);
        tokio::time::sleep(self.delay).await;
        self.counts.leave(&self.device);
        Ok(points.iter().map(|p| sample(&self.device, &p.id)).collect())
    }
}

#[async_trait]
impl Connector for Mock {
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError> {
        self.devices = config.devices.iter().map(|d| d.name.clone()).collect();
        Ok(())
    }

    fn capabilities(&self) -> Capabilities {
        Capabilities {
            protocol: "bench",
            version: "0",
            modes: vec![Mode::Typed],
            datatypes: vec![DataType::Float64],
            point_kinds: vec![],
            command_verbs: vec![],
            features: vec![],
            subscribe: false,
        }
    }

    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError> {
        Ok(self.devices.iter().map(|d| LinkReport::new(d.clone(), LinkStatus::Connected, None)).collect())
    }

    fn reader(&self, device: &DeviceId) -> Option<Arc<dyn DeviceReader>> {
        let delay = if device == "slow" { self.slow_for } else { Duration::ZERO };
        Some(Arc::new(MockReader { device: device.clone(), counts: self.counts.clone(), delay }))
    }

    async fn read_points(&mut self, _: &DeviceId, _: &[PointRef]) -> Result<Vec<Sample>, ConnectorError> {
        panic!("every device has a reader");
    }

    async fn disconnect(&mut self) -> Result<(), ConnectorError> {
        Ok(())
    }
}

#[tokio::test]
async fn a_slow_device_does_not_delay_the_others_and_is_read_once_at_a_time() {
    let broker = BrokerHandle::start().await.unwrap();
    let text = format!(
        "[connector]\nprotocol = \"bench\"\npoll_interval = \"200ms\"\noperation_timeout = \"10s\"\n\
         [mqtt]\nhost = \"127.0.0.1\"\nport = {}\n\
         [[device]]\nname = \"slow\"\nprotocol_address = {{}}\n\
         [[device.point]]\nid = \"p\"\ndatatype = \"float64\"\naddress = {{}}\n\
         [[device]]\nname = \"fast\"\nprotocol_address = {{}}\n\
         [[device.point]]\nid = \"p\"\ndatatype = \"float64\"\naddress = {{}}\n",
        broker.port()
    );
    let config = tedge_dot_sdk::resolve_config(&text, std::path::Path::new(".")).unwrap();
    let counts = Arc::new(Counts::default());
    let mock = Mock { devices: Vec::new(), counts: counts.clone(), slow_for: Duration::from_secs(2) };
    let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
    let run = tokio::spawn(tedge_dot_sdk::runtime::run_until(
        Box::new(mock),
        config,
        std::env::temp_dir().join("concurrent-reads.toml"),
        async {
            let _ = stop_rx.await;
        },
    ));
    // Wait for the first fast sample, then count over a window the slow read spans.
    broker
        .wait_for(0, Duration::from_secs(10), "a sample of the fast device", |r| {
            r.topic.ends_with("/sample/p") && r.topic.contains("/fast/")
        })
        .await
        .unwrap();
    let mark = broker.mark();
    let window = Duration::from_millis(1500);
    tokio::time::sleep(window).await;
    let records = broker.records_from(mark);
    let fast = records.iter().filter(|r| r.topic.contains("/fast/") && r.topic.ends_with("/sample/p")).count();
    // 200 ms polls over 1.5 s while the slow device's 2 s read is in flight: about 7. A runtime
    // that waited for the slow read would publish at most one.
    assert!(fast >= 5, "the fast device was polled {fast} times in {window:?}");
    assert_eq!(counts.peak.load(Ordering::SeqCst), 1, "a device had two reads in flight");
    let _ = stop_tx.send(());
    let _ = tokio::time::timeout(Duration::from_secs(15), run).await;
}
