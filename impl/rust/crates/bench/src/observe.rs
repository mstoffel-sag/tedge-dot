//! The observer: one MQTT subscriber on the topics a scenario counts.
//!
//! Per topic class it counts messages, bytes and the values inside them, and remembers the time
//! of the last message (for `drain_s`). Values are also bucketed by their **own timestamp**
//! (a sample's `ts_ms`, a measurement's `time`), not by arrival: a polled pass arrives as one
//! burst, so counting by arrival inside a window is off by up to one poll, while counting by
//! timestamp gives the exact number of readings taken in the window.

use rumqttc::{AsyncClient, Event, MqttOptions, Packet, QoS};
use serde::Serialize;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Class {
    /// Connector sample envelopes: one value each.
    Samples,
    /// thin-edge measurements: one value per series.
    Measurements,
    /// Cumulocity measurements made by the mapper: one value per series.
    C8y,
    /// Messages the mapper rejected.
    Errors,
}

impl Class {
    pub const ALL: [Class; 4] = [Class::Samples, Class::Measurements, Class::C8y, Class::Errors];

    pub fn filter(self) -> &'static str {
        match self {
            Class::Samples => "te/+/+/ot/+/sample/+",
            Class::Measurements => "te/+/+/+/+/m/+",
            Class::C8y => "c8y/measurement/measurements/create",
            Class::Errors => "te/errors",
        }
    }

    fn of(topic: &str) -> Option<Class> {
        if topic == "te/errors" {
            Some(Class::Errors)
        } else if topic == "c8y/measurement/measurements/create" {
            Some(Class::C8y)
        } else if topic.contains("/ot/") && topic.contains("/sample/") {
            Some(Class::Samples)
        } else if topic.starts_with("te/") && topic.rsplit('/').nth(1) == Some("m") {
            Some(Class::Measurements)
        } else {
            None
        }
    }
}

/// Width of the timestamp buckets, in ms.
const BUCKET_MS: i64 = 10;

#[derive(Clone, Debug, Default)]
pub struct Counts {
    pub msgs: u64,
    pub bytes: u64,
    pub values: u64,
    /// Values per `BUCKET_MS` bucket of their own timestamp.
    by_ts: HashMap<i64, u64>,
    /// Arrival time of the last message, Unix ms.
    pub last_ms: f64,
    pub first_ms: f64,
}

impl Counts {
    /// Values whose own timestamp lies in `[from_ms, to_ms)`.
    pub fn values_between(&self, from_ms: f64, to_ms: f64) -> u64 {
        let (a, b) = ((from_ms as i64) / BUCKET_MS, (to_ms as i64) / BUCKET_MS);
        self.by_ts.iter().filter(|(k, _)| **k >= a && **k < b).map(|(_, v)| *v).sum()
    }

    #[cfg(test)]
    pub fn add_for_test(&mut self, ts_ms: f64, values: u64) {
        *self.by_ts.entry(ts_ms as i64 / BUCKET_MS).or_default() += values;
    }
}

/// Per sample topic (one point): lowest and highest `seq` seen and how many arrived.
#[derive(Clone, Copy)]
struct SeqRange {
    min: u64,
    max: u64,
    count: u64,
}

#[derive(Default)]
struct State {
    counts: HashMap<Class, Counts>,
    errors: Vec<String>,
    seqs: HashMap<String, SeqRange>,
}

#[derive(Clone)]
pub struct Observer {
    state: Arc<Mutex<State>>,
    task: Arc<tokio::task::JoinHandle<()>>,
}

pub fn now_ms() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs_f64() * 1000.0
}

/// The number after `"key":` in a flat JSON text, without parsing the rest.
fn number_after(text: &str, key: &str) -> Option<f64> {
    let at = text.find(key)? + key.len();
    let rest = text[at..].trim_start_matches([' ', ':']);
    let end = rest.find(|c: char| !(c.is_ascii_digit() || matches!(c, '.' | '-' | 'e' | 'E' | '+'))).unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// RFC 3339 → Unix ms (what thin-edge measurements carry as `time`).
fn rfc3339_ms(s: &str) -> Option<f64> {
    let t = time::OffsetDateTime::parse(s, &time::format_description::well_known::Rfc3339).ok()?;
    Some(t.unix_timestamp_nanos() as f64 / 1e6)
}

/// Values in one message and the timestamp they were taken at.
fn inspect(class: Class, payload: &[u8]) -> (u64, Option<f64>) {
    match class {
        Class::Samples => {
            let text = std::str::from_utf8(payload).unwrap_or("");
            // A bad sample carries no value.
            let value = u64::from(text.contains("\"value\""));
            (value, number_after(text, "\"ts_ms\""))
        }
        Class::Measurements => {
            let Ok(v) = serde_json::from_slice::<serde_json::Value>(payload) else { return (0, None) };
            let ts = match v.get("time") {
                Some(serde_json::Value::String(s)) => rfc3339_ms(s),
                Some(serde_json::Value::Number(n)) => n.as_f64().map(|s| s * 1000.0),
                _ => None,
            };
            fn leaves(v: &serde_json::Value) -> u64 {
                match v {
                    serde_json::Value::Number(_) => 1,
                    serde_json::Value::Object(m) => m.iter().filter(|(k, _)| *k != "time").map(|(_, v)| leaves(v)).sum(),
                    _ => 0,
                }
            }
            (leaves(&v), ts)
        }
        Class::C8y => {
            let Ok(v) = serde_json::from_slice::<serde_json::Value>(payload) else { return (0, None) };
            // { "type", "time", "<fragment>": { "<series>": { "value": n, "unit"? } } }
            let n = v
                .as_object()
                .map(|m| m.values().filter_map(|f| f.as_object()).flat_map(|f| f.values()).filter(|s| s.get("value").is_some()).count())
                .unwrap_or(0);
            let ts = v.get("time").and_then(|t| t.as_str()).and_then(rfc3339_ms);
            (n as u64, ts)
        }
        Class::Errors => (0, None),
    }
}

impl Observer {
    /// Subscribe to `classes` on the broker at `port` and start counting. Returns once the
    /// subscriptions are acknowledged.
    pub async fn start(port: u16, classes: &[Class]) -> Result<Observer, String> {
        let mut opts = MqttOptions::new(format!("tedge-dot-bench-observer-{}", std::process::id()), "127.0.0.1", port);
        opts.set_keep_alive(Duration::from_secs(30));
        opts.set_max_packet_size(4 << 20, 4 << 20);
        opts.set_clean_session(true);
        let (client, mut events) = AsyncClient::new(opts, 64);
        for c in classes {
            client.subscribe(c.filter(), QoS::AtMostOnce).await.map_err(|e| e.to_string())?;
        }
        let state = Arc::new(Mutex::new(State::default()));
        let (ready_tx, ready_rx) = tokio::sync::oneshot::channel();
        let st = state.clone();
        let want = classes.len();
        let task = tokio::spawn(async move {
            let _client = client; // keep the request channel open
            let mut acks = 0;
            let mut ready = Some(ready_tx);
            loop {
                match events.poll().await {
                    Ok(Event::Incoming(Packet::SubAck(_))) => {
                        acks += 1;
                        if acks == want {
                            if let Some(tx) = ready.take() {
                                let _ = tx.send(Ok(()));
                            }
                        }
                    }
                    Ok(Event::Incoming(Packet::Publish(p))) => {
                        let Some(class) = Class::of(&p.topic) else { continue };
                        let (values, ts) = inspect(class, &p.payload);
                        let seq = if class == Class::Samples {
                            number_after(std::str::from_utf8(&p.payload).unwrap_or(""), "\"seq\"").map(|n| n as u64)
                        } else {
                            None
                        };
                        let now = now_ms();
                        let mut s = st.lock().unwrap();
                        if let Some(seq) = seq {
                            match s.seqs.get_mut(p.topic.as_str()) {
                                Some(r) => {
                                    r.min = r.min.min(seq);
                                    r.max = r.max.max(seq);
                                    r.count += 1;
                                }
                                None => {
                                    s.seqs.insert(p.topic.clone(), SeqRange { min: seq, max: seq, count: 1 });
                                }
                            }
                        }
                        if class == Class::Errors && s.errors.len() < 20 {
                            s.errors.push(String::from_utf8_lossy(&p.payload).into_owned());
                        }
                        let c = s.counts.entry(class).or_default();
                        if c.msgs == 0 {
                            c.first_ms = now;
                        }
                        c.msgs += 1;
                        c.bytes += p.payload.len() as u64;
                        c.values += values;
                        c.last_ms = now;
                        if let Some(ts) = ts {
                            *c.by_ts.entry(ts as i64 / BUCKET_MS).or_default() += values;
                        }
                    }
                    Ok(_) => {}
                    Err(e) => {
                        if let Some(tx) = ready.take() {
                            let _ = tx.send(Err(e.to_string()));
                            return;
                        }
                        tokio::time::sleep(Duration::from_millis(200)).await;
                    }
                }
            }
        });
        match tokio::time::timeout(Duration::from_secs(10), ready_rx).await {
            Ok(Ok(Ok(()))) => Ok(Observer { state, task: Arc::new(task) }),
            Ok(Ok(Err(e))) => Err(format!("observer cannot connect to the broker on port {port}: {e}")),
            _ => Err(format!("observer: no subscription acknowledgement from the broker on port {port}")),
        }
    }

    pub fn counts(&self, class: Class) -> Counts {
        self.state.lock().unwrap().counts.get(&class).cloned().unwrap_or_default()
    }

    /// Samples lost after the connector published them: the gaps in each point's `seq`
    /// between the first and last sample received, and the samples received in all.
    pub fn lost_samples(&self) -> (u64, u64) {
        let s = self.state.lock().unwrap();
        s.seqs.values().fold((0, 0), |(lost, got), r| {
            ((lost + (r.max - r.min + 1).saturating_sub(r.count)), got + r.count)
        })
    }

    pub fn errors(&self) -> Vec<String> {
        self.state.lock().unwrap().errors.clone()
    }

    /// Arrival time of the last message of any of `classes`, Unix ms (0 when none).
    pub fn last_ms(&self, classes: &[Class]) -> f64 {
        let s = self.state.lock().unwrap();
        classes.iter().filter_map(|c| s.counts.get(c)).map(|c| c.last_ms).fold(0.0, f64::max)
    }

    /// Wait until none of `classes` has received a message for `quiet`, or `limit` passes.
    /// Returns the time of the last message (Unix ms).
    pub async fn wait_quiet(&self, classes: &[Class], quiet: Duration, limit: Duration) -> f64 {
        let end = now_ms() + limit.as_secs_f64() * 1000.0;
        loop {
            let last = self.last_ms(classes);
            let now = now_ms();
            if now - last >= quiet.as_secs_f64() * 1000.0 || now > end {
                return last;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }
}

impl Drop for Observer {
    fn drop(&mut self) {
        if Arc::strong_count(&self.task) == 1 {
            self.task.abort();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classes_from_topics() {
        assert_eq!(Class::of("te/device/plc0/ot/bench/sample/p1"), Some(Class::Samples));
        assert_eq!(Class::of("te/device/plc0///m/bench"), Some(Class::Measurements));
        assert_eq!(Class::of("c8y/measurement/measurements/create"), Some(Class::C8y));
        assert_eq!(Class::of("te/errors"), Some(Class::Errors));
        assert_eq!(Class::of("te/device/plc0/ot/bench/status/link"), None);
    }

    #[test]
    fn values_and_timestamps() {
        let sample = br#"{"ts":"2026-10-28T10:00:00.000Z","ts_ms":1793181600000.0,"value":1.5,"quality":"good"}"#;
        assert_eq!(inspect(Class::Samples, sample), (1, Some(1793181600000.0)));
        let bad = br#"{"ts_ms":1793181600000.0,"quality":"bad","error":"x"}"#;
        assert_eq!(inspect(Class::Samples, bad).0, 0);
        let m = br#"{"time":"2026-10-28T10:00:00.000Z","bench":{"p0":1,"p1":2},"x":3}"#;
        assert_eq!(inspect(Class::Measurements, m), (3, Some(1793181600000.0)));
        let c = br#"{"type":"bench","time":"2026-10-28T10:00:00.000Z","bench":{"p0":{"value":1},"p1":{"value":2,"unit":"C"}}}"#;
        assert_eq!(inspect(Class::C8y, c).0, 2);
    }

    #[test]
    fn counts_by_own_timestamp() {
        let mut c = Counts::default();
        for ts in [1000.0, 1005.0, 1999.0, 2000.0, 2500.0] {
            *c.by_ts.entry(ts as i64 / BUCKET_MS).or_default() += 1;
        }
        assert_eq!(c.values_between(1000.0, 2000.0), 3);
        assert_eq!(c.values_between(2000.0, 3000.0), 2);
    }
}
