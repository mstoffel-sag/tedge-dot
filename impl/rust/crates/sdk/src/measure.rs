//! Grouped output (contract §5.5): with `[connector] output = "group"` the runtime publishes
//! thin-edge measurements itself, several series per message, grouped per device, type and time,
//! instead of one sample per reading for the `ot-measurement` flow to convert.
//!
//! [`Batcher`] holds one table per device with a slot per point. The runtime offers it every
//! reading the report policy (§5.3) and the value map (§4.3) let through, and the batcher decides
//! when a series is published, and with which time, by the point's `grouping.by`:
//!
//! - `"time"` (default): readings that share a millisecond timestamp share a message. A message
//!   closes when a reading with another time arrives for its type, when a point repeats, or at
//!   the end of the runtime's pass.
//! - `"frame"`: readings within `window` of the device clock are published as layers when the
//!   window ends: a point's first value in the window goes into layer 0, its second into layer
//!   1, and so on. A layer is stamped with its first reading's time, moved to 1 ms after the
//!   previous layer's time when it is not later, so a point's values stay in time order.
//! - `"sample"`: every `every`, one series per slot holding a good value, stamped with the sample
//!   time. Stale slots are left out; event points are published as under `"time"`. With
//!   `aggregate`, the series are the time-weighted mean, the minimum, the maximum or the last of
//!   the values the point held during the period, over its good time.
//!
//! A message is published on `te/device/<device>///m/<type>` and holds one entry per fragment:
//! `{"time":…,"<fragment>":{"<series>":<value>,…},…}`.
//!
//! It is pure — no clock, no MQTT — so the runtime drives it with the real clocks and the shared
//! test vectors (`doc/contract/test-vectors/measurements/`) drive it here and in the C SDK
//! (impl/c/sdk/src/measure.c), which implements the same rules.

use crate::config::{parse_duration, ConnectorConfig, DeviceConfig, PointConfig};
use crate::connector::Access;
use crate::model::{format_rfc3339_ms, Mode, Quality, Sample, Value};
use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::time::{Duration, Instant};
use time::OffsetDateTime;

/// The values `[connector] output` may take: the two modes, then their deprecated names.
pub const OUTPUTS: &[&str] = &["single", "group", "samples", "measurements"];
/// The keys a `grouping` table may carry.
pub const GROUPING_KEYS: &[&str] = &["by", "window", "every", "aggregate"];
/// The keys of the deprecated `batch` table, the former name of `grouping` (`group_by` is `by`).
pub const BATCH_KEYS: &[&str] = &["group_by", "window", "every", "aggregate"];
/// The values `grouping.aggregate` may name.
pub const AGGREGATES: &[&str] = &["mean", "min", "max", "last"];
/// The keys of the `[connector] measurement` table.
pub const MEASUREMENT_KEYS: &[&str] = &["type", "fragment", "series", "group", "point_separator", "include_boolean"];
/// The keys of a device's `measurement` table.
pub const DEVICE_MEASUREMENT_KEYS: &[&str] = &["type", "fragment", "series", "group"];
/// The refusal of a table level that sets both names of the grouping table.
pub const BATCH_AND_GROUPING: &str = "batch and grouping cannot both be set (batch is the deprecated name of grouping)";

/// A message is split before its estimated Cumulocity size passes this many bytes, which leaves
/// margin under the c8y mapper's 16,184-byte limit.
pub const SIZE_BUDGET: usize = 15_000;
/// What the c8y mapper adds per series when it converts a measurement (measured at about 10).
pub const SERIES_OVERHEAD: usize = 12;

/// `{"time":"2026-01-01T00:00:00.000Z",` + the closing `}`, around the fragments.
const FRAME_BYTES: usize = 9 + 24 + 2 + 1;
/// `:{` + `}` around one fragment's series, after its quoted name.
const FRAGMENT_BYTES: usize = 3;

const WINDOW_MIN: Duration = Duration::from_millis(1);
const WINDOW_MAX: Duration = Duration::from_secs(60);
const EVERY_MIN: Duration = Duration::from_millis(100);
const EVERY_MAX: Duration = Duration::from_secs(3600);
/// A frame holds at most this many layers; a reading that would open one more publishes them
/// and starts a new frame in the same window.
const MAX_LAYERS: usize = 64;

/// What the runtime publishes for readings.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Output {
    /// One sample envelope per reading (contract §5); the `ot-measurement` flow makes one
    /// measurement of each.
    #[default]
    Single,
    /// Grouped thin-edge measurements, and samples only for the points flows need.
    Group,
}

impl Output {
    /// The output a loaded configuration asks for. The loader refused any other value.
    pub fn of(config: &ConnectorConfig) -> Self {
        match config.connector.output.as_deref() {
            Some("group" | "measurements") => Output::Group,
            _ => Output::Single,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Output::Single => "single",
            Output::Group => "group",
        }
    }
}

/// The warning for a deprecated `output` value, if the configuration uses one.
pub fn deprecated_output(config: &ConnectorConfig) -> Option<String> {
    let old = config.connector.output.as_deref()?;
    let new = match old {
        "samples" => "single",
        "measurements" => "group",
        _ => return None,
    };
    Some(format!("output = \"{old}\" is deprecated: use \"{new}\""))
}

/// The warning logged once per load when a configuration or point library uses `batch`.
pub const BATCH_DEPRECATED: &str = "batch is deprecated: use grouping (group_by is now by)";

/// Rewrite a table's deprecated `batch` as `grouping`, with `group_by` renamed `by`, after its
/// keys were checked as written. Returns whether it rewrote one. A table that sets both is
/// refused with [`BATCH_AND_GROUPING`]; the caller prefixes the place.
pub fn rewrite_batch(table: &mut toml::Value) -> Result<bool, String> {
    let Some(table) = table.as_table_mut() else {
        return Ok(false);
    };
    let Some(mut batch) = table.remove("batch") else {
        return Ok(false);
    };
    if table.contains_key("grouping") {
        return Err(BATCH_AND_GROUPING.to_string());
    }
    if let Some(policy) = batch.as_table_mut().and_then(|b| b.remove("group_by")) {
        if let Some(b) = batch.as_table_mut() {
            b.insert("by".to_string(), policy);
        }
    }
    table.insert("grouping".to_string(), batch);
    Ok(true)
}

/// When a point's readings are published, and with which time (`grouping.by`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum GroupBy {
    Time,
    Frame(Duration),
    Sample(Duration),
}

impl GroupBy {
    /// The policy of a merged `grouping` table (`[connector]`, then device, then point). Each
    /// table was checked as written ([`check_values`]); this checks what only the merge decides:
    /// the duration the policy needs is there. A key of another policy is inherited from a level
    /// that chose it, and is ignored.
    pub fn from_table(table: &serde_json::Map<String, serde_json::Value>) -> Result<Self, String> {
        let by = table.get("by").and_then(|v| v.as_str()).unwrap_or("time");
        let duration = |key: &str| table.get(key).and_then(|v| v.as_str()).and_then(parse_duration);
        match by {
            "frame" => duration("window")
                .map(GroupBy::Frame)
                .ok_or_else(|| "grouping.window is required with by = \"frame\"".to_string()),
            "sample" => duration("every")
                .map(GroupBy::Sample)
                .ok_or_else(|| "grouping.every is required with by = \"sample\"".to_string()),
            _ => Ok(GroupBy::Time),
        }
    }
}

/// What a `"sample"` point publishes for each period (`grouping.aggregate`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Aggregate {
    Mean,
    Min,
    Max,
    Last,
}

impl Aggregate {
    fn of(name: &str) -> Option<Self> {
        match name {
            "mean" => Some(Aggregate::Mean),
            "min" => Some(Aggregate::Min),
            "max" => Some(Aggregate::Max),
            "last" => Some(Aggregate::Last),
            _ => None,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Aggregate::Mean => "mean",
            Aggregate::Min => "min",
            Aggregate::Max => "max",
            Aggregate::Last => "last",
        }
    }
}

/// The aggregates of a merged `grouping` table and whether they are suffixed to the series name:
/// a string keeps the name, a list suffixes `_<aggregate>`. `None` without `aggregate`.
pub fn aggregates_of(table: &serde_json::Map<String, serde_json::Value>) -> Option<(Vec<Aggregate>, bool)> {
    match table.get("aggregate")? {
        serde_json::Value::String(a) => Some((vec![Aggregate::of(a)?], false)),
        serde_json::Value::Array(list) => {
            let aggs: Option<Vec<Aggregate>> = list.iter().map(|v| v.as_str().and_then(Aggregate::of)).collect();
            Some((aggs?, true))
        }
        _ => None,
    }
}

/// Merge one level's `grouping` table over `base`. A table that sets `aggregate` without `by`
/// chooses `"sample"` (§5.5), at its own level, before the merge.
pub fn merge_grouping_level(base: &mut serde_json::Map<String, serde_json::Value>, level: Option<&serde_json::Value>) {
    let Some(serde_json::Value::Object(level)) = level else {
        return;
    };
    for (k, v) in level {
        base.insert(k.clone(), v.clone());
    }
    if level.contains_key("aggregate") && !level.contains_key("by") {
        base.insert("by".into(), serde_json::Value::String("sample".into()));
    }
}

/// Validate the values of one `grouping` table as written (or rewritten from `batch`) —
/// connector, device or point, inline or in a library. Its keys are checked with the other
/// contract tables (`library.rs`); the caller prefixes the place. The C loader
/// (impl/c/sdk/src/config.c `check_grouping_values`) refuses the same tables with the same
/// messages.
pub fn check_values(table: &toml::Value) -> Result<(), String> {
    let Some(map) = table.as_table() else {
        return Err("grouping must be a table".to_string());
    };
    if let Some(v) = map.get("by") {
        let by = match v.as_str() {
            Some(g @ ("time" | "frame" | "sample")) => g,
            _ => return Err("grouping.by must be \"time\", \"frame\" or \"sample\"".to_string()),
        };
        // A table that chooses its policy may only set that policy's keys.
        for (key, policy) in [("window", "frame"), ("every", "sample"), ("aggregate", "sample")] {
            if map.contains_key(key) && by != policy {
                return Err(format!("grouping.{key} does not apply to by = \"{by}\""));
            }
        }
    }
    let within = |key: &str, min: Duration, max: Duration, range: &str| match map.get(key) {
        None => Ok(()),
        Some(v) => match v.as_str().and_then(parse_duration) {
            Some(d) if d >= min && d <= max => Ok(()),
            _ => Err(format!("grouping.{key} must be a duration from {range}")),
        },
    };
    within("window", WINDOW_MIN, WINDOW_MAX, "1ms to 60s")?;
    within("every", EVERY_MIN, EVERY_MAX, "100ms to 1h")?;
    if let Some(v) = map.get("aggregate") {
        let names: Option<Vec<&str>> = match v {
            toml::Value::String(a) => Some(vec![a.as_str()]),
            toml::Value::Array(list) if !list.is_empty() => list.iter().map(|v| v.as_str()).collect(),
            _ => None,
        };
        let valid = names.is_some_and(|names| {
            names.iter().all(|n| AGGREGATES.contains(n))
                && names.iter().enumerate().all(|(i, n)| !names[..i].contains(n))
        });
        if !valid {
            return Err("grouping.aggregate must be \"mean\", \"min\", \"max\" or \"last\", or a list of them without repeats"
                .to_string());
        }
    }
    Ok(())
}

/// Check what only the resolved configuration decides (§5.5), after each table was checked: an
/// aggregated point has no `report` policy, and in grouped output no series of an aggregated
/// point has the name of another series of its device, type and fragment. Messages name the
/// place.
pub fn check_resolved(config: &ConnectorConfig) -> Result<(), String> {
    let naming = Naming::of(config);
    let protocol = config.connector.protocol.as_str();
    for device in &config.devices {
        let mut names: HashMap<(String, String, String), (String, bool)> = HashMap::new();
        for point in &device.points {
            let table = config.grouping_table(device, point);
            let aggregated = match GroupBy::from_table(&table) {
                Ok(GroupBy::Sample(_)) if !every_event(point) => aggregates_of(&table),
                _ => None,
            };
            if aggregated.is_some()
                && !crate::report::ReportPolicy::from_table(&config.report_table(device, point)).is_passthrough()
            {
                return Err(format!(
                    "device '{}': point '{}': grouping.aggregate cannot be combined with report: an aggregate uses every reading",
                    device.name, point.id
                ));
            }
            if Output::of(config) != Output::Group || !measurable(point, device) {
                continue;
            }
            let name = naming.resolve(protocol, device, point);
            let series: Vec<String> = match &aggregated {
                Some((aggs, true)) => aggs.iter().map(|a| format!("{}_{}", name.series, a.name())).collect(),
                _ => vec![name.series.clone()],
            };
            for series in series {
                let key = (name.ty.clone(), name.fragment.clone(), series.clone());
                let mine = aggregated.is_some();
                if let Some((other, theirs)) = names.get(&key) {
                    if mine || *theirs {
                        return Err(format!(
                            "device '{}': points '{other}' and '{}' both publish the series '{series}' in the fragment '{}' of the type '{}'",
                            device.name, point.id, name.fragment, name.ty
                        ));
                    }
                    continue;
                }
                names.insert(key, (point.id.clone(), mine));
            }
        }
    }
    Ok(())
}

/// Validate a `measurement` table's values: `[connector]`'s (`connector` true) or a device's.
/// The caller prefixes the place.
pub fn check_measurement(table: &toml::Value, connector: bool) -> Result<(), String> {
    let Some(map) = table.as_table() else {
        return Err("measurement must be a table".to_string());
    };
    let strings: &[&str] = if connector {
        &["type", "fragment", "series", "group", "point_separator"]
    } else {
        &["type", "fragment", "series", "group"]
    };
    for key in strings {
        if map.get(*key).is_some_and(|v| !v.is_str()) {
            return Err(format!("measurement.{key} must be a string"));
        }
    }
    if connector && map.get("include_boolean").is_some_and(|v| !v.is_bool()) {
        return Err("measurement.include_boolean must be true or false".to_string());
    }
    Ok(())
}

/// The names one level gives a series (§5.5): the point's `meta.measurement`, a device's or
/// `[connector]`'s `measurement` table. `group` names the type and the fragment of a level that
/// does not name them itself. Empty when the level does not name it.
#[derive(Clone, Debug, Default, PartialEq)]
struct Level {
    ty: String,
    fragment: String,
    series: String,
}

impl Level {
    fn of(table: Option<&serde_json::Value>) -> Self {
        let text = |key: &str| {
            table
                .and_then(|t| t.get(key))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string()
        };
        let group = text("group");
        let or_group = |s: String| if s.is_empty() { group.clone() } else { s };
        Level { ty: or_group(text("type")), fragment: or_group(text("fragment")), series: text("series") }
    }
}

/// A series' resolved names.
#[derive(Clone, Debug, PartialEq)]
struct Name {
    ty: String,
    fragment: String,
    series: String,
}

/// The connector-wide naming settings of `[connector] measurement`, as the `ot-measurement`
/// flow's parameters of the same names.
#[derive(Clone, Debug, PartialEq)]
struct Naming {
    connector: Level,
    separator: String,
    include_boolean: bool,
}

impl Naming {
    fn of(config: &ConnectorConfig) -> Self {
        let table = config.connector.measurement.as_ref();
        Naming {
            connector: Level::of(table),
            separator: table
                .and_then(|t| t.get("point_separator"))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string(),
            include_boolean: table
                .and_then(|t| t.get("include_boolean"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true),
        }
    }

    /// A point's type, fragment and series, in `ot-measurement`'s order, key by key: the point's
    /// `meta.measurement`, the device's `measurement`, `[connector] measurement`, then a split of
    /// the point id at `point_separator` (the prefix for the type and the fragment, the suffix
    /// for the series), then the protocol for the type, the type for the fragment and the point
    /// id for the series.
    fn resolve(&self, protocol: &str, device: &DeviceConfig, point: &PointConfig) -> Name {
        let meta = point
            .meta
            .as_ref()
            .and_then(|m| m.get("measurement"))
            .filter(|m| m.is_object());
        let levels = [Level::of(meta), Level::of(device.measurement.as_ref()), self.connector.clone()];
        let first = |pick: fn(&Level) -> &String| levels.iter().map(pick).find(|s| !s.is_empty()).cloned();
        let sep = self.separator.as_str();
        let split = (!sep.is_empty())
            .then(|| point.id.find(sep))
            .flatten()
            .filter(|at| *at > 0 && at + sep.len() < point.id.len())
            .map(|at| (point.id[..at].to_string(), point.id[at + sep.len()..].to_string()));
        let ty = first(|l| &l.ty)
            .or_else(|| split.as_ref().map(|s| s.0.clone()))
            .unwrap_or_else(|| protocol.to_string());
        let fragment = first(|l| &l.fragment)
            .or_else(|| split.as_ref().map(|s| s.0.clone()))
            .unwrap_or_else(|| ty.clone());
        let series = first(|l| &l.series)
            .or_else(|| split.as_ref().map(|s| s.1.clone()))
            .unwrap_or_else(|| point.id.clone());
        Name { ty, fragment, series }
    }
}

/// Whether a flow reads this point's samples, so grouped output still publishes them (§5.5):
/// `meta.alarm`, `meta.event` or `meta.parameter` (each unless `false`), write access, or
/// `sample = true`. `sample = false` turns them off whatever else the point has.
pub fn publishes_sample(point: &PointConfig) -> bool {
    if let Some(explicit) = point.sample {
        return explicit;
    }
    let flagged = |key: &str| {
        point
            .meta
            .as_ref()
            .and_then(|m| m.get(key))
            .is_some_and(|v| !v.is_null() && *v != serde_json::Value::Bool(false))
    };
    flagged("alarm")
        || flagged("event")
        || flagged("parameter")
        || Access::parse(point.access.as_deref()).can_write()
}

/// Whether readings of this point can become series: a typed point not opted out with
/// `meta.measurement = false`.
fn measurable(point: &PointConfig, device: &DeviceConfig) -> bool {
    let opted_out = point
        .meta
        .as_ref()
        .and_then(|m| m.get("measurement"))
        .is_some_and(|v| *v == serde_json::Value::Bool(false));
    !opted_out && point.resolved_mode(device.default_mode) == Mode::Typed
}

/// Whether the point raises an event for every reading (`meta.event.every`).
fn every_event(point: &PointConfig) -> bool {
    point
        .meta
        .as_ref()
        .and_then(|m| m.pointer("/event/every"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false)
}

/// A message for the runtime to publish.
#[derive(Clone, Debug, PartialEq)]
pub struct Message {
    pub topic: String,
    pub payload: String,
    pub retain: bool,
}

/// What the runtime does with a reading besides the measurement.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Routing {
    /// Publish the sample envelope.
    pub sample: bool,
    /// The reading also went into the measurements: the envelope carries `"measured": true`.
    pub measured: bool,
}

/// One point's place in its device's table, resolved at load.
#[derive(Debug)]
struct PointPlan {
    /// Index into the device's `topics`.
    topic: usize,
    /// Index into the topic's `fragments`.
    fragment: usize,
    /// `"<series>"`, escaped and quoted for the body, followed by `:`.
    series_key: String,
    measurable: bool,
    sample: bool,
    group_by: GroupBy,
    /// Under `"sample"` with `aggregate`: each aggregate with its series key. Empty otherwise.
    aggregates: Vec<(Aggregate, String)>,
}

#[derive(Debug)]
struct Topic {
    topic: String,
    /// The measurement type the topic ends with.
    ty: String,
    /// `"<fragment>"`, escaped and quoted, in the order their first point is configured.
    fragments: Vec<String>,
    /// The retained units message for the topic (empty when no series of it has a unit).
    units: String,
    /// The units were settled since this batcher was built.
    units_settled: bool,
}

/// The series of a message not yet published, per fragment of its topic.
#[derive(Debug, Default)]
struct Body {
    /// Per fragment of the topic, `"a":1,"b":2` without the braces; empty when it holds none.
    parts: Vec<String>,
    /// The length of the message text with these series; meaningless while `n` is 0.
    len: usize,
    /// The fragments holding a series.
    fragments: usize,
    /// The series.
    n: usize,
}

impl Body {
    /// The text length after adding `piece` to fragment `f`, whose quoted name is `name_len`
    /// bytes long.
    fn len_with(&self, f: usize, name_len: usize, piece: usize) -> usize {
        let base = if self.n == 0 { FRAME_BYTES } else { self.len };
        if self.parts.get(f).is_none_or(String::is_empty) {
            base + usize::from(self.fragments > 0) + name_len + FRAGMENT_BYTES + piece
        } else {
            base + 1 + piece
        }
    }

    /// The estimated Cumulocity size after adding `piece` to fragment `f` (§5.5).
    fn estimate_with(&self, f: usize, name_len: usize, piece: usize) -> usize {
        self.len_with(f, name_len, piece) + SERIES_OVERHEAD * (self.n + 1)
    }

    fn push(&mut self, f: usize, name_len: usize, piece: &str) {
        self.len = self.len_with(f, name_len, piece.len());
        if self.parts.len() <= f {
            self.parts.resize(f + 1, String::new());
        }
        let part = &mut self.parts[f];
        if part.is_empty() {
            self.fragments += 1;
        } else {
            part.push(',');
        }
        part.push_str(piece);
        self.n += 1;
    }

    fn clear(&mut self) {
        for part in &mut self.parts {
            part.clear();
        }
        self.len = 0;
        self.fragments = 0;
        self.n = 0;
    }

    /// `{"time":"<time>","<fragment>":{<series>},…}`, the fragments in the topic's order.
    fn render(&self, fragments: &[String], time: OffsetDateTime) -> String {
        let mut out = String::with_capacity(self.len);
        out.push_str("{\"time\":\"");
        out.push_str(&format_rfc3339_ms(time));
        out.push('"');
        for (name, part) in fragments.iter().zip(&self.parts) {
            if part.is_empty() {
                continue;
            }
            out.push(',');
            out.push_str(name);
            out.push_str(":{");
            out.push_str(part);
            out.push('}');
        }
        out.push('}');
        out
    }
}

/// A message being filled: the open `time` message of a topic, or one layer of a frame.
#[derive(Debug)]
struct Open {
    time: OffsetDateTime,
    time_ms: i128,
    /// When the frame opened, on the device clock.
    opened: Instant,
    /// The part not yet published.
    body: Body,
    /// Every point in the message, including the parts already published by a split.
    points: Vec<usize>,
}

/// A point's slot under `"sample"`: the value it holds, and for an aggregated point what the
/// period held so far, weighted by the device clock.
#[derive(Clone, Copy, Debug)]
struct Slot {
    /// The good value the point holds, if any.
    value: Option<f64>,
    /// Since when it holds `value`, or since when the period's sums were last closed.
    since: Instant,
    /// The sum of each held value times the nanoseconds it held, this period.
    sum: f64,
    /// The good nanoseconds this period.
    good_ns: f64,
    /// The smallest and largest value held this period; `None` when none was.
    range: Option<(f64, f64)>,
}

impl Slot {
    fn new(now: Instant) -> Self {
        Slot { value: None, since: now, sum: 0.0, good_ns: 0.0, range: None }
    }

    /// Add the held value's time up to `now` to the period's sums.
    fn close(&mut self, now: Instant) {
        if let Some(v) = self.value {
            let ns = now.saturating_duration_since(self.since).as_nanos() as f64;
            self.sum += v * ns;
            self.good_ns += ns;
        }
        self.since = now;
    }

    /// Hold `value` from `now` on (`None`: nothing good is held).
    fn hold(&mut self, value: Option<f64>, now: Instant) {
        self.close(now);
        self.value = value;
        if let Some(v) = value {
            self.range = Some(self.range.map_or((v, v), |(lo, hi)| (lo.min(v), hi.max(v))));
        }
    }

    /// Start the next period with the value held now.
    fn restart(&mut self, now: Instant) {
        self.since = now;
        self.sum = 0.0;
        self.good_ns = 0.0;
        self.range = self.value.map(|v| (v, v));
    }
}

#[derive(Debug)]
struct DeviceTable {
    points: Vec<PointPlan>,
    index: HashMap<String, usize>,
    topics: Vec<Topic>,
    /// The point's values in its open message (`time`) or frame (`frame`): the layer its next
    /// value goes into.
    depth: Vec<u8>,
    /// The open message of a `time` key, or the layers of a `frame` key, in order.
    open: HashMap<(usize, GroupBy), Vec<Open>>,
    /// The time of the last layer of each `frame` key, in Unix milliseconds, across frames.
    last_layer_ms: HashMap<(usize, GroupBy), i128>,
    slots: Vec<Slot>,
    /// One per distinct `every` of the device: the period and the next sample time.
    samplers: Vec<(Duration, Instant)>,
    stale: bool,
}

/// The point tables of a connector's devices in grouped output, and the messages they produce.
#[derive(Debug)]
pub struct Batcher {
    devices: Vec<DeviceTable>,
    by_name: HashMap<String, usize>,
    include_boolean: bool,
    /// The units message last published per meta topic, kept across reloads so an unchanged one
    /// is not published again.
    published_units: HashMap<String, String>,
}

/// `s` as a JSON string.
fn quoted(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".to_string())
}

/// Append `n` to `out` as JSON: an integer without a fraction, else the shortest form that reads
/// back as the same double.
fn push_number(out: &mut String, n: f64) {
    if n.fract() == 0.0 && n.abs() < 1e15 {
        let _ = write!(out, "{}", n as i64);
    } else {
        out.push_str(ryu::Buffer::new().format_finite(n));
    }
}

impl Batcher {
    /// The tables for `config`'s devices. `previous` hands over the units already published, so
    /// a reload publishes only the ones that changed.
    pub fn new(config: &ConnectorConfig, now: Instant, previous: Option<Batcher>) -> Self {
        let naming = Naming::of(config);
        let protocol = config.connector.protocol.as_str();
        let mut devices = Vec::new();
        let mut by_name = HashMap::new();
        for device in &config.devices {
            let mut topics: Vec<Topic> = Vec::new();
            let mut units: Vec<BTreeMap<String, String>> = Vec::new();
            let mut points = Vec::new();
            let mut index = HashMap::new();
            let mut samplers: Vec<(Duration, Instant)> = Vec::new();
            for point in &device.points {
                let Name { ty, fragment, series } = naming.resolve(protocol, device, point);
                let topic = match topics.iter().position(|t| t.ty == ty) {
                    Some(t) => t,
                    None => {
                        topics.push(Topic {
                            topic: format!("te/device/{}///m/{ty}", device.name),
                            ty: ty.clone(),
                            fragments: Vec::new(),
                            units: String::new(),
                            units_settled: false,
                        });
                        units.push(BTreeMap::new());
                        topics.len() - 1
                    }
                };
                let fragment_json = quoted(&fragment);
                let fragments = &mut topics[topic].fragments;
                let fragment_index = match fragments.iter().position(|f| *f == fragment_json) {
                    Some(f) => f,
                    None => {
                        fragments.push(fragment_json);
                        fragments.len() - 1
                    }
                };
                let measurable = measurable(point, device);
                // Validated by the loader; a config built without it falls back to `time`.
                let table = config.grouping_table(device, point);
                let mut group_by = GroupBy::from_table(&table).unwrap_or(GroupBy::Time);
                if matches!(group_by, GroupBy::Sample(_)) && every_event(point) {
                    group_by = GroupBy::Time;
                }
                let names: Vec<(Option<Aggregate>, String)> = match aggregates_of(&table) {
                    Some((aggs, suffixed)) if matches!(group_by, GroupBy::Sample(_)) => aggs
                        .into_iter()
                        .map(|a| (Some(a), if suffixed { format!("{series}_{}", a.name()) } else { series.clone() }))
                        .collect(),
                    _ => vec![(None, series.clone())],
                };
                if let Some(unit) = point.unit.as_deref().filter(|u| measurable && !u.is_empty()) {
                    for (_, name) in &names {
                        units[topic].insert(format!("{fragment}.{name}"), unit.to_string());
                    }
                }
                let aggregates: Vec<(Aggregate, String)> = names
                    .into_iter()
                    .filter_map(|(a, name)| a.map(|a| (a, format!("{}:", quoted(&name)))))
                    .collect();
                if let GroupBy::Sample(every) = group_by {
                    if !samplers.iter().any(|(e, _)| *e == every) {
                        samplers.push((every, now + every));
                    }
                }
                index.insert(point.id.clone(), points.len());
                points.push(PointPlan {
                    topic,
                    fragment: fragment_index,
                    series_key: format!("{}:", quoted(&series)),
                    measurable,
                    sample: publishes_sample(point),
                    group_by,
                    aggregates,
                });
            }
            for (topic, units) in topics.iter_mut().zip(units) {
                if !units.is_empty() {
                    let map: serde_json::Map<String, serde_json::Value> = units
                        .into_iter()
                        .map(|(k, u)| (k, serde_json::json!({ "unit": u })))
                        .collect();
                    topic.units = serde_json::Value::Object(map).to_string();
                }
            }
            by_name.insert(device.name.clone(), devices.len());
            let n = points.len();
            devices.push(DeviceTable {
                points,
                index,
                topics,
                depth: vec![0; n],
                open: HashMap::new(),
                last_layer_ms: HashMap::new(),
                slots: vec![Slot::new(now); n],
                samplers,
                stale: false,
            });
        }
        Batcher {
            devices,
            by_name,
            include_boolean: naming.include_boolean,
            published_units: previous.map(|p| p.published_units).unwrap_or_default(),
        }
    }

    /// The series value of a reading, if it becomes one.
    fn series_value(&self, plan: &PointPlan, sample: &Sample) -> Option<f64> {
        if !plan.measurable || sample.quality != Quality::Good || sample.mode != Mode::Typed {
            return None;
        }
        match sample.value.as_ref()? {
            Value::Number(n) if n.is_finite() => Some(*n),
            Value::Bool(b) if self.include_boolean => Some(if *b { 1.0 } else { 0.0 }),
            _ => None,
        }
    }

    /// Take one reading the report policy let through. `now` is the device clock. Messages
    /// that close because of it are appended to `out`.
    pub fn offer(&mut self, sample: &Sample, now: Instant, out: &mut Vec<Message>) -> Routing {
        let Some((d, k)) = self.locate(&sample.device, &sample.point) else {
            // Not configured (a module that reports more than it was asked for): as before.
            return Routing { sample: true, measured: false };
        };
        let plan = &self.devices[d].points[k];
        let routing_sample = plan.sample;
        let value = self.series_value(plan, sample);
        let group_by = plan.group_by;
        let aggregated = !plan.aggregates.is_empty();
        let Some(value) = value else {
            if sample.quality != Quality::Good {
                self.devices[d].slots[k].hold(None, now);
            }
            return Routing { sample: routing_sample, measured: false };
        };
        match group_by {
            GroupBy::Sample(_) if aggregated => self.devices[d].slots[k].hold(Some(value), now),
            GroupBy::Sample(_) => self.devices[d].slots[k].value = Some(value),
            GroupBy::Time | GroupBy::Frame(_) => self.add(d, k, sample.ts, value, now, out),
        }
        Routing { sample: routing_sample, measured: true }
    }

    fn locate(&self, device: &str, point: &str) -> Option<(usize, usize)> {
        let d = *self.by_name.get(device)?;
        let k = *self.devices[d].index.get(point)?;
        Some((d, k))
    }

    fn add(&mut self, d: usize, k: usize, ts: OffsetDateTime, value: f64, now: Instant, out: &mut Vec<Message>) {
        let ts_ms = ts.unix_timestamp_nanos().div_euclid(1_000_000);
        let dev = &self.devices[d];
        let plan = &dev.points[k];
        let key = (plan.topic, plan.group_by);
        let depth = dev.depth[k] as usize;
        // The window a new frame opens in: a frame that reached its layer cap keeps its window.
        let mut opened = now;
        if let Some(layers) = dev.open.get(&key) {
            let first = &layers[0];
            let close = match plan.group_by {
                GroupBy::Frame(window) if first.opened + window <= now => true,
                GroupBy::Frame(_) if depth >= MAX_LAYERS => {
                    opened = first.opened;
                    true
                }
                GroupBy::Frame(_) => false,
                _ => depth > 0 || first.time_ms != ts_ms,
            };
            if close {
                self.close(d, key, out);
            }
        }
        let dev = &mut self.devices[d];
        let layer = dev.depth[k] as usize;
        let layers = dev.open.entry(key).or_default();
        if layer == layers.len() {
            let (time, time_ms) = match key.1 {
                GroupBy::Frame(_) => {
                    // Layer times strictly increase per key, so a point's values stay in order.
                    match dev.last_layer_ms.get(&key) {
                        Some(&last) if ts_ms <= last => {
                            let ms = last + 1;
                            (OffsetDateTime::from_unix_timestamp_nanos(ms * 1_000_000).unwrap_or(ts), ms)
                        }
                        _ => (ts, ts_ms),
                    }
                }
                _ => (ts, ts_ms),
            };
            if matches!(key.1, GroupBy::Frame(_)) {
                dev.last_layer_ms.insert(key, time_ms);
            }
            let opened = layers.first().map_or(opened, |o| o.opened);
            layers.push(Open { time, time_ms, opened, body: Body::default(), points: Vec::new() });
        }
        let plan = &dev.points[k];
        let f = plan.fragment;
        let name_len = dev.topics[plan.topic].fragments[f].len();
        let mut piece = String::with_capacity(plan.series_key.len() + 24);
        piece.push_str(&plan.series_key);
        push_number(&mut piece, value);
        let open = &mut layers[layer];
        if open.body.n > 0 && open.body.estimate_with(f, name_len, piece.len()) > SIZE_BUDGET {
            // Split: the full part goes now; the rest keeps the message's time and window.
            let topic = &mut dev.topics[key.0];
            settle_units(topic, &mut self.published_units, out);
            out.push(Message { topic: topic.topic.clone(), payload: open.body.render(&topic.fragments, open.time), retain: false });
            open.body.clear();
        }
        open.body.push(f, name_len, &piece);
        open.points.push(k);
        dev.depth[k] += 1;
    }

    /// Publish the open message, or every layer of the frame, of `key` on device `d`, after its
    /// topic's units when they are not published yet.
    fn close(&mut self, d: usize, key: (usize, GroupBy), out: &mut Vec<Message>) {
        let dev = &mut self.devices[d];
        let Some(layers) = dev.open.remove(&key) else {
            return;
        };
        let topic = &mut dev.topics[key.0];
        for open in layers {
            for k in &open.points {
                dev.depth[*k] = 0;
            }
            if open.body.n == 0 {
                continue;
            }
            settle_units(topic, &mut self.published_units, out);
            out.push(Message {
                topic: topic.topic.clone(),
                payload: open.body.render(&topic.fragments, open.time),
                retain: false,
            });
        }
    }

    /// The end of a runtime pass: publish every open `time` message.
    pub fn end_of_pass(&mut self, out: &mut Vec<Message>) {
        for d in 0..self.devices.len() {
            if self.devices[d].open.is_empty() {
                continue;
            }
            let mut keys: Vec<(usize, GroupBy)> = self.devices[d]
                .open
                .keys()
                .filter(|(_, g)| *g == GroupBy::Time)
                .copied()
                .collect();
            keys.sort_by_key(|(t, _)| *t);
            for key in keys {
                self.close(d, key, out);
            }
        }
    }

    /// Publish every open message, frames included: the table is about to be rebuilt or the
    /// connector stops.
    pub fn flush_all(&mut self, out: &mut Vec<Message>) {
        for d in 0..self.devices.len() {
            let mut keys: Vec<(usize, GroupBy)> = self.devices[d].open.keys().copied().collect();
            keys.sort_by_key(|(t, _)| *t);
            for key in keys {
                self.close(d, key, out);
            }
        }
    }

    /// Publish the frames whose window has ended and the samples that are due at `now` (device
    /// clock); `wall` is the time samples are stamped with.
    pub fn due(&mut self, now: Instant, wall: OffsetDateTime, out: &mut Vec<Message>) {
        for d in 0..self.devices.len() {
            let mut ended: Vec<(Instant, (usize, GroupBy))> = self.devices[d]
                .open
                .iter()
                .filter_map(|(key, layers)| match key.1 {
                    GroupBy::Frame(window) if layers[0].opened + window <= now => Some((layers[0].opened, *key)),
                    _ => None,
                })
                .collect();
            ended.sort_by_key(|(opened, key)| (*opened, key.0));
            for (_, key) in ended {
                self.close(d, key, out);
            }
            for i in 0..self.devices[d].samplers.len() {
                let (every, next) = self.devices[d].samplers[i];
                if next > now {
                    continue;
                }
                self.sample(d, every, now, wall, out);
                // On the period's grid; a sampler that fell a whole period behind restarts.
                let mut next = next + every;
                if next <= now {
                    next = now + every;
                }
                self.devices[d].samplers[i].1 = next;
            }
        }
    }

    /// Publish one sample of device `d`'s slots sampled every `every`. A stale device's plain
    /// slots are left out; an aggregated slot publishes the good time it had before.
    fn sample(&mut self, d: usize, every: Duration, now: Instant, wall: OffsetDateTime, out: &mut Vec<Message>) {
        let dev = &mut self.devices[d];
        let mut bodies: Vec<Body> = (0..dev.topics.len()).map(|_| Body::default()).collect();
        let mut series: Vec<(&str, f64)> = Vec::new();
        for (k, plan) in dev.points.iter().enumerate() {
            if plan.group_by != GroupBy::Sample(every) {
                continue;
            }
            series.clear();
            let slot = &mut dev.slots[k];
            if plan.aggregates.is_empty() {
                match slot.value {
                    Some(value) if !dev.stale => series.push((&plan.series_key, value)),
                    _ => continue,
                }
            } else {
                slot.close(now);
                if slot.good_ns > 0.0 {
                    for (aggregate, key) in &plan.aggregates {
                        let value = match aggregate {
                            Aggregate::Mean => Some(slot.sum / slot.good_ns),
                            Aggregate::Min => slot.range.map(|r| r.0),
                            Aggregate::Max => slot.range.map(|r| r.1),
                            Aggregate::Last => slot.value,
                        };
                        if let Some(value) = value {
                            series.push((key, value));
                        }
                    }
                }
                slot.restart(now);
            }
            for &(key, value) in &series {
                let mut piece = key.to_string();
                push_number(&mut piece, value);
                let topic = &mut dev.topics[plan.topic];
                let body = &mut bodies[plan.topic];
                let name_len = topic.fragments[plan.fragment].len();
                if body.n > 0 && body.estimate_with(plan.fragment, name_len, piece.len()) > SIZE_BUDGET {
                    settle_units(topic, &mut self.published_units, out);
                    out.push(Message { topic: topic.topic.clone(), payload: body.render(&topic.fragments, wall), retain: false });
                    body.clear();
                }
                body.push(plan.fragment, name_len, &piece);
            }
        }
        for (t, body) in bodies.into_iter().enumerate() {
            if body.n == 0 {
                continue;
            }
            let topic = &mut dev.topics[t];
            settle_units(topic, &mut self.published_units, out);
            out.push(Message { topic: topic.topic.clone(), payload: body.render(&topic.fragments, wall), retain: false });
        }
    }

    /// The earliest device-clock time [`Batcher::due`] has something to do, if any.
    pub fn next_deadline(&self) -> Option<Instant> {
        self.devices
            .iter()
            .flat_map(|dev| {
                let frames = dev.open.iter().filter_map(|(key, layers)| match key.1 {
                    GroupBy::Frame(window) => Some(layers[0].opened + window),
                    _ => None,
                });
                frames.chain(dev.samplers.iter().map(|(_, next)| *next))
            })
            .min()
    }

    /// Mark a device's slots stale (its link is down or its push delivery failed its check), or
    /// healthy again. A stale device's slots are left out of samples.
    /// `now` is the device clock: an aggregated slot stops counting good time when it goes
    /// stale, and counts again from its next good reading.
    pub fn set_stale(&mut self, device: &str, stale: bool, now: Instant) {
        if let Some(&d) = self.by_name.get(device) {
            let dev = &mut self.devices[d];
            if stale && !dev.stale {
                for (plan, slot) in dev.points.iter().zip(dev.slots.iter_mut()) {
                    if !plan.aggregates.is_empty() {
                        slot.hold(None, now);
                    }
                }
            }
            dev.stale = stale;
        }
    }
}

/// Publish a topic's units before its first measurement, when they differ from what was last
/// published for it. An emptied map clears the retained message.
fn settle_units(topic: &mut Topic, published: &mut HashMap<String, String>, out: &mut Vec<Message>) {
    if topic.units_settled {
        return;
    }
    topic.units_settled = true;
    let meta = format!("{}/meta", topic.topic);
    let before = published.get(&meta).map(String::as_str);
    if before == Some(topic.units.as_str()) || (before.is_none() && topic.units.is_empty()) {
        return;
    }
    published.insert(meta.clone(), topic.units.clone());
    out.push(Message { topic: meta, payload: topic.units.clone(), retain: true });
}

/// The `outputs` of the capability descriptor (§7): each device's effective output and its
/// merged `grouping` table (`[connector]`, then the device), with the default `by` filled in.
/// `None` for a connector in single output that configures no `grouping`, so a configuration
/// using only defaults adds nothing.
pub fn describe(config: &ConnectorConfig) -> Option<serde_json::Value> {
    let output = Output::of(config);
    let any_grouping = config.connector.grouping.is_some() || config.devices.iter().any(|d| d.grouping.is_some());
    if output == Output::Single && !any_grouping {
        return None;
    }
    let devices = config
        .devices
        .iter()
        .map(|device| {
            let mut grouping = serde_json::Map::new();
            merge_grouping_level(&mut grouping, config.connector.grouping.as_ref());
            merge_grouping_level(&mut grouping, device.grouping.as_ref());
            grouping
                .entry("by")
                .or_insert_with(|| serde_json::Value::String("time".into()));
            serde_json::json!({ "device": device.name, "output": output.as_str(), "grouping": grouping })
        })
        .collect();
    Some(serde_json::Value::Array(devices))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(v: serde_json::Value) -> serde_json::Map<String, serde_json::Value> {
        v.as_object().unwrap().clone()
    }

    #[test]
    fn group_by_needs_its_duration() {
        use serde_json::json;
        assert_eq!(GroupBy::from_table(&table(json!({}))), Ok(GroupBy::Time));
        assert_eq!(
            GroupBy::from_table(&table(json!({"by": "frame", "window": "100ms"}))),
            Ok(GroupBy::Frame(Duration::from_millis(100)))
        );
        assert_eq!(
            GroupBy::from_table(&table(json!({"by": "frame"}))),
            Err("grouping.window is required with by = \"frame\"".into())
        );
        // A point choosing `time` under a device that chose `frame` inherits the window.
        assert_eq!(
            GroupBy::from_table(&table(json!({"by": "time", "window": "100ms"}))),
            Ok(GroupBy::Time)
        );
    }

    #[test]
    fn a_table_sets_only_its_own_policy_s_duration() {
        let check = |t: &str| check_values(&toml::from_str::<toml::Value>(t).unwrap());
        assert_eq!(check("by = \"frame\"\nwindow = \"100ms\""), Ok(()));
        assert_eq!(check("window = \"100ms\""), Ok(()));
        assert_eq!(
            check("by = \"time\"\nwindow = \"100ms\""),
            Err("grouping.window does not apply to by = \"time\"".into())
        );
        assert_eq!(
            check("by = \"bucket\""),
            Err("grouping.by must be \"time\", \"frame\" or \"sample\"".into())
        );
        assert_eq!(
            check("by = \"frame\"\nwindow = \"2m\""),
            Err("grouping.window must be a duration from 1ms to 60s".into())
        );
        assert_eq!(
            check("by = \"sample\"\nevery = \"50ms\""),
            Err("grouping.every must be a duration from 100ms to 1h".into())
        );
    }

    const VECTORS: &str = include_str!("../../../../../doc/contract/test-vectors/measurements/vectors.json");

    fn at(base: Instant, step: &serde_json::Value) -> Instant {
        base + Duration::from_millis(step["at"].as_u64().unwrap())
    }

    fn parse_ts(s: &str) -> OffsetDateTime {
        OffsetDateTime::parse(s, &time::format_description::well_known::Rfc3339).unwrap()
    }

    /// The reading a vector step describes, as a module would return it for the configured point.
    fn reading(config: &ConnectorConfig, r: &serde_json::Value) -> Sample {
        let device = config.devices.iter().find(|d| d.name == r["device"]).unwrap();
        let point = device.points.iter().find(|p| p.id == r["point"]).unwrap();
        let mode = point.resolved_mode(device.default_mode);
        let value = match &r["value"] {
            serde_json::Value::Bool(b) => Value::Bool(*b),
            serde_json::Value::Number(n) => Value::Number(n.as_f64().unwrap()),
            serde_json::Value::String(t) => Value::Text(t.clone()),
            other => panic!("value {other}"),
        };
        Sample {
            ts: parse_ts(r["ts"].as_str().unwrap()),
            source_ts: None,
            replayed: false,
            device: device.name.clone(),
            protocol: "test",
            point: point.id.clone(),
            mode,
            datatype: point.datatype,
            value: (mode == Mode::Typed).then_some(value),
            raw: vec![0],
            raw_group: 1,
            quality: match r["quality"].as_str().unwrap_or("good") {
                "good" => Quality::Good,
                "bad" => Quality::Bad,
                _ => Quality::Stale,
            },
            unit: point.unit.clone(),
            addr: serde_json::Value::Null,
            seq: None,
            error: None,
            source_value: None,
        }
    }

    fn config_of(case: &serde_json::Value) -> ConnectorConfig {
        let mut text = case["config"].as_str().unwrap().to_string();
        if let Some(g) = case.get("generate") {
            let pattern = g["id"].as_str().unwrap();
            for i in 0..g["count"].as_u64().unwrap() {
                let id = pattern.replace("{i:04}", &format!("{i:04}"));
                text.push_str(&format!(
                    "[[device.point]]\nid = \"{id}\"\ndatatype = \"{}\"\naddress = {{}}\n",
                    g["datatype"].as_str().unwrap()
                ));
            }
        }
        crate::library::resolve(&text, std::path::Path::new(".")).unwrap()
    }

    /// The shared vectors (`doc/contract/test-vectors/measurements/`), also run by the C SDK.
    #[test]
    fn measurement_vectors() {
        let doc: serde_json::Value = serde_json::from_str(VECTORS).unwrap();
        let wall0 = parse_ts("2026-01-01T00:00:00.000Z");
        for case in doc["cases"].as_array().unwrap() {
            let name = case["name"].as_str().unwrap();
            let config = config_of(case);
            let base = Instant::now();
            let mut batcher = Batcher::new(&config, base, None);
            let mut out = Vec::new();
            let mut routing = Vec::new();
            for step in case["steps"].as_array().unwrap() {
                let now = at(base, step);
                if let Some(r) = step.get("read") {
                    let r = batcher.offer(&reading(&config, r), now, &mut out);
                    routing.push(serde_json::json!({ "sample": r.sample, "measured": r.measured }));
                } else if let Some(r) = step.get("read_all") {
                    let device = config.devices.iter().find(|d| d.name == r["device"]).unwrap();
                    for point in &device.points {
                        let mut one = r.clone();
                        one["point"] = serde_json::json!(point.id);
                        batcher.offer(&reading(&config, &one), now, &mut out);
                    }
                } else if step.get("pass").is_some() {
                    batcher.end_of_pass(&mut out);
                } else if step.get("due").is_some() {
                    let wall = wall0 + (now - base);
                    batcher.due(now, wall, &mut out);
                } else if let Some(st) = step.get("stale") {
                    batcher.set_stale(st["device"].as_str().unwrap(), st["stale"].as_bool().unwrap(), now);
                } else {
                    panic!("{name}: unknown step {step}");
                }
            }
            if let Some(split) = case.get("split") {
                let counts: Vec<usize> = out
                    .iter()
                    .map(|m| {
                        assert_eq!(m.topic, split["topic"], "{name}");
                        // Every fragment's series count, and the estimate stays within the budget.
                        let body: serde_json::Value = serde_json::from_str(&m.payload).unwrap();
                        let n: usize = body
                            .as_object()
                            .unwrap()
                            .iter()
                            .filter(|(k, _)| *k != "time")
                            .map(|(_, f)| f.as_object().unwrap().len())
                            .sum();
                        assert!(m.payload.len() + SERIES_OVERHEAD * n <= SIZE_BUDGET, "{name}: {}", m.payload.len());
                        n
                    })
                    .collect();
                let want: Vec<usize> = serde_json::from_value(split["series"].clone()).unwrap();
                assert_eq!(counts, want, "{name}");
                continue;
            }
            let got: Vec<serde_json::Value> = out
                .iter()
                .map(|m| {
                    let payload = if m.payload.is_empty() {
                        serde_json::json!("")
                    } else {
                        serde_json::from_str(&m.payload).unwrap()
                    };
                    serde_json::json!({ "topic": m.topic, "retain": m.retain, "payload": payload })
                })
                .collect();
            assert_eq!(serde_json::Value::Array(got), case["messages"], "{name}");
            assert_eq!(serde_json::Value::Array(routing), case["routing"], "{name}");
        }
    }

    /// The parts of a split layer share its time, and the next layer is ordered after that time,
    /// not after each part (the vectors' `split` form does not carry times).
    #[test]
    fn split_frame_layer_parts_share_its_time() {
        let case = serde_json::json!({
            "config": "[connector]\nprotocol = \"opcua\"\noutput = \"measurements\"\n\n[[device]]\nname = \"plc1\"\nprotocol_address = {}\nbatch = { group_by = \"frame\", window = \"100ms\" }\n",
            "generate": { "count": 300, "datatype": "float64", "id": "a_long_point_name_that_makes_each_series_take_about_a_hundred_bytes_of_the_message_{i:04}" },
        });
        let config = config_of(&case);
        let base = Instant::now();
        let mut batcher = Batcher::new(&config, base, None);
        let mut out = Vec::new();
        let read = |point: &str, ts: &str| reading(&config, &serde_json::json!({ "device": "plc1", "point": point, "value": 1, "ts": ts }));
        for p in &config.devices[0].points {
            batcher.offer(&read(&p.id, "2026-01-01T00:00:00.040Z"), base, &mut out);
        }
        let first = config.devices[0].points[0].id.clone();
        batcher.offer(&read(&first, "2026-01-01T00:00:00.030Z"), base + Duration::from_millis(10), &mut out);
        batcher.due(base + Duration::from_millis(100), parse_ts("2026-01-01T00:00:00.100Z"), &mut out);
        let times: Vec<String> = out
            .iter()
            .filter(|m| !m.retain)
            .map(|m| serde_json::from_str::<serde_json::Value>(&m.payload).unwrap()["time"].as_str().unwrap().to_string())
            .collect();
        assert!(times.len() >= 3, "{times:?}");
        let (last, parts) = times.split_last().unwrap();
        assert!(parts.iter().all(|t| t == "2026-01-01T00:00:00.040Z"), "{times:?}");
        assert_eq!(last, "2026-01-01T00:00:00.041Z");
    }

    fn load(extra: &str) -> Result<ConnectorConfig, String> {
        let text = format!(
            "[connector]\nprotocol = \"opcua\"\n{extra}\n[[device]]\nname = \"plc\"\nprotocol_address = {{}}\n[[device.point]]\nid = \"p\"\naddress = {{}}\n"
        );
        crate::library::resolve(&text, std::path::Path::new("."))
    }

    /// Invalid point `grouping`, `batch` and `sample` values and the end of the message each gets.
    const POINT_CASES: [(&str, &str); 16] = [
        ("grouping = { by = \"time\", window = \"100ms\" }", "point 'p': grouping.window does not apply to by = \"time\""),
        ("grouping = { by = \"frame\", window = \"2m\" }", "point 'p': grouping.window must be a duration from 1ms to 60s"),
        ("grouping = { by = \"sample\", every = \"50ms\" }", "point 'p': grouping.every must be a duration from 100ms to 1h"),
        ("grouping = { by = \"bucket\" }", "point 'p': grouping.by must be \"time\", \"frame\" or \"sample\""),
        ("grouping = { windw = \"1s\" }", "unknown key 'windw' in the grouping of point 'p' (did you mean 'window'?)"),
        ("sample = \"yes\"", "point 'p': sample must be true or false"),
        ("grouping = { by = \"frame\", window = \"100ms\", aggregate = \"mean\" }", "point 'p': grouping.aggregate does not apply to by = \"frame\""),
        ("grouping = { aggregate = \"median\", every = \"1s\" }", "point 'p': grouping.aggregate must be \"mean\", \"min\", \"max\" or \"last\", or a list of them without repeats"),
        ("grouping = { aggregate = [\"mean\", \"mean\"], every = \"1s\" }", "point 'p': grouping.aggregate must be \"mean\", \"min\", \"max\" or \"last\", or a list of them without repeats"),
        ("grouping = { aggregate = [], every = \"1s\" }", "point 'p': grouping.aggregate must be \"mean\", \"min\", \"max\" or \"last\", or a list of them without repeats"),
        ("grouping = { aggregate = \"mean\" }", "device 'plc': point 'p': grouping.every is required with by = \"sample\""),
        ("grouping = { aggregate = \"mean\", every = \"1s\" }\nreport = { deadband = 0.5 }", "device 'plc': point 'p': grouping.aggregate cannot be combined with report: an aggregate uses every reading"),
        // The deprecated `batch`: its keys are checked as written, its values as `grouping`'s.
        ("batch = { windw = \"1s\" }", "unknown key 'windw' in the batch of point 'p' (did you mean 'window'?)"),
        ("batch = { by = \"time\" }", "unknown key 'by' in the batch of point 'p'"),
        ("batch = { group_by = \"time\", window = \"100ms\" }", "point 'p': grouping.window does not apply to by = \"time\""),
        ("batch = { group_by = \"time\" }\ngrouping = { by = \"time\" }", "device 'plc': point 'p': batch and grouping cannot both be set (batch is the deprecated name of grouping)"),
    ];

    #[test]
    fn the_loader_names_the_place() {
        assert_eq!(
            load("output = \"both\"").unwrap_err(),
            "[connector] output must be one of \"single\", \"group\", \"samples\", \"measurements\" (got 'both')"
        );
        assert_eq!(
            load("grouping = { by = \"frame\" }").unwrap_err(),
            "device 'plc': point 'p': grouping.window is required with by = \"frame\""
        );
        assert_eq!(
            load("batch = { group_by = \"frame\" }").unwrap_err(),
            "device 'plc': point 'p': grouping.window is required with by = \"frame\""
        );
        assert_eq!(
            load("batch = {}\ngrouping = {}").unwrap_err(),
            "[connector] batch and grouping cannot both be set (batch is the deprecated name of grouping)"
        );
        assert_eq!(
            load("measurement = { type = 1 }").unwrap_err(),
            "[connector] measurement.type must be a string"
        );
        assert_eq!(
            load("measurement = { include_boolean = \"yes\" }").unwrap_err(),
            "[connector] measurement.include_boolean must be true or false"
        );
        assert_eq!(
            load("measurement = { grup = \"x\" }").unwrap_err(),
            "unknown key 'grup' in the measurement of [connector] (did you mean 'group'?)"
        );
        // Concurrent reads (§5.6), the same cases as tests/config.c.
        assert_eq!(load("max_inflight = 0").unwrap_err(), "[connector] max_inflight must be a positive integer");
        assert_eq!(
            load("max_inflight_total = \"many\"").unwrap_err(),
            "[connector] max_inflight_total must be a positive integer"
        );
        // The same cases as the C loader's tests/config.c
        // check_measurement_settings_are_validated, with the same messages.
        let point = |table: &str| {
            let text = format!(
                "[connector]\nprotocol = \"opcua\"\n[[device]]\nname = \"plc\"\nprotocol_address = {{}}\n[[device.point]]\nid = \"p\"\naddress = {{}}\n{table}\n"
            );
            crate::library::resolve(&text, std::path::Path::new(".")).unwrap_err()
        };
        for (table, message) in POINT_CASES {
            assert!(point(table).ends_with(message), "{table}: {}", point(table));
        }
        assert_eq!(Output::of(&load("").unwrap()), Output::Single);
        assert_eq!(Output::of(&load("output = \"single\"").unwrap()), Output::Single);
        assert_eq!(Output::of(&load("output = \"samples\"").unwrap()), Output::Single);
        assert_eq!(Output::of(&load("output = \"group\"").unwrap()), Output::Group);
        assert_eq!(Output::of(&load("output = \"measurements\"").unwrap()), Output::Group);
        assert_eq!(deprecated_output(&load("output = \"group\"").unwrap()), None);
        assert_eq!(
            deprecated_output(&load("output = \"measurements\"").unwrap()).as_deref(),
            Some("output = \"measurements\" is deprecated: use \"group\"")
        );
        // A suffixed series may not take another point's name (the same case as tests/config.c).
        let two = |output: &str| {
            crate::library::resolve(
                &format!(
                    "[connector]\nprotocol = \"opcua\"\n{output}\n[[device]]\nname = \"plc\"\nprotocol_address = {{}}\n\
                     [[device.point]]\nid = \"p\"\naddress = {{}}\ngrouping = {{ aggregate = [\"mean\", \"max\"], every = \"1s\" }}\n\
                     [[device.point]]\nid = \"p_max\"\naddress = {{}}\n"
                ),
                std::path::Path::new("."),
            )
        };
        assert_eq!(
            two("output = \"group\"").unwrap_err(),
            "device 'plc': points 'p' and 'p_max' both publish the series 'p_max' in the fragment 'opcua' of the type 'opcua'"
        );
        assert!(two("").is_ok(), "single output publishes no series");
        // Another fragment of the same type is another series.
        assert!(two("output = \"group\"\n[connector.measurement]\npoint_separator = \"_\"").is_ok());
    }

    #[test]
    fn the_descriptor_lists_each_device_s_output_and_grouping() {
        assert_eq!(describe(&load("").unwrap()), None);
        // `aggregate` without `by` chooses "sample" (the same case as tests/config.c).
        assert_eq!(
            describe(&load("output = \"group\"\ngrouping = { aggregate = [\"mean\", \"max\"], every = \"1s\" }").unwrap()),
            Some(serde_json::json!([
                { "device": "plc", "output": "group",
                  "grouping": { "by": "sample", "every": "1s", "aggregate": ["mean", "max"] } }
            ]))
        );
        // The deprecated names are reported under the new ones.
        assert_eq!(
            describe(&load("output = \"measurements\"\nbatch = { group_by = \"frame\", window = \"100ms\" }").unwrap()),
            Some(serde_json::json!([
                { "device": "plc", "output": "group", "grouping": { "by": "frame", "window": "100ms" } }
            ]))
        );
    }

    /// The names a point gets (§5.5), key by key: point meta, device, connector, the separator,
    /// then the defaults; `group` names the type and the fragment of its level.
    #[test]
    fn names_resolve_key_by_key() {
        let name = |connector: &str, device: &str, point_id: &str, meta: &str| {
            let text = format!(
                "[connector]\nprotocol = \"opcua\"\n{connector}\n[[device]]\nname = \"plc\"\nprotocol_address = {{}}\n{device}\n\
                 [[device.point]]\nid = \"{point_id}\"\naddress = {{}}\n{meta}\n"
            );
            let config = crate::library::resolve(&text, std::path::Path::new(".")).unwrap();
            let n = Naming::of(&config).resolve("opcua", &config.devices[0], &config.devices[0].points[0]);
            (n.ty, n.fragment, n.series)
        };
        let t = |a: &str, b: &str, c: &str| (a.to_string(), b.to_string(), c.to_string());
        assert_eq!(name("", "", "flow", ""), t("opcua", "opcua", "flow"));
        // The fragment defaults to the type.
        assert_eq!(name("", "measurement = { type = \"c8y_Sensor\" }", "flow", ""), t("c8y_Sensor", "c8y_Sensor", "flow"));
        assert_eq!(
            name("measurement = { type = \"Combined\" }", "", "b", "meta = { measurement = { fragment = \"c8y_Battery\", series = \"level\" } }"),
            t("Combined", "c8y_Battery", "level")
        );
        // The separator names the fragment under a type, and the type too without one.
        assert_eq!(
            name("measurement = { type = \"Combined\", point_separator = \".\" }", "", "c8y_Battery.level", ""),
            t("Combined", "c8y_Battery", "level")
        );
        assert_eq!(
            name("measurement = { point_separator = \".\" }", "", "Environment.Temperature", ""),
            t("Environment", "Environment", "Temperature")
        );
        // `group` sets both at its level and wins over a lower level's type.
        assert_eq!(
            name("measurement = { type = \"Combined\" }", "", "p", "meta = { measurement = { group = \"Env\" } }"),
            t("Env", "Env", "p")
        );
        assert_eq!(
            name("measurement = { group = \"G\", fragment = \"F\" }", "", "p", ""),
            t("G", "F", "p")
        );
        // The device sits between the point and the connector.
        assert_eq!(
            name("measurement = { type = \"C\", series = \"s\" }", "measurement = { type = \"D\" }", "p", "meta = { measurement = { series = \"m\" } }"),
            t("D", "D", "m")
        );
    }

    /// Two fragments of one type and time share one message; the size estimate counts every
    /// fragment, and is the text length plus 12 bytes per series.
    #[test]
    fn several_fragments_share_a_message() {
        let mut body = Body::default();
        let names = vec![quoted("c8y_Environmental"), quoted("c8y_Battery")];
        let ts = parse_ts("2026-11-12T12:23:00.000Z");
        for (f, piece) in [(1, "\"level\":85"), (0, "\"temperature\":23.4"), (1, "\"voltage\":3.7")] {
            let estimate = body.estimate_with(f, names[f].len(), piece.len());
            body.push(f, names[f].len(), piece);
            assert_eq!(estimate, body.len + SERIES_OVERHEAD * body.n);
            assert_eq!(body.len, body.render(&names, ts).len());
        }
        assert_eq!(
            body.render(&names, ts),
            "{\"time\":\"2026-11-12T12:23:00.000Z\",\"c8y_Environmental\":{\"temperature\":23.4},\"c8y_Battery\":{\"level\":85,\"voltage\":3.7}}"
        );
    }

    #[test]
    fn numbers_are_integers_or_shortest() {
        let s = |n: f64| {
            let mut out = String::new();
            push_number(&mut out, n);
            out
        };
        assert_eq!(s(118.0), "118");
        assert_eq!(s(-3.0), "-3");
        assert_eq!(s(118.6), "118.6");
        assert_eq!(s(0.1 + 0.2), "0.30000000000000004");
        assert_eq!(s(1e20), "1e20");
    }
}
