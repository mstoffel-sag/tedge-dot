//! Config file watching for the `run` service (openspec change config-file-watch-reload).
//!
//! The service reloads its connector configs on SIGHUP. This module notices, by polling file
//! metadata, when the files a reload would read have changed, so that a config written by anything
//! (thin-edge.io configuration management, a provisioning tool, an editor) is applied without the
//! signal. It only decides *when* to reload. The reload itself is the SIGHUP code path in `run`.
//!
//! Watched: every `*.toml` in a config directory named on the command line (so an added or removed
//! file is a change), every config file named on the command line, and the point-library files the
//! discovered configs reference, as `library::referenced_library_files` resolves them.
//!
//! A change is acted on once it has settled: the snapshot differs from the last one acted on, and
//! the next poll shows the same snapshot again. The snapshot that triggers a reload becomes the
//! baseline *before* the reload runs, so a file saved while connectors are still reloading differs
//! from it and gets a reload of its own.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use tedge_dot_sdk::{library, parse_duration};

/// Environment variable holding the poll interval ("2s", "500ms"; "0" turns watching off).
pub const INTERVAL_ENV: &str = "TEDGE_DOT_CONFIG_WATCH_INTERVAL";
pub const DEFAULT_INTERVAL: Duration = Duration::from_secs(2);
/// The C build polls on its supervisor's 200 ms tick, so neither build polls faster.
pub const MIN_INTERVAL: Duration = Duration::from_millis(200);

/// The poll interval from the environment value `value`: `None` when watching is off. An invalid
/// value falls back to the default and a value below the minimum is raised to it; both come with
/// the warning to log.
pub fn interval_from(value: Option<&str>) -> (Option<Duration>, Option<String>) {
    let Some(raw) = value else {
        return (Some(DEFAULT_INTERVAL), None);
    };
    match parse_duration(raw) {
        Some(d) if d.is_zero() => (None, None),
        Some(d) if d < MIN_INTERVAL => (
            Some(MIN_INTERVAL),
            Some(format!(
                "{INTERVAL_ENV}={raw} is below the minimum; watching every {}ms",
                MIN_INTERVAL.as_millis()
            )),
        ),
        Some(d) => (Some(d), None),
        None => (
            Some(DEFAULT_INTERVAL),
            Some(format!(
                "{INTERVAL_ENV}={raw} is not a duration (e.g. \"2s\", \"500ms\", \"0\" to turn \
                 watching off); watching every {}s",
                DEFAULT_INTERVAL.as_secs()
            )),
        ),
    }
}

/// What identifies a version of a file: a rename-into-place gets a new inode even when size and
/// modification time happen to match.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Stamp {
    len: u64,
    modified: Option<SystemTime>,
    inode: u64,
}

/// Every watched path, with its stamp, or `None` for a path that does not exist (a library that
/// is referenced but missing, or a named config that was removed).
type Snapshot = BTreeMap<PathBuf, Option<Stamp>>;

fn stamp(path: &Path) -> Option<Stamp> {
    let meta = std::fs::metadata(path).ok()?;
    if !meta.is_file() {
        return None;
    }
    #[cfg(unix)]
    let inode = std::os::unix::fs::MetadataExt::ino(&meta);
    #[cfg(not(unix))]
    let inode = 0;
    Some(Stamp {
        len: meta.len(),
        modified: meta.modified().ok(),
        inode,
    })
}

pub struct Watcher {
    /// The config arguments of `run`: directories and files.
    args: Vec<String>,
    /// Library files referenced by the configs, as of the last rebuild.
    libraries: Vec<PathBuf>,
    /// The snapshot the last reload acted on (or the one taken at start).
    baseline: Snapshot,
    /// A snapshot that differed from the baseline at the previous poll, waiting to settle.
    pending: Option<Snapshot>,
}

impl Watcher {
    pub fn new(args: Vec<String>) -> Self {
        let mut watcher = Watcher {
            args,
            libraries: Vec::new(),
            baseline: Snapshot::new(),
            pending: None,
        };
        watcher.rebuild();
        watcher.baseline = watcher.snapshot();
        watcher
    }

    /// Re-read which library files the configs reference: called after each reload, since a
    /// reload may have applied a config that references different libraries. The baseline is
    /// left alone, so a newly watched file counts as added at the next poll (one reload that
    /// finds every config unchanged) rather than going unnoticed.
    pub fn rebuild(&mut self) {
        let mut libraries: Vec<PathBuf> = self
            .config_files()
            .iter()
            .flat_map(|config| library::referenced_library_files(config))
            .collect();
        libraries.sort();
        libraries.dedup();
        self.libraries = libraries;
    }

    /// One poll. Returns the changed paths when a settled change calls for a reload.
    pub fn poll(&mut self) -> Option<Vec<PathBuf>> {
        let now = self.snapshot();
        if now == self.baseline {
            self.pending = None;
            return None;
        }
        if self.pending.as_ref() != Some(&now) {
            // Changed since the last poll: wait for one more interval without a change.
            self.pending = Some(now);
            return None;
        }
        let changed = changed_paths(&self.baseline, &now);
        // The baseline moves to what this reload is about to read, before it reads it: a write
        // that lands while the reload runs then shows up as a difference at the next poll.
        self.baseline = now;
        self.pending = None;
        Some(changed)
    }

    /// The config files the arguments name now: each directory's `*.toml`, each named file.
    fn config_files(&self) -> Vec<PathBuf> {
        let mut files = Vec::new();
        for arg in &self.args {
            let path = Path::new(arg);
            if path.is_dir() {
                if let Ok(entries) = std::fs::read_dir(path) {
                    files.extend(
                        entries
                            .flatten()
                            .map(|entry| entry.path())
                            .filter(|p| p.is_file() && p.extension().is_some_and(|e| e == "toml")),
                    );
                }
            } else {
                files.push(path.to_path_buf());
            }
        }
        files
    }

    fn snapshot(&self) -> Snapshot {
        self.config_files()
            .into_iter()
            .chain(self.libraries.iter().cloned())
            .map(|path| {
                let stamp = stamp(&path);
                (path, stamp)
            })
            .collect()
    }
}

/// The paths that differ between two snapshots: added, removed or changed.
fn changed_paths(before: &Snapshot, after: &Snapshot) -> Vec<PathBuf> {
    let mut changed: Vec<PathBuf> = after
        .iter()
        .filter(|(path, stamp)| before.get(*path) != Some(*stamp))
        .map(|(path, _)| path.clone())
        .collect();
    changed.extend(
        before
            .keys()
            .filter(|path| !after.contains_key(*path))
            .cloned(),
    );
    changed.sort();
    changed
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    struct Dir(PathBuf);
    impl Dir {
        fn new(tag: &str) -> Self {
            let dir =
                std::env::temp_dir().join(format!("tedge-dot-watch-{tag}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            Dir(dir)
        }
        fn write(&self, rel: &str, text: &str) -> PathBuf {
            let path = self.0.join(rel);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent).unwrap();
            }
            // Through a temporary file and a rename, as tedge-write and persist_config do: the
            // new inode makes the change visible even within one mtime tick.
            let tmp = path.with_extension("tmp");
            fs::write(&tmp, text).unwrap();
            fs::rename(&tmp, &path).unwrap();
            path
        }
        fn arg(&self) -> Vec<String> {
            vec![self.0.display().to_string()]
        }
    }
    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    /// Poll until settled: two polls are needed after a change (see `poll`).
    fn settle(w: &mut Watcher) -> Option<Vec<PathBuf>> {
        w.poll().or_else(|| w.poll())
    }

    #[test]
    fn interval_from_env() {
        assert_eq!(interval_from(None), (Some(DEFAULT_INTERVAL), None));
        assert_eq!(
            interval_from(Some("500ms")).0,
            Some(Duration::from_millis(500))
        );
        assert_eq!(interval_from(Some("0")), (None, None));
        let (d, warning) = interval_from(Some("100ms"));
        assert_eq!(d, Some(MIN_INTERVAL));
        assert!(warning.unwrap().contains("below the minimum"));
        let (d, warning) = interval_from(Some("often"));
        assert_eq!(d, Some(DEFAULT_INTERVAL));
        assert!(warning.unwrap().contains("not a duration"));
    }

    #[test]
    fn nothing_changed_nothing_reloads() {
        let dir = Dir::new("idle");
        dir.write("a.toml", "x = 1");
        let mut w = Watcher::new(dir.arg());
        assert_eq!(w.poll(), None);
        assert_eq!(w.poll(), None);
    }

    #[test]
    fn added_changed_and_removed_configs_reload_once_settled() {
        let dir = Dir::new("acr");
        let a = dir.write("a.toml", "x = 1");
        let mut w = Watcher::new(dir.arg());

        let b = dir.write("b.toml", "y = 1");
        assert_eq!(w.poll(), None, "not yet settled");
        assert_eq!(w.poll(), Some(vec![b.clone()]));
        assert_eq!(w.poll(), None, "acted on once");

        dir.write("a.toml", "x = 2");
        assert_eq!(settle(&mut w), Some(vec![a.clone()]));

        fs::remove_file(&b).unwrap();
        assert_eq!(settle(&mut w), Some(vec![b]));
    }

    #[test]
    fn unrelated_files_are_ignored() {
        let dir = Dir::new("unrelated");
        dir.write("a.toml", "x = 1");
        let mut w = Watcher::new(dir.arg());
        dir.write("notes.txt", "hello");
        assert_eq!(settle(&mut w), None);
    }

    #[test]
    fn writes_close_together_reload_once() {
        let dir = Dir::new("burst");
        let a = dir.write("a.toml", "x = 1");
        let mut w = Watcher::new(dir.arg());
        dir.write("a.toml", "x = 2");
        assert_eq!(w.poll(), None);
        dir.write("a.toml", "x = 3");
        assert_eq!(w.poll(), None, "still changing: deferred");
        assert_eq!(w.poll(), Some(vec![a]));
        assert_eq!(w.poll(), None);
    }

    #[test]
    fn a_write_during_a_reload_gets_its_own_reload() {
        let dir = Dir::new("during");
        let a = dir.write("a.toml", "x = 1");
        let mut w = Watcher::new(dir.arg());
        dir.write("a.toml", "x = 2");
        assert_eq!(settle(&mut w), Some(vec![a.clone()]));
        // The reload runs now; the file is saved again before it finishes.
        dir.write("a.toml", "x = 3");
        w.rebuild();
        assert_eq!(settle(&mut w), Some(vec![a]));
    }

    #[test]
    fn referenced_libraries_are_watched_and_unreferenced_are_not() {
        let dir = Dir::new("libs");
        let libs = Dir::new("libs-search");
        let used = libs.write("modbus/used.toml", "[[point]]\nid = \"a\"\n");
        libs.write("modbus/unused.toml", "[[point]]\nid = \"b\"\n");
        dir.write(
            "plant.toml",
            &format!(
                "[connector]\nprotocol = \"modbus\"\npoint_library_path = [\"{}\"]\n\n\
                 [[device]]\nname = \"plc1\"\npoints_from = [\"used\"]\n",
                libs.0.display()
            ),
        );
        let mut w = Watcher::new(dir.arg());

        libs.write("modbus/unused.toml", "[[point]]\nid = \"c\"\n");
        assert_eq!(settle(&mut w), None, "a library no config references");

        libs.write("modbus/used.toml", "[[point]]\nid = \"d\"\n");
        assert_eq!(settle(&mut w), Some(vec![used]));
    }

    #[test]
    fn a_newly_referenced_library_costs_at_most_one_extra_reload() {
        let dir = Dir::new("newref");
        let libs = Dir::new("newref-search");
        let lib = libs.write("modbus/extra.toml", "[[point]]\nid = \"a\"\n");
        let config = |refs: &str| {
            format!(
                "[connector]\nprotocol = \"modbus\"\npoint_library_path = [\"{}\"]\n\n\
                 [[device]]\nname = \"plc1\"\npoints_from = [{refs}]\n",
                libs.0.display()
            )
        };
        let plant = dir.write("plant.toml", &config(""));
        let mut w = Watcher::new(dir.arg());

        dir.write("plant.toml", &config("\"extra\""));
        assert_eq!(settle(&mut w), Some(vec![plant]));
        w.rebuild(); // after the reload: the library is watched from now on
        assert_eq!(
            settle(&mut w),
            Some(vec![lib.clone()]),
            "counted as added, once"
        );
        assert_eq!(settle(&mut w), None);

        libs.write("modbus/extra.toml", "[[point]]\nid = \"b\"\n");
        assert_eq!(settle(&mut w), Some(vec![lib]));
    }

    #[test]
    fn a_missing_library_is_watched_where_it_would_be_found() {
        let dir = Dir::new("missing");
        let libs = Dir::new("missing-search");
        dir.write(
            "plant.toml",
            &format!(
                "[connector]\nprotocol = \"modbus\"\npoint_library_path = [\"{}\"]\n\n\
                 [[device]]\nname = \"plc1\"\npoints_from = [\"later\"]\n",
                libs.0.display()
            ),
        );
        let mut w = Watcher::new(dir.arg());
        let lib = libs.write("modbus/later.toml", "[[point]]\nid = \"a\"\n");
        assert_eq!(settle(&mut w), Some(vec![lib]));
    }
}
