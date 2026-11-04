//! The reporting policy (report by exception, contract §5.3).
//!
//! A point's `report` table decides which of its readings are published: only changes, only
//! changes beyond a deadband, at most once per interval, once a change has settled — and a
//! heartbeat that still publishes a fresh reading when nothing changes. The runtime applies it
//! in front of the sample topic, so every connector behaves alike and no module filters.
//!
//! [`ReportState`] is the per-point state machine. It is pure — no clock, no MQTT, generic over
//! what it holds back — so the runtime drives it with the monotonic time and the shared test
//! vectors (`doc/contract/test-vectors/report/`) drive it here and in the C SDK
//! (impl/c/sdk/src/report.c), which implements the same rules.

use crate::config::parse_duration;
use crate::model::{hex_grouped, Quality, Sample, Value};
use std::time::Duration;

/// The keys a `report` table may carry.
pub const REPORT_KEYS: &[&str] = &[
    "on_change",
    "deadband",
    "min_interval",
    "max_interval",
    "debounce",
];

/// Differences up to this are not a change for `on_change` without a deadband.
const EPSILON: f64 = 1e-9;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Deadband {
    /// An absolute amount, in the point's transformed units.
    Absolute(f64),
    /// A percentage of the last published value's magnitude.
    Percent(f64),
}

/// A point's effective reporting policy. The default publishes every reading.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ReportPolicy {
    pub on_change: bool,
    pub deadband: Option<Deadband>,
    pub min_interval: Option<Duration>,
    pub max_interval: Option<Duration>,
    pub debounce: Option<Duration>,
}

impl ReportPolicy {
    /// Build the policy from a (merged) `report` table. Values are assumed validated
    /// ([`check_values`]); a zero duration or deadband switches the setting off.
    pub fn from_table(table: &serde_json::Map<String, serde_json::Value>) -> Self {
        let duration = |key: &str| {
            table
                .get(key)
                .and_then(|v| v.as_str())
                .and_then(parse_duration)
                .filter(|d| !d.is_zero())
        };
        let deadband = match table.get("deadband") {
            Some(serde_json::Value::Number(n)) => {
                n.as_f64().filter(|d| *d > 0.0).map(Deadband::Absolute)
            }
            Some(serde_json::Value::String(s)) => {
                parse_percent(s).filter(|p| *p > 0.0).map(Deadband::Percent)
            }
            _ => None,
        };
        ReportPolicy {
            on_change: table
                .get("on_change")
                .and_then(|v| v.as_bool())
                .unwrap_or(false),
            deadband,
            min_interval: duration("min_interval"),
            max_interval: duration("max_interval"),
            debounce: duration("debounce"),
        }
    }

    /// Whether the policy publishes every reading, so the runtime can skip it.
    pub fn is_passthrough(&self) -> bool {
        *self == ReportPolicy::default()
    }

    /// Whether readings are compared with the last published one.
    fn filters_changes(&self) -> bool {
        self.on_change || self.deadband.is_some() || self.debounce.is_some()
    }

    /// Whether `obs` is a change from `base` under this policy.
    fn changed(&self, obs: &Obs, base: &Obs) -> bool {
        match (obs, base) {
            (Obs::Number(a), Obs::Number(b)) => {
                if a.is_nan() || b.is_nan() {
                    return a.is_nan() != b.is_nan();
                }
                let delta = (a - b).abs();
                match self.deadband {
                    Some(Deadband::Absolute(d)) => delta >= d,
                    Some(Deadband::Percent(_)) if *b == 0.0 => delta > 0.0,
                    Some(Deadband::Percent(p)) => delta >= p / 100.0 * b.abs(),
                    None => delta > EPSILON,
                }
            }
            (Obs::Other(a), Obs::Other(b)) => a != b,
            _ => true,
        }
    }
}

/// `"<p>%"` as `p`.
fn parse_percent(s: &str) -> Option<f64> {
    let number = s.strip_suffix('%')?;
    let well_formed = !number.is_empty()
        && number.bytes().all(|b| b.is_ascii_digit() || b == b'.')
        && number.bytes().filter(|b| *b == b'.').count() <= 1
        && !number.starts_with('.')
        && !number.ends_with('.');
    well_formed.then(|| number.parse().ok()).flatten()
}

/// Validate the values of one `report` table as written — connector, device or point, inline
/// or in a library. Its keys are checked with the other contract tables (`library.rs`); the
/// caller prefixes the place. The C loader (impl/c/sdk/src/config.c `check_report_values`)
/// refuses the same tables with the same messages.
pub fn check_values(table: &toml::Value) -> Result<(), String> {
    let Some(map) = table.as_table() else {
        return Err("report must be a table".to_string());
    };
    if let Some(v) = map.get("on_change") {
        if !v.is_bool() {
            return Err("report.on_change must be true or false".to_string());
        }
    }
    if let Some(v) = map.get("deadband") {
        let ok = match v {
            toml::Value::Integer(i) => *i >= 0,
            toml::Value::Float(f) => f.is_finite() && *f >= 0.0,
            toml::Value::String(s) => parse_percent(s).is_some(),
            _ => false,
        };
        if !ok {
            return Err(
                "report.deadband must be a number >= 0 or a percentage such as \"2%\"".to_string(),
            );
        }
    }
    let duration = |key: &str| {
        match map.get(key) {
        None => Ok(None),
        Some(v) => v.as_str().and_then(parse_duration).map(Some).ok_or_else(|| {
            format!("report.{key} must be a duration such as \"500ms\", \"2s\" or \"5m\" (\"0\" switches it off)")
        }),
    }
    };
    let min = duration("min_interval")?;
    let max = duration("max_interval")?;
    duration("debounce")?;
    if let (Some(min), Some(max)) = (min, max) {
        if !min.is_zero() && !max.is_zero() && max <= min {
            return Err("report.max_interval must be longer than report.min_interval".to_string());
        }
    }
    Ok(())
}

/// The effective policy of a point whose merged table is `table`, and — when inheritance put a
/// heartbeat at or under the rate limit, which no single table may do — the warning to log. The
/// heartbeat is then raised to twice the rate limit, keeping it strictly longer.
pub fn effective(
    table: &serde_json::Map<String, serde_json::Value>,
) -> (ReportPolicy, Option<String>) {
    let mut policy = ReportPolicy::from_table(table);
    let mut warning = None;
    if let (Some(min), Some(max)) = (policy.min_interval, policy.max_interval) {
        if max <= min {
            let raised = min * 2;
            warning = Some(format!(
                "the inherited report.max_interval ({max:?}) is not longer than report.min_interval ({min:?}); using {raised:?}"
            ));
            policy.max_interval = Some(raised);
        }
    }
    (policy, warning)
}

/// Merge `over` into `base` key by key.
pub fn merge_into(
    base: &mut serde_json::Map<String, serde_json::Value>,
    over: Option<&serde_json::Value>,
) {
    if let Some(serde_json::Value::Object(over)) = over {
        for (k, v) in over {
            base.insert(k.clone(), v.clone());
        }
    }
}

/// What a reading is compared by: numbers numerically, everything else exactly.
#[derive(Clone, Debug, PartialEq)]
pub enum Obs {
    Number(f64),
    Other(String),
}

impl Obs {
    /// The comparable part of a sample: its value, or its raw bytes when it has none.
    pub fn of(sample: &Sample) -> Obs {
        match &sample.value {
            Some(Value::Number(n)) => Obs::Number(*n),
            Some(Value::Bool(b)) => Obs::Other(format!("b:{b}")),
            Some(Value::Text(t)) => Obs::Other(format!("s:{t}")),
            None => Obs::Other(format!("r:{}", hex_grouped(&sample.raw, 1))),
        }
    }
}

struct Candidate<T> {
    first: Obs,
    since: Duration,
    latest: T,
    latest_obs: Obs,
}

/// What a pass of the runtime loop asks of one point.
pub struct Due<T> {
    /// A held or settled reading to publish now.
    pub publish: Option<T>,
    /// Read the point on demand: its heartbeat is due and nothing delivers it.
    pub read: bool,
}

/// How long a point stays on source time after its last replayed reading, when no live reading
/// ends the replay first (§5.3).
pub const REPLAY_IDLE: Duration = Duration::from_secs(2);

/// Which clock the point's interval timers run on.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Clock {
    /// The runtime's monotonic clock: live readings.
    Runtime,
    /// Source time: the module is replaying history. `last_offer` is the runtime time of the
    /// last replayed reading.
    Replay { last_offer: Duration },
}

/// The reporting state of one point. `now` is monotonic time, from any fixed origin.
pub struct ReportState<T> {
    policy: ReportPolicy,
    /// When the state was created or last reset: the heartbeat's reference before any publish.
    origin: Duration,
    last: Option<(Obs, Quality)>,
    last_published: Option<Duration>,
    last_read: Option<Duration>,
    /// A heartbeat read returned nothing for the point; no more until a reset.
    unreadable: bool,
    pending: Option<(T, Obs)>,
    candidate: Option<Candidate<T>>,
    clock: Clock,
    /// The source time of the last reading published during a replay, to resume from when
    /// another replay starts.
    last_published_src: Option<Duration>,
}

impl<T> ReportState<T> {
    pub fn new(policy: ReportPolicy, now: Duration) -> Self {
        ReportState {
            policy,
            origin: now,
            last: None,
            last_published: None,
            last_read: None,
            unreadable: false,
            pending: None,
            candidate: None,
            clock: Clock::Runtime,
            last_published_src: None,
        }
    }

    pub fn policy(&self) -> &ReportPolicy {
        &self.policy
    }

    /// Forget everything, so the next reading is published: a reload, a device reconnect, or
    /// an MQTT session restore (a reading published while the broker was away may be lost).
    pub fn reset(&mut self, now: Duration) {
        *self = ReportState::new(std::mem::take(&mut self.policy), now);
    }

    /// A heartbeat read returned nothing for this point: it cannot be read on demand.
    pub fn no_data(&mut self) {
        self.unreadable = true;
    }

    fn since_published(&self, now: Duration) -> Duration {
        now.saturating_sub(self.last_published.unwrap_or(self.origin))
    }

    fn publish(&mut self, item: T, obs: Obs, quality: Quality, now: Duration) -> Option<T> {
        self.last = Some((obs, quality));
        self.last_published = Some(now);
        self.pending = None;
        self.candidate = None;
        Some(item)
    }

    /// Offer a reading. Returns it when it is to be published now; otherwise it is dropped or
    /// held (and may come back from [`due`](Self::due)).
    pub fn offer(&mut self, item: T, obs: Obs, quality: Quality, now: Duration) -> Option<T> {
        if self.clock != Clock::Runtime {
            self.hand_over(now);
        }
        self.offer_at(item, obs, quality, now)
    }

    /// Offer a reading the module replays from the source's history, with its source time
    /// `src` (from any fixed origin, such as the Unix epoch). The policy's intervals are measured
    /// on source time, so a burst of history is reduced the way the live stream would have been:
    /// a held or settled reading whose interval ended by `src` is published first. Returns the
    /// readings to publish now, in order.
    pub fn offer_replayed(&mut self, item: T, obs: Obs, quality: Quality, src: Duration, now: Duration) -> Vec<T> {
        if self.clock == Clock::Runtime {
            // Resume source time where the last replay left it; with none, the next reading
            // is far enough from the origin to count as a new interval.
            self.last_published = self.last_published_src;
            if let Some(c) = self.candidate.as_mut() {
                c.since = src;
            }
        }
        self.clock = Clock::Replay { last_offer: now };
        let mut out = Vec::new();
        out.extend(self.release(src));
        out.extend(self.offer_at(item, obs, quality, src));
        if self.last_published.is_some() {
            self.last_published_src = self.last_published;
        }
        out
    }

    /// End a replay: the timers of a held or debouncing reading restart on the runtime clock.
    fn hand_over(&mut self, now: Duration) {
        self.clock = Clock::Runtime;
        if self.last_published.is_some() {
            self.last_published_src = self.last_published;
            self.last_published = Some(now);
        }
        if let Some(c) = self.candidate.as_mut() {
            c.since = now;
        }
    }

    fn offer_at(&mut self, item: T, obs: Obs, quality: Quality, now: Duration) -> Option<T> {
        let Some((last_obs, last_quality)) = &self.last else {
            return self.publish(item, obs, quality, now);
        };
        if *last_quality != quality {
            return self.publish(item, obs, quality, now);
        }
        if self
            .policy
            .max_interval
            .is_some_and(|max| self.since_published(now) >= max)
        {
            return self.publish(item, obs, quality, now);
        }
        let changed_from_last = self.policy.changed(&obs, last_obs);

        let (item, obs) = match self.policy.debounce {
            None => (item, obs),
            Some(debounce) => {
                if !changed_from_last {
                    // Back at the published value: nothing settled or held is news any more.
                    self.candidate = None;
                    self.pending = None;
                    return None;
                }
                match self.candidate.take() {
                    Some(mut c) if !self.policy.changed(&obs, &c.first) => {
                        c.latest = item;
                        c.latest_obs = obs;
                        if now.saturating_sub(c.since) < debounce {
                            self.candidate = Some(c);
                            return None;
                        }
                        (c.latest, c.latest_obs)
                    }
                    _ => {
                        self.candidate = Some(Candidate {
                            first: obs.clone(),
                            since: now,
                            latest: item,
                            latest_obs: obs,
                        });
                        return None;
                    }
                }
            }
        };

        if self
            .policy
            .min_interval
            .is_some_and(|min| self.since_published(now) < min)
        {
            self.pending = Some((item, obs));
            return None;
        }
        // A newer reading supersedes anything held, whether or not it is published itself.
        self.pending = None;
        if self.policy.filters_changes()
            && !self.policy.changed(&obs, &self.last.as_ref().unwrap().0)
        {
            return None;
        }
        self.publish(item, obs, quality, now)
    }

    /// A pass of the runtime loop: publish a held reading whose interval ended, or a debounced
    /// one that settled, and say whether a pushed point's heartbeat read is due.
    pub fn due(&mut self, now: Duration, pushed: bool) -> Due<T> {
        if let Clock::Replay { last_offer } = self.clock {
            // Replayed readings release what they hold themselves, and a replay publishes, so
            // no heartbeat read is due while it goes on.
            if now.saturating_sub(last_offer) < REPLAY_IDLE {
                return Due { publish: None, read: false };
            }
            self.hand_over(now);
        }
        let out = self.release(now);
        let mut read = false;
        if let Some(max) = self.policy.max_interval {
            let reference = self
                .last_published
                .unwrap_or(self.origin)
                .max(self.last_read.unwrap_or(self.origin));
            if pushed && !self.unreadable && now.saturating_sub(reference) >= max {
                self.last_read = Some(now);
                read = true;
            }
        }
        Due { publish: out, read }
    }

    /// A held reading whose interval ended, or a debounced one that settled, by `now`.
    fn release(&mut self, now: Duration) -> Option<T> {
        let mut out = None;
        if let (Some(min), Some(_)) = (self.policy.min_interval, &self.pending) {
            if self.since_published(now) >= min {
                let (item, obs) = self.pending.take().unwrap();
                out = self.publish_if_changed(item, obs, now);
            }
        }
        if out.is_none() {
            if let (Some(debounce), Some(c)) = (self.policy.debounce, &self.candidate) {
                if now.saturating_sub(c.since) >= debounce {
                    let c = self.candidate.take().unwrap();
                    if self
                        .policy
                        .min_interval
                        .is_some_and(|min| self.since_published(now) < min)
                    {
                        self.pending = Some((c.latest, c.latest_obs));
                    } else {
                        out = self.publish_if_changed(c.latest, c.latest_obs, now);
                    }
                }
            }
        }
        out
    }

    fn publish_if_changed(&mut self, item: T, obs: Obs, now: Duration) -> Option<T> {
        let (last_obs, quality) = self.last.as_ref()?;
        if self.policy.filters_changes() && !self.policy.changed(&obs, last_obs) {
            return None;
        }
        let quality = *quality;
        self.publish(item, obs, quality, now)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const VECTORS: &str =
        include_str!("../../../../../doc/contract/test-vectors/report/vectors.json");

    fn obs_of(sample: &serde_json::Value) -> Obs {
        if let Some(n) = sample.get("num").and_then(|v| v.as_f64()) {
            Obs::Number(n)
        } else if sample.get("nan").is_some() {
            Obs::Number(f64::NAN)
        } else if let Some(b) = sample.get("bool").and_then(|v| v.as_bool()) {
            Obs::Other(format!("b:{b}"))
        } else if let Some(s) = sample.get("str").and_then(|v| v.as_str()) {
            Obs::Other(format!("s:{s}"))
        } else {
            Obs::Other(format!(
                "r:{}",
                sample.get("raw").and_then(|v| v.as_str()).unwrap_or("")
            ))
        }
    }

    fn quality_of(sample: &serde_json::Value) -> Quality {
        match sample
            .get("quality")
            .and_then(|v| v.as_str())
            .unwrap_or("good")
        {
            "bad" => Quality::Bad,
            "stale" => Quality::Stale,
            _ => Quality::Good,
        }
    }

    #[test]
    fn shared_vectors() {
        let doc: serde_json::Value = serde_json::from_str(VECTORS).unwrap();
        let vectors = doc["vectors"].as_array().unwrap();
        assert!(!vectors.is_empty());
        for vector in vectors {
            let name = vector["name"].as_str().unwrap();
            let table = vector["policy"].as_object().unwrap();
            let toml_table: toml::Value = toml::Value::try_from(table).unwrap();
            check_values(&toml_table).unwrap_or_else(|e| panic!("{name}: invalid policy: {e}"));
            let pushed = vector
                .get("pushed")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let mut state: ReportState<String> =
                ReportState::new(ReportPolicy::from_table(table), Duration::ZERO);
            for (index, step) in vector["steps"].as_array().unwrap().iter().enumerate() {
                let now = Duration::from_millis(step["at"].as_u64().unwrap());
                let expected: Vec<String> = step
                    .get("publish")
                    .and_then(|p| p.as_array())
                    .map(|p| {
                        p.iter()
                            .map(|id| id.as_str().unwrap().to_string())
                            .collect()
                    })
                    .unwrap_or_default();
                let mut published = Vec::new();
                let mut read = false;
                if let Some(sample) = step.get("sample") {
                    let id = sample["id"].as_str().unwrap().to_string();
                    match sample.get("src").and_then(|v| v.as_u64()) {
                        Some(src) => published.extend(state.offer_replayed(
                            id,
                            obs_of(sample),
                            quality_of(sample),
                            Duration::from_millis(src),
                            now,
                        )),
                        None => published.extend(state.offer(id, obs_of(sample), quality_of(sample), now)),
                    }
                } else if step.get("tick").is_some() {
                    let due = state.due(now, pushed);
                    published.extend(due.publish);
                    read = due.read;
                } else if step.get("no_data").is_some() {
                    state.no_data();
                } else if step.get("reset").is_some() {
                    state.reset(now);
                } else {
                    panic!("{name}: step {index} has no kind");
                }
                assert_eq!(
                    published, expected,
                    "{name}: step {index} (at {now:?}) published"
                );
                let expected_read = step.get("read").and_then(|r| r.as_bool()).unwrap_or(false);
                assert_eq!(
                    read, expected_read,
                    "{name}: step {index} (at {now:?}) read"
                );
            }
        }
    }

    #[test]
    fn table_validation() {
        let check = |text: &str| check_values(&toml::from_str::<toml::Value>(text).unwrap());
        assert!(check(
            "on_change = true\ndeadband = 0.5\nmin_interval = \"10s\"\nmax_interval = \"1m\""
        )
        .is_ok());
        assert!(check("deadband = \"2%\"").is_ok());
        assert!(check("deadband = \"2.5%\"").is_ok());
        assert!(check("max_interval = \"0\"\nmin_interval = \"1h\"").is_ok());
        assert_eq!(
            check("min_interval = \"10s\"\nmax_interval = \"5s\"").unwrap_err(),
            "report.max_interval must be longer than report.min_interval"
        );
        assert!(check("deadband = \"-1%\"")
            .unwrap_err()
            .contains("report.deadband"));
        assert!(check("deadband = -1")
            .unwrap_err()
            .contains("report.deadband"));
        assert!(check("deadband = \"2\"")
            .unwrap_err()
            .contains("report.deadband"));
        assert!(check("debounce = \"soon\"")
            .unwrap_err()
            .contains("report.debounce"));
        assert!(check("on_change = \"yes\"")
            .unwrap_err()
            .contains("report.on_change"));
        assert!(check("debounce = \"0\"").is_ok());
    }

    #[test]
    fn inherited_heartbeat_under_the_rate_limit_is_raised() {
        let merged = serde_json::json!({ "min_interval": "1h", "max_interval": "30m" });
        let (policy, warning) = effective(merged.as_object().unwrap());
        assert_eq!(policy.max_interval, Some(Duration::from_secs(7200)));
        assert!(warning.is_some());
        let fine = serde_json::json!({ "min_interval": "10s", "max_interval": "30m" });
        assert!(effective(fine.as_object().unwrap()).1.is_none());
    }

    #[test]
    fn empty_table_is_passthrough() {
        assert!(ReportPolicy::from_table(&serde_json::Map::new()).is_passthrough());
        let off = serde_json::json!({ "on_change": false, "deadband": 0, "max_interval": "0" });
        assert!(ReportPolicy::from_table(off.as_object().unwrap()).is_passthrough());
    }

    #[derive(Clone, Debug)]
    enum Event {
        Sample { value: i8, bad: bool },
        Tick,
    }

    fn events() -> impl Strategy<Value = Vec<(u16, Event)>> {
        prop::collection::vec(
            (
                0u16..5000,
                prop_oneof![
                    3 => (-3i8..3, prop::bool::weighted(0.1)).prop_map(|(value, bad)| Event::Sample { value, bad }),
                    1 => Just(Event::Tick),
                ],
            ),
            1..60,
        )
    }

    proptest! {
        // A quality change is published at once, and nothing held from before it follows it.
        #[test]
        fn quality_changes_are_never_overtaken(
            events in events(),
            on_change in any::<bool>(),
            min in prop::option::of(1u64..8000),
            debounce in prop::option::of(1u64..3000),
        ) {
            let policy = ReportPolicy {
                on_change,
                deadband: None,
                min_interval: min.map(Duration::from_millis),
                max_interval: None,
                debounce: debounce.map(Duration::from_millis),
            };
            let mut state: ReportState<(usize, Quality)> = ReportState::new(policy, Duration::ZERO);
            let mut now = Duration::ZERO;
            let mut published: Vec<(usize, Quality)> = Vec::new();
            let mut last_offered_quality = None;
            for (index, (step, event)) in events.into_iter().enumerate() {
                now += Duration::from_millis(step as u64);
                match event {
                    Event::Sample { value, bad } => {
                        let quality = if bad { Quality::Bad } else { Quality::Good };
                        let out = state.offer((index, quality), Obs::Number(value as f64), quality, now);
                        if last_offered_quality != Some(quality) {
                            prop_assert_eq!(out, Some((index, quality)), "a quality change is published at once");
                        }
                        last_offered_quality = Some(quality);
                        published.extend(out);
                    }
                    Event::Tick => published.extend(state.due(now, false).publish),
                }
            }
            // Published in offer order, and every publish after a quality change has that quality.
            prop_assert!(published.windows(2).all(|w| w[0].0 < w[1].0));
        }

        // Without a change filter, once readings stop and the interval ends, the last reading
        // is published: min_interval delays, it never loses the final state.
        #[test]
        fn the_last_reading_is_never_lost(values in prop::collection::vec((0u16..3000, -3i8..3), 1..40), min in 1u64..8000) {
            let policy = ReportPolicy { min_interval: Some(Duration::from_millis(min)), on_change: true, ..Default::default() };
            let mut state: ReportState<i8> = ReportState::new(policy, Duration::ZERO);
            let mut now = Duration::ZERO;
            let mut last_published = None;
            for (step, value) in &values {
                now += Duration::from_millis(*step as u64);
                if let Some(v) = state.offer(*value, Obs::Number(*value as f64), Quality::Good, now) {
                    last_published = Some(v);
                }
                if let Some(v) = state.due(now, false).publish {
                    last_published = Some(v);
                }
            }
            now += Duration::from_millis(min);
            if let Some(v) = state.due(now, false).publish {
                last_published = Some(v);
            }
            prop_assert_eq!(last_published, Some(values.last().unwrap().1));
        }

        // A replay arrives in a burst, but its intervals are measured on source time: it
        // publishes exactly what the live state would publish on a clock that runs on source
        // time and ticks as each reading arrives. The last reading still arrives once the replay
        // hands over to the runtime clock.
        #[test]
        fn a_replay_matches_live_on_source_time(
            readings in prop::collection::vec((1u64..5000, -3i8..3), 1..200),
            min in prop::option::of(1u64..20_000),
            debounce in prop::option::of(1u64..20_000),
            on_change in any::<bool>(),
        ) {
            let policy = ReportPolicy {
                min_interval: min.map(Duration::from_millis),
                debounce: debounce.map(Duration::from_millis),
                on_change,
                ..Default::default()
            };
            let mut replay: ReportState<usize> = ReportState::new(policy.clone(), Duration::ZERO);
            let mut live: ReportState<usize> = ReportState::new(policy, Duration::ZERO);
            let mut src = Duration::from_secs(1_700_000_000);
            let (mut from_replay, mut from_live) = (Vec::new(), Vec::new());
            for (i, (gap, value)) in readings.iter().enumerate() {
                src += Duration::from_millis(*gap);
                let obs = Obs::Number(*value as f64);
                from_replay.extend(replay.offer_replayed(i, obs.clone(), Quality::Good, src, Duration::ZERO));
                from_live.extend(live.due(src, true).publish);
                from_live.extend(live.offer(i, obs, Quality::Good, src));
            }
            prop_assert_eq!(&from_replay, &from_live);

            // After the hand-over, whatever is still held comes out on the runtime clock.
            let mut now = REPLAY_IDLE;
            let wait = Duration::from_millis(min.unwrap_or(0) + debounce.unwrap_or(0));
            from_replay.extend(replay.due(now, true).publish);
            now += wait;
            from_replay.extend(replay.due(now, true).publish);
            now += wait;
            from_replay.extend(replay.due(now, true).publish);
            src += wait * 2;
            from_live.extend(live.due(src, true).publish);
            from_live.extend(live.due(src + wait, true).publish);
            prop_assert_eq!(from_replay.last(), from_live.last());
        }
    }
}
