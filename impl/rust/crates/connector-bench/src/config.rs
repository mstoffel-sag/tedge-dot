//! The `bench` module's configuration: device `protocol_address` and point `address`.
//!
//! Validation is written out by hand rather than through serde, so the messages are exactly the
//! ones the C module prints for the same mistakes (a parity test feeds both builds the same
//! invalid configs).

use crate::stream::Model;
use serde_json::{Map, Value};
use std::time::Duration;
use tedge_dot_sdk::parse_duration;

const DEVICE_KEYS: &[&str] = &[
    "latency",
    "batch",
    "push_rate",
    "ts_jitter",
    "ts_resolution",
    "publish_interval",
];
const POINT_KEYS: &[&str] = &["model", "step", "change"];

#[derive(Clone, Debug, PartialEq)]
pub struct DeviceSpec {
    /// Time one block of `batch` points takes to read.
    pub latency: Duration,
    /// Points per block; `usize::MAX` when unset.
    pub batch: usize,
    /// Pushed samples per second across the device's pushed points; 0 polls everything.
    pub push_rate: f64,
    pub ts_jitter: Duration,
    pub ts_resolution: Duration,
    /// 0 delivers continuously.
    pub publish_interval: Duration,
}

impl Default for DeviceSpec {
    fn default() -> Self {
        DeviceSpec {
            latency: Duration::ZERO,
            batch: usize::MAX,
            push_rate: 0.0,
            ts_jitter: Duration::ZERO,
            ts_resolution: Duration::from_millis(1),
            publish_interval: Duration::ZERO,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct PointSpec {
    pub model: Model,
    pub step: f64,
    pub change: f64,
}

impl Default for PointSpec {
    fn default() -> Self {
        PointSpec {
            model: Model::Walk,
            step: 1.0,
            change: 1.0,
        }
    }
}

/// An absent or empty table is all defaults; anything else must be a table.
fn table<'a>(v: &'a Value, place: &str) -> Result<Option<&'a Map<String, Value>>, String> {
    match v {
        Value::Null => Ok(None),
        Value::Object(m) => Ok(Some(m)),
        _ => Err(format!("{place}: must be a table")),
    }
}

fn unknown_key(m: &Map<String, Value>, known: &[&str], place: &str) -> Result<(), String> {
    // Sorted, so both builds name the same key when a table has several unknown ones.
    let mut keys: Vec<&String> = m.keys().collect();
    keys.sort();
    match keys.into_iter().find(|k| !known.contains(&k.as_str())) {
        Some(k) => Err(format!("{place}: unknown key '{k}'")),
        None => Ok(()),
    }
}

fn duration(v: &Value, key: &str, place: &str) -> Result<Duration, String> {
    v.as_str()
        .and_then(parse_duration)
        .ok_or_else(|| format!("{place}: '{key}' must be a duration such as \"2ms\""))
}

pub fn device_spec(device: &str, v: &Value) -> Result<DeviceSpec, String> {
    let place = format!("device '{device}' protocol_address");
    let mut spec = DeviceSpec::default();
    let Some(m) = table(v, &place)? else {
        return Ok(spec);
    };
    unknown_key(m, DEVICE_KEYS, &place)?;
    if let Some(v) = m.get("latency") {
        spec.latency = duration(v, "latency", &place)?;
    }
    if let Some(v) = m.get("batch") {
        spec.batch = v
            .as_u64()
            .filter(|n| *n >= 1)
            .ok_or_else(|| format!("{place}: 'batch' must be a positive integer"))?
            as usize;
    }
    if let Some(v) = m.get("push_rate") {
        spec.push_rate = v
            .as_f64()
            .filter(|r| r.is_finite() && *r >= 0.0)
            .ok_or_else(|| format!("{place}: 'push_rate' must be a number of 0 or more"))?;
    }
    if let Some(v) = m.get("ts_jitter") {
        spec.ts_jitter = duration(v, "ts_jitter", &place)?;
    }
    if let Some(v) = m.get("ts_resolution") {
        spec.ts_resolution = duration(v, "ts_resolution", &place)?;
        if spec.ts_resolution < Duration::from_millis(1) {
            return Err(format!("{place}: 'ts_resolution' must be at least 1ms"));
        }
    }
    if let Some(v) = m.get("publish_interval") {
        spec.publish_interval = duration(v, "publish_interval", &place)?;
    }
    Ok(spec)
}

pub fn point_spec(device: &str, point: &str, v: &Value) -> Result<PointSpec, String> {
    let place = format!("point '{point}' on device '{device}' address");
    let mut spec = PointSpec::default();
    let Some(m) = table(v, &place)? else {
        return Ok(spec);
    };
    unknown_key(m, POINT_KEYS, &place)?;
    if let Some(v) = m.get("model") {
        spec.model = match v.as_str() {
            Some("walk") => Model::Walk,
            Some("constant") => Model::Constant,
            Some("counter") => Model::Counter,
            _ => {
                return Err(format!(
                    "{place}: 'model' must be \"walk\", \"constant\" or \"counter\""
                ))
            }
        };
    }
    if let Some(v) = m.get("step") {
        spec.step = v
            .as_f64()
            .filter(|s| s.is_finite() && *s >= 0.0)
            .ok_or_else(|| format!("{place}: 'step' must be a finite number of 0 or more"))?;
    }
    if let Some(v) = m.get("change") {
        spec.change = v
            .as_f64()
            .filter(|c| (0.0..=1.0).contains(c))
            .ok_or_else(|| format!("{place}: 'change' must be a number from 0 to 1"))?;
    }
    Ok(spec)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn defaults_when_absent() {
        assert_eq!(device_spec("d", &Value::Null).unwrap(), DeviceSpec::default());
        assert_eq!(point_spec("d", "p", &json!({})).unwrap(), PointSpec::default());
    }

    #[test]
    fn device_keys() {
        let s = device_spec(
            "d",
            &json!({"latency": "2ms", "batch": 500, "push_rate": 10000, "ts_jitter": "50ms",
                    "ts_resolution": "10ms", "publish_interval": "100ms"}),
        )
        .unwrap();
        assert_eq!(s.latency, Duration::from_millis(2));
        assert_eq!(s.batch, 500);
        assert_eq!(s.push_rate, 10000.0);
        assert_eq!(s.ts_resolution, Duration::from_millis(10));
        assert_eq!(s.publish_interval, Duration::from_millis(100));
    }

    #[test]
    fn messages_name_the_place() {
        let err = point_spec("plc0", "p1", &json!({"change": 1.5})).unwrap_err();
        assert_eq!(err, "point 'p1' on device 'plc0' address: 'change' must be a number from 0 to 1");
        let err = device_spec("plc0", &json!({"batch": 0})).unwrap_err();
        assert_eq!(err, "device 'plc0' protocol_address: 'batch' must be a positive integer");
        let err = device_spec("plc0", &json!({"zeta": 1, "alpha": 1})).unwrap_err();
        assert_eq!(err, "device 'plc0' protocol_address: unknown key 'alpha'");
        let err = device_spec("plc0", &json!({"ts_resolution": "500us"})).unwrap_err();
        assert_eq!(
            err,
            "device 'plc0' protocol_address: 'ts_resolution' must be a duration such as \"2ms\""
        );
    }
}
