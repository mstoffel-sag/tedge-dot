//! Building samples from PI values (spec §4, §5.1): the decoder's result, the point's unit (or
//! PI's), and the `addr` echo.

use crate::config::Point;
use crate::resolve::Stream;
use crate::value::decode;
use serde_json::{json, Value as Json};
use tedge_dot_sdk::{Quality, Sample};
use time::OffsetDateTime;

pub const PROTOCOL: &str = "aveva-pi";

fn base(device: &str, point: &Point) -> Sample {
    Sample {
        ts: OffsetDateTime::now_utc(),
        source_ts: None,
        replayed: false,
        device: device.to_string(),
        protocol: PROTOCOL,
        point: point.id.clone(),
        mode: point.mode,
        datatype: point.datatype,
        value: None,
        raw: Vec::new(),
        raw_group: 1,
        quality: Quality::Good,
        unit: point.unit.clone(),
        addr: json!({}),
        seq: None,
        error: None,
        source_value: None,
    }
}

/// The sample of one PI value of `point`. `source` is the PI time for a pushed value (`None`
/// for a snapshot read, which carries the read time), `replayed` marks history delivered after
/// the fact (contract §5.3 rule 7).
pub fn build(device: &str, point: &Point, stream: &Stream, pi: &Json, source: Option<OffsetDateTime>, replayed: bool) -> Sample {
    let d = decode(pi, point.datatype, point.mode);
    let mut s = base(device, point);
    let mut addr = serde_json::Map::new();
    addr.insert("web_id".into(), json!(stream.web_id));
    addr.insert("path".into(), json!(stream.path));
    if let Some(t) = &stream.point_type {
        addr.insert("point_type".into(), json!(t));
    }
    if let Some(ts) = &d.timestamp {
        addr.insert("timestamp".into(), json!(ts));
    }
    if !d.flags.is_empty() {
        addr.insert("flags".into(), json!(d.flags));
    }
    s.addr = Json::Object(addr);
    s.raw = d.raw.into_bytes();
    s.quality = d.quality;
    s.value = d.value;
    s.error = d.error;
    if s.unit.is_none() {
        s.unit = stream.units.clone().or(d.units);
    }
    s.source_ts = source;
    s.replayed = replayed && source.is_some();
    s
}

/// A `bad` sample without a PI value: an unresolved point, or a per-item error.
pub fn bad(device: &str, point: &Point, path: &str, error: String) -> Sample {
    let mut s = base(device, point);
    s.quality = Quality::Bad;
    s.addr = json!({ "path": path });
    s.error = Some(error);
    s
}
