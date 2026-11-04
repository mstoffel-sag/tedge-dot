//! Delivery cursors (spec §5.4): per pushed point, the PI timestamp of the last event handed to
//! the runtime and the values delivered at that timestamp, kept across restarts in
//! `<state_dir>/<service_name>/<device>.json`.

use serde::{Deserialize, Serialize};
use serde_json::Value as Json;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};
use time::format_description::well_known::Rfc3339;
use time::OffsetDateTime;
use tracing::warn;

const VERSION: u32 = 1;

/// A PI timestamp as text (kept verbatim for `startTime`) and as an instant (for comparison).
#[derive(Clone, Debug)]
pub struct Stamp {
    pub text: String,
    pub at: OffsetDateTime,
}

impl Stamp {
    pub fn parse(text: &str) -> Option<Stamp> {
        Some(Stamp { text: text.to_string(), at: OffsetDateTime::parse(text, &Rfc3339).ok()? })
    }

    pub fn of(at: OffsetDateTime) -> Stamp {
        Stamp { text: at.format(&Rfc3339).expect("an instant formats"), at }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
struct Entry {
    web_id: String,
    ts: String,
    /// The PI values delivered at `ts`: PI can archive several events at one timestamp.
    values: Vec<Json>,
}

#[derive(Debug, Serialize, Deserialize)]
struct File {
    version: u32,
    points: HashMap<String, Entry>,
}

/// One point's position.
#[derive(Clone, Debug)]
pub struct Cursor {
    pub ts: Stamp,
    pub values: Vec<Json>,
}

impl Cursor {
    /// Whether an event at `at` with `value` comes after this cursor (spec §5.4 dedup): later,
    /// or at the same time with a value not yet delivered at that time.
    pub fn admits(&self, at: OffsetDateTime, value: &Json) -> bool {
        at > self.ts.at || (at == self.ts.at && !self.values.contains(value))
    }
}

/// A device's cursors and their file.
#[derive(Debug)]
pub struct Store {
    path: PathBuf,
    points: HashMap<String, (String, Cursor)>,
    dirty: bool,
    last_flush: Instant,
    every: Duration,
}

impl Store {
    /// Load the device's file, keeping the cursors of points still configured with the same
    /// WebId (`expected`: point id → WebId). A missing file means no cursors; an unreadable one
    /// is reported and replaced on the next flush.
    pub fn load(path: PathBuf, expected: &HashMap<String, String>, every: Duration) -> Store {
        let mut points = HashMap::new();
        match std::fs::read(&path) {
            Ok(bytes) => match serde_json::from_slice::<File>(&bytes) {
                Ok(file) if file.version == VERSION => {
                    for (id, e) in file.points {
                        let Some(ts) = Stamp::parse(&e.ts) else { continue };
                        if expected.get(&id) == Some(&e.web_id) {
                            points.insert(id, (e.web_id, Cursor { ts, values: e.values }));
                        }
                    }
                }
                Ok(file) => warn!(path = %path.display(), "cursor file version {} is not {VERSION}; starting live", file.version),
                Err(e) => warn!(path = %path.display(), "cursor file is unreadable ({e}); starting live"),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => warn!(path = %path.display(), "cursor file is unreadable ({e}); starting live"),
        }
        // A point no longer configured (or re-addressed) is dropped on the next write.
        let dirty = points.len() != expected.len();
        Store { path, points, dirty, last_flush: Instant::now(), every }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn get(&self, point: &str) -> Option<&Cursor> {
        self.points.get(point).map(|(_, c)| c)
    }

    /// An event was handed to the runtime.
    pub fn advance(&mut self, point: &str, web_id: &str, ts: &Stamp, value: &Json) {
        match self.points.get_mut(point) {
            Some((_, c)) if c.ts.at == ts.at => {
                if !c.values.contains(value) {
                    c.values.push(value.clone());
                }
            }
            Some((_, c)) if c.ts.at > ts.at => {}
            _ => {
                self.points.insert(
                    point.to_string(),
                    (web_id.to_string(), Cursor { ts: ts.clone(), values: vec![value.clone()] }),
                );
            }
        }
        self.dirty = true;
    }

    /// Write the file when it changed and `cursor_flush` has passed.
    pub fn maybe_flush(&mut self) {
        if self.dirty && self.last_flush.elapsed() >= self.every {
            self.flush();
        }
    }

    /// Write the file now, by atomic rename.
    pub fn flush(&mut self) {
        if !self.dirty {
            return;
        }
        let file = File {
            version: VERSION,
            points: self
                .points
                .iter()
                .map(|(id, (web_id, c))| {
                    (id.clone(), Entry { web_id: web_id.clone(), ts: c.ts.text.clone(), values: c.values.clone() })
                })
                .collect(),
        };
        let write = || -> std::io::Result<()> {
            if let Some(dir) = self.path.parent() {
                std::fs::create_dir_all(dir)?;
            }
            let tmp = self.path.with_extension("json.tmp");
            std::fs::write(&tmp, serde_json::to_vec(&file).expect("cursors serialize"))?;
            std::fs::rename(&tmp, &self.path)
        };
        match write() {
            Ok(()) => self.dirty = false,
            Err(e) => warn!(path = %self.path.display(), "cannot write the cursor file: {e}"),
        }
        self.last_flush = Instant::now();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn expected(pairs: &[(&str, &str)]) -> HashMap<String, String> {
        pairs.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect()
    }

    #[test]
    fn dedup_keeps_several_events_at_one_time() {
        let t = Stamp::parse("2026-11-02T08:00:00.1234567Z").unwrap();
        let mut c = Cursor { ts: t.clone(), values: vec![json!(1)] };
        assert!(!c.admits(t.at, &json!(1)));
        assert!(c.admits(t.at, &json!(2)));
        c.values.push(json!(2));
        assert!(!c.admits(t.at, &json!(2)));
        assert!(c.admits(Stamp::parse("2026-11-02T08:00:00.1234568Z").unwrap().at, &json!(1)));
        assert!(!c.admits(Stamp::parse("2026-11-02T07:59:59Z").unwrap().at, &json!(9)));
    }

    #[test]
    fn round_trip_and_web_id_check() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("svc").join("pi.json");
        let mut s = Store::load(path.clone(), &expected(&[("a", "W1"), ("b", "W2")]), Duration::ZERO);
        let t = Stamp::parse("2026-11-02T08:00:00Z").unwrap();
        s.advance("a", "W1", &t, &json!(1.5));
        s.advance("b", "W2", &t, &json!("x"));
        s.flush();
        let s = Store::load(path.clone(), &expected(&[("a", "W1"), ("b", "W-changed")]), Duration::ZERO);
        assert_eq!(s.get("a").unwrap().values, vec![json!(1.5)]);
        assert!(s.get("b").is_none(), "a re-addressed point starts live");
    }

    #[test]
    fn corrupt_file_starts_live_and_is_replaced() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("pi.json");
        std::fs::write(&path, b"{not json").unwrap();
        let mut s = Store::load(path.clone(), &expected(&[("a", "W1")]), Duration::ZERO);
        assert!(s.get("a").is_none());
        s.advance("a", "W1", &Stamp::parse("2026-11-02T08:00:00Z").unwrap(), &json!(1));
        s.maybe_flush();
        let back: serde_json::Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(back["version"], 1);
        assert_eq!(back["points"]["a"]["web_id"], "W1");
    }

    #[test]
    fn an_older_event_does_not_move_the_cursor_back() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = Store::load(dir.path().join("p.json"), &expected(&[("a", "W1")]), Duration::from_secs(3600));
        s.advance("a", "W1", &Stamp::parse("2026-11-02T08:00:00Z").unwrap(), &json!(1));
        s.advance("a", "W1", &Stamp::parse("2026-11-02T07:00:00Z").unwrap(), &json!(2));
        assert_eq!(s.get("a").unwrap().ts.text, "2026-11-02T08:00:00Z");
    }
}
