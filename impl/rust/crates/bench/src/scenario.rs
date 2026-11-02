//! Scenarios (`bench/scenarios.toml`) and the connector configurations generated for them.
//!
//! A scenario fixes the connector setup; each of its rates (and each value of a varied key such
//! as `instances`) becomes one run. For a polled scenario either `poll_interval` is fixed and
//! the points per device follow from the rate, or `points` is fixed and the poll interval
//! follows. A pushed scenario fixes `points` and spreads the rate over its devices.

use serde::{Deserialize, Serialize};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::time::Duration;

#[derive(Clone, Debug, Deserialize)]
#[serde(untagged)]
pub enum OneOrMany<T> {
    One(T),
    Many(Vec<T>),
}

impl<T: Clone> OneOrMany<T> {
    fn values(&self) -> Vec<T> {
        match self {
            OneOrMany::One(v) => vec![v.clone()],
            OneOrMany::Many(v) => v.clone(),
        }
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Scenario {
    pub name: String,
    #[serde(default)]
    pub description: String,
    /// `bench` or `opcua`.
    #[serde(default = "default_module")]
    pub module: String,
    /// Install the `ot-*` flows in the mapper.
    #[serde(default)]
    pub flows: bool,
    #[serde(default)]
    pub push: bool,
    pub devices: usize,
    /// Points per device.
    pub points: Option<usize>,
    pub poll_interval: Option<String>,
    pub sampling_interval: Option<String>,
    pub latency: Option<String>,
    pub batch: Option<usize>,
    pub change: Option<f64>,
    pub ts_jitter: Option<String>,
    pub ts_resolution: Option<OneOrMany<String>>,
    pub publish_interval: Option<String>,
    pub instances: Option<OneOrMany<usize>>,
    /// `"measurements"` for the runtime's measurement output (contract §5.5); samples otherwise.
    pub output: Option<String>,
    /// Grouping policies to run, one run each: `"time"`, `"frame:<window>"` or
    /// `"sample:<every>"`. Measurement output only.
    pub group_by: Option<OneOrMany<String>>,
    /// Share of each device's points that declare an alarm (`meta.alarm`), so they still
    /// publish samples in measurement output.
    pub alarm_share: Option<f64>,
    pub rates: Vec<f64>,
    pub window: Option<String>,
    pub warmup: Option<String>,
    /// Implementations to run; both unless restricted.
    pub impls: Option<Vec<String>>,
    /// Why `impls` is restricted (listed in the baseline).
    pub impls_reason: Option<String>,
}

fn default_module() -> String {
    "bench".into()
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ScenarioFile {
    #[serde(rename = "scenario")]
    pub scenarios: Vec<Scenario>,
}

pub fn load(path: &Path) -> Result<ScenarioFile, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    toml::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))
}

fn duration(s: &str, what: &str) -> Result<Duration, String> {
    let s = s.trim();
    let (num, unit) = s.split_at(s.find(|c: char| !(c.is_ascii_digit() || c == '.')).unwrap_or(s.len()));
    let n: f64 = num.parse().map_err(|_| format!("{what}: '{s}' is not a duration"))?;
    let secs = match unit {
        "ms" => n / 1000.0,
        "" | "s" => n,
        "m" => n * 60.0,
        _ => return Err(format!("{what}: '{s}' is not a duration")),
    };
    Ok(Duration::from_secs_f64(secs))
}

/// One run of a scenario: everything needed to generate configs and judge the result.
#[derive(Clone, Debug, Serialize)]
pub struct Run {
    pub scenario: String,
    pub module: String,
    #[serde(rename = "impl")]
    pub implementation: String,
    pub flows: bool,
    pub push: bool,
    pub target_rate: f64,
    pub devices: usize,
    pub points_per_device: usize,
    pub instances: usize,
    pub poll_interval_s: f64,
    pub sampling_interval_s: Option<f64>,
    pub push_rate_per_device: Option<f64>,
    pub latency: Option<String>,
    pub batch: Option<usize>,
    pub change: Option<f64>,
    pub ts_jitter: Option<String>,
    pub ts_resolution: Option<String>,
    pub publish_interval: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub group_by: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub alarm_share: Option<f64>,
    pub window_s: f64,
    pub warmup_s: f64,
}

impl Run {
    /// Whether readings arrive as measurements rather than samples.
    pub fn measurements(&self) -> bool {
        self.output.as_deref() == Some("measurements")
    }

    /// Readings per second the generated config produces (rounding of points can make it
    /// differ slightly from the target).
    pub fn rate(&self) -> f64 {
        match self.push_rate_per_device {
            Some(r) => r * self.devices as f64,
            None => (self.devices * self.points_per_device) as f64 / self.poll_interval_s,
        }
    }

    /// The boundary interval left out at each end of the window (spec: "excluding the first
    /// and last poll interval"). Pushed readings carry spread timestamps, so one second.
    pub fn margin_s(&self) -> f64 {
        if self.push {
            1.0
        } else {
            self.poll_interval_s
        }
    }

    pub fn device_names(&self) -> Vec<String> {
        (0..self.devices).map(|d| format!("plc{d}")).collect()
    }

    /// A short label for file names and logs.
    pub fn label(&self) -> String {
        let mut l = format!("{}-{}-{}", self.scenario, self.implementation, self.target_rate as u64);
        if self.instances > 1 {
            let _ = write!(l, "-x{}", self.instances);
        }
        if let Some(r) = &self.ts_resolution {
            let _ = write!(l, "-res{r}");
        }
        if let Some(g) = &self.group_by {
            let _ = write!(l, "-{}", g.replace(':', "-"));
        }
        l
    }
}

pub struct Overrides {
    pub rate: Option<f64>,
    pub window: Option<Duration>,
    pub implementation: Option<String>,
}

/// Every run of `s`, for each implementation, rate and varied value.
pub fn expand(s: &Scenario, o: &Overrides) -> Result<Vec<Run>, String> {
    let impls = s.impls.clone().unwrap_or_else(|| vec!["rust".into(), "c".into()]);
    let impls: Vec<String> = match &o.implementation {
        Some(i) if impls.contains(i) => vec![i.clone()],
        Some(i) => {
            return Err(format!(
                "scenario {} does not run with IMPL={i}{}",
                s.name,
                s.impls_reason.as_deref().map(|r| format!(": {r}")).unwrap_or_default()
            ))
        }
        None => impls,
    };
    let rates = o.rate.map(|r| vec![r]).unwrap_or_else(|| s.rates.clone());
    let instances = s.instances.as_ref().map(OneOrMany::values).unwrap_or_else(|| vec![1]);
    let resolutions: Vec<Option<String>> = match &s.ts_resolution {
        Some(r) => r.values().into_iter().map(Some).collect(),
        None => vec![None],
    };
    let policies: Vec<Option<String>> = match &s.group_by {
        Some(g) => g.values().into_iter().map(Some).collect(),
        None => vec![None],
    };
    for policy in policies.iter().flatten() {
        batch_table(policy)?;
    }
    if s.group_by.is_some() && s.output.as_deref() != Some("measurements") {
        return Err(format!("scenario {}: group_by needs output = \"measurements\"", s.name));
    }
    let window = match o.window {
        Some(w) => w,
        None => duration(s.window.as_deref().unwrap_or("20s"), "window")?,
    };
    let warmup = duration(s.warmup.as_deref().unwrap_or("5s"), "warmup")?;
    let sampling = s.sampling_interval.as_deref().map(|v| duration(v, "sampling_interval")).transpose()?;
    if s.devices == 0 {
        return Err(format!("scenario {}: devices must be at least 1", s.name));
    }

    let mut runs = Vec::new();
    for implementation in &impls {
        for &rate in &rates {
            for &inst in &instances {
                for (res, policy) in resolutions.iter().flat_map(|r| policies.iter().map(move |p| (r, p))) {
                    let (points, poll_s, push_rate) = if s.push {
                        let p = s.points.ok_or_else(|| format!("scenario {}: a pushed scenario needs points", s.name))?;
                        (p, 3600.0, Some(rate / s.devices as f64))
                    } else {
                        match (&s.poll_interval, s.points) {
                            (Some(pi), None) => {
                                let poll = duration(pi, "poll_interval")?.as_secs_f64();
                                let total = (rate * poll).round() as usize;
                                (total.div_ceil(s.devices).max(1), poll, None)
                            }
                            (None, Some(p)) => (p, (s.devices * p) as f64 / rate, None),
                            _ => {
                                return Err(format!(
                                    "scenario {}: set exactly one of poll_interval and points",
                                    s.name
                                ))
                            }
                        }
                    };
                    runs.push(Run {
                        scenario: s.name.clone(),
                        module: s.module.clone(),
                        implementation: implementation.clone(),
                        flows: s.flows,
                        push: s.push,
                        target_rate: rate,
                        devices: s.devices,
                        points_per_device: points,
                        instances: inst,
                        poll_interval_s: poll_s,
                        sampling_interval_s: sampling.map(|d| d.as_secs_f64()).or(s.push.then_some(0.2)),
                        push_rate_per_device: push_rate,
                        latency: s.latency.clone(),
                        batch: s.batch,
                        change: s.change,
                        ts_jitter: s.ts_jitter.clone(),
                        ts_resolution: res.clone(),
                        publish_interval: s.publish_interval.clone(),
                        output: s.output.clone(),
                        group_by: policy.clone(),
                        alarm_share: s.alarm_share,
                        window_s: window.as_secs_f64(),
                        warmup_s: warmup.as_secs_f64().max(if s.push { 1.0 } else { poll_s }),
                    });
                }
            }
        }
    }
    Ok(runs)
}

/// The `[connector] batch` table of a scenario's `group_by` value.
fn batch_table(policy: &str) -> Result<String, String> {
    match policy.split_once(':') {
        None if policy == "time" => Ok("{ group_by = \"time\" }".into()),
        Some(("frame", w)) => Ok(format!("{{ group_by = \"frame\", window = \"{w}\" }}")),
        Some(("sample", e)) => Ok(format!("{{ group_by = \"sample\", every = \"{e}\" }}")),
        _ => Err(format!("group_by '{policy}': use \"time\", \"frame:<window>\" or \"sample:<every>\"")),
    }
}

fn fmt_duration(secs: f64) -> String {
    format!("{}ms", (secs * 1000.0).round() as u64)
}

/// Write one connector config per instance into `dir`; devices are dealt round robin.
/// `endpoint` is the OPC UA simulator's, for `module = "opcua"`.
pub fn write_configs(run: &Run, dir: &Path, broker_port: u16, endpoint: Option<&str>) -> Result<Vec<PathBuf>, String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let mut files = Vec::new();
    for i in 0..run.instances {
        let mut t = String::new();
        let _ = writeln!(t, "[connector]\nprotocol = \"{}\"\nservice_name = \"tedge-dot-bench-{i}\"", run.module);
        let _ = writeln!(t, "poll_interval = \"{}\"\nlog_level = \"warn\"", fmt_duration(run.poll_interval_s));
        if let Some(s) = run.sampling_interval_s {
            let _ = writeln!(t, "sampling_interval = \"{}\"", fmt_duration(s));
        }
        if let Some(o) = &run.output {
            let _ = writeln!(t, "output = \"{o}\"");
        }
        if let Some(g) = &run.group_by {
            let _ = writeln!(t, "batch = {}", batch_table(g)?);
        }
        let _ = writeln!(t, "\n[mqtt]\nhost = \"127.0.0.1\"\nport = {broker_port}\n");
        if run.module == "opcua" {
            let _ = writeln!(t, "[connection]\napplication_name = \"tedge-dot-bench-{i}\"\nsecurity_policy = \"None\"\n");
        }
        for d in (i..run.devices).step_by(run.instances) {
            let _ = writeln!(t, "[[device]]\nname = \"plc{d}\"");
            if run.module == "opcua" {
                let ep = endpoint.ok_or("an OPC UA scenario needs the simulator's endpoint")?;
                let _ = writeln!(t, "protocol_address = {{ endpoint = \"{ep}\" }}");
            } else {
                let mut addr = Vec::new();
                if let Some(r) = run.push_rate_per_device {
                    addr.push(format!("push_rate = {r}"));
                }
                if let Some(l) = &run.latency {
                    addr.push(format!("latency = \"{l}\""));
                }
                if let Some(b) = run.batch {
                    addr.push(format!("batch = {b}"));
                }
                for (k, v) in [("ts_jitter", &run.ts_jitter), ("ts_resolution", &run.ts_resolution), ("publish_interval", &run.publish_interval)] {
                    if let Some(v) = v {
                        addr.push(format!("{k} = \"{v}\""));
                    }
                }
                let _ = writeln!(t, "protocol_address = {{ {} }}", addr.join(", "));
            }
            for p in 0..run.points_per_device {
                if run.module == "opcua" {
                    let node = d * run.points_per_device + p;
                    let _ = writeln!(
                        t,
                        "[[device.point]]\nid = \"p{p}\"\ndatatype = \"float64\"\nsubscribe = {}\naddress = {{ node_id = \"ns=2;s=Bulk{node}\" }}",
                        run.push
                    );
                } else {
                    let addr = run.change.map(|c| format!("change = {c}")).unwrap_or_default();
                    let _ = writeln!(t, "[[device.point]]\nid = \"p{p}\"\ndatatype = \"float64\"\naddress = {{ {addr} }}");
                    // Every n-th point declares an alarm that never raises (the walk stays far
                    // below it), so its samples go through the flows without alarm traffic.
                    if let Some(share) = run.alarm_share.filter(|s| *s > 0.0) {
                        let every = (1.0 / share).round().max(1.0) as usize;
                        if p % every == 0 {
                            let _ = writeln!(t, "meta = {{ alarm = {{ type = \"p{p}_high\", when = {{ above = 1e12 }} }} }}");
                        }
                    }
                }
            }
            t.push('\n');
        }
        let path = dir.join(format!("connector-{i}.toml"));
        std::fs::write(&path, t).map_err(|e| format!("{}: {e}", path.display()))?;
        files.push(path);
    }
    Ok(files)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scenario(toml: &str) -> Scenario {
        toml::from_str::<ScenarioFile>(toml).unwrap().scenarios.remove(0)
    }

    const NONE: Overrides = Overrides { rate: None, window: None, implementation: None };

    #[test]
    fn polled_points_follow_the_rate() {
        let s = scenario("[[scenario]]\nname = \"s\"\ndevices = 100\npoll_interval = \"1s\"\nrates = [10000, 100000]\n");
        let runs = expand(&s, &NONE).unwrap();
        assert_eq!(runs.len(), 4, "2 rates x 2 impls");
        assert_eq!(runs[0].points_per_device, 100);
        assert_eq!(runs[1].points_per_device, 1000);
        assert_eq!(runs[1].rate(), 100000.0);
    }

    #[test]
    fn fixed_points_set_the_poll_interval() {
        let s = scenario("[[scenario]]\nname = \"o\"\nmodule = \"opcua\"\ndevices = 1\npoints = 20000\nrates = [20000, 100000]\n");
        let runs = expand(&s, &Overrides { implementation: Some("rust".into()), ..NONE }).unwrap();
        assert_eq!(runs[0].poll_interval_s, 1.0);
        assert!((runs[1].poll_interval_s - 0.2).abs() < 1e-9);
    }

    #[test]
    fn variants_and_restrictions() {
        let s = scenario(
            "[[scenario]]\nname = \"j\"\npush = true\ndevices = 10\npoints = 100\nts_resolution = [\"1ms\", \"10ms\"]\n\
             instances = [1, 4]\nrates = [1000]\nimpls = [\"rust\"]\nimpls_reason = \"no source times in C\"\n",
        );
        let runs = expand(&s, &NONE).unwrap();
        assert_eq!(runs.len(), 4);
        assert!(runs.iter().all(|r| r.implementation == "rust"));
        assert_eq!(runs[0].push_rate_per_device, Some(100.0));
        let err = expand(&s, &Overrides { implementation: Some("c".into()), ..NONE }).unwrap_err();
        assert!(err.contains("no source times in C"), "{err}");
    }

    #[test]
    fn configs_split_devices_over_instances() {
        let s = scenario("[[scenario]]\nname = \"s\"\ndevices = 5\npoll_interval = \"1s\"\ninstances = 2\nrates = [50]\n");
        let run = expand(&s, &Overrides { implementation: Some("rust".into()), ..NONE }).unwrap().remove(0);
        let dir = std::env::temp_dir().join(format!("tedge-dot-bench-gen-{}", std::process::id()));
        let files = write_configs(&run, &dir, 1883, None).unwrap();
        assert_eq!(files.len(), 2);
        let a = std::fs::read_to_string(&files[0]).unwrap();
        let b = std::fs::read_to_string(&files[1]).unwrap();
        assert_eq!(a.matches("[[device]]").count(), 3);
        assert_eq!(b.matches("[[device]]").count(), 2);
        assert_eq!(a.matches("[[device.point]]").count(), 30);
        assert!(a.contains("service_name = \"tedge-dot-bench-0\"") && b.contains("tedge-dot-bench-1"));
        let _ = std::fs::remove_dir_all(dir);
    }
}
