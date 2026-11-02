//! `run`: measure one scenario run against the stack and print its result line.
//!
//! Order of a run: check the binary has the module, install or remove the flows, register the
//! devices, wait for the mapper to go idle (no backlog from a previous run), start the
//! connector instances, warm up, measure the window, stop the connectors, wait until the
//! output is quiet (`drain_s`), then compute and record the result.

use crate::host;
use crate::observe::{now_ms, Class, Counts, Observer};
use crate::procs::{self, Guard};
use crate::scenario::{self, Run};
use crate::stack::{self, Stack};
use serde::Serialize;
use std::collections::HashMap;
use std::io::BufRead;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

/// Flows a package installs into the c8y mapper (dist/config.yaml).
const FLOWS: [&str; 7] = [
    "ot-measurement",
    "ot-registration",
    "ot-alarm",
    "ot-event",
    "ot-parameter-state",
    "ot-command-forward",
    "ot-command-result",
];

pub struct Paths {
    pub repo: PathBuf,
    pub rust_bin: PathBuf,
    pub c_bin: PathBuf,
    pub conformance_bin: PathBuf,
    pub stack_dir: PathBuf,
    pub results_dir: PathBuf,
}

impl Paths {
    pub fn binary(&self, implementation: &str) -> &Path {
        if implementation == "c" {
            &self.c_bin
        } else {
            &self.rust_bin
        }
    }
}

/// The smoke thresholds (`--check`).
#[derive(Clone, Copy)]
pub struct Checks {
    pub max_missing_pct: f64,
    pub max_drain_s: f64,
}

#[derive(Serialize, Default)]
pub struct PerClass {
    pub samples: f64,
    pub measurements: f64,
    pub c8y: f64,
}

#[derive(Serialize, Default)]
pub struct PerProcess {
    pub connector: f64,
    pub broker: f64,
    pub mapper: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub observer: Option<f64>,
}

#[derive(Serialize)]
pub struct ResultLine {
    pub schema: u32,
    pub at: String,
    pub scenario: String,
    #[serde(rename = "impl")]
    pub implementation: String,
    pub params: Run,
    pub git: host::Git,
    pub host: host::Host,
    pub versions: host::Versions,
    pub target_rate: f64,
    pub configured_rate: f64,
    pub achieved_rate: f64,
    pub values_per_s: PerClass,
    pub msgs_per_s: PerClass,
    /// Values per thin-edge measurement message: how well the readings batch.
    pub values_per_message: f64,
    pub cpu_pct: PerProcess,
    pub us_per_reading: PerProcess,
    pub connector_rss_mb: f64,
    pub c8y_bytes_per_s: f64,
    pub te_errors: u64,
    pub drain_s: f64,
    /// Readings the scenario's rate expects in the span, and the ones taken in it.
    pub expected: u64,
    pub observed: u64,
    /// How far the readings fell short of the configured rate: a connector that polls late or
    /// can't keep up, not a lost message.
    pub rate_shortfall_pct: f64,
    /// Samples lost after the connector published them (gaps in each point's `seq`).
    pub missing: u64,
    pub missing_pct: f64,
    pub startup_s: f64,
    pub window_s: f64,
    pub span_s: f64,
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        if entry.file_type()?.is_file() {
            std::fs::copy(entry.path(), to.join(entry.file_name()))?;
        }
    }
    Ok(())
}

/// Install or remove the `ot-*` flows in the stack mapper's flows directory. Returns whether
/// anything changed, so the caller can give the mapper time to reload.
pub fn set_flows(repo: &Path, stack: &Stack, installed: bool) -> Result<bool, String> {
    let mut changed = false;
    for name in FLOWS {
        let dst = stack.flows_dir().join(name);
        if installed && !dst.exists() {
            copy_dir(&repo.join("flows").join(name), &dst).map_err(|e| format!("installing flow {name}: {e}"))?;
            changed = true;
        } else if !installed && dst.exists() {
            std::fs::remove_dir_all(&dst).map_err(|e| format!("removing flow {name}: {e}"))?;
            changed = true;
        }
    }
    Ok(changed)
}

/// Fails at once, naming the build to use, when `bin` cannot run a `bench` config. Never falls
/// back to the other implementation.
pub fn check_module(bin: &Path, implementation: &str, scratch: &Path) -> Result<(), String> {
    let build = if implementation == "c" {
        "the C build with -DTDOT_BENCH=ON (`just bench-build`)"
    } else {
        "the Rust build with --features bench (`just bench-build`)"
    };
    if !bin.exists() {
        return Err(format!("IMPL={implementation}: {} does not exist; build {build}", bin.display()));
    }
    std::fs::create_dir_all(scratch).map_err(|e| e.to_string())?;
    let cfg = scratch.join("module-check.toml");
    std::fs::write(
        &cfg,
        "[connector]\nprotocol = \"bench\"\n[[device]]\nname = \"check\"\nprotocol_address = {}\n\
         [[device.point]]\nid = \"p\"\ndatatype = \"float64\"\naddress = {}\n",
    )
    .map_err(|e| e.to_string())?;
    let out = Command::new(bin).arg("read").arg("-c").arg(&cfg).output().map_err(|e| format!("{}: {e}", bin.display()))?;
    if out.status.success() {
        return Ok(());
    }
    let why = String::from_utf8_lossy(&out.stderr).lines().last().unwrap_or("").to_string();
    Err(format!(
        "IMPL={implementation}: {} has no bench module ({why}); build {build}. The harness does not fall back to the other implementation.",
        bin.display()
    ))
}

/// Wait until the mapper used less than 50 ms of CPU over 2 s; fail after `limit`.
pub async fn wait_idle(pid: u32, limit: Duration) -> Result<f64, String> {
    let start = tokio::time::Instant::now();
    loop {
        let a = procs::cpu_seconds(pid).map_err(|e| format!("mapper CPU: {e}"))?;
        tokio::time::sleep(Duration::from_secs(2)).await;
        let b = procs::cpu_seconds(pid).map_err(|e| format!("mapper CPU: {e}"))?;
        if b - a < 0.05 {
            return Ok(start.elapsed().as_secs_f64());
        }
        if start.elapsed() >= limit {
            return Err(format!(
                "the mapper did not go idle within {} s (it used {:.0} ms of CPU in the last 2 s), \
                 probably a backlog from a previous run; retry once it is idle",
                limit.as_secs(),
                (b - a) * 1000.0
            ));
        }
    }
}

/// Start the standalone OPC UA simulator and return its endpoint.
fn start_opcua_sim(bin: &Path, points: usize, log: &Path, guard: &mut Guard) -> Result<String, String> {
    let mut child = Command::new(bin)
        .args(["serve", "opcua", "--points", &points.to_string(), "--port", "0"])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(std::fs::File::create(log).map_err(|e| e.to_string())?)
        .spawn()
        .map_err(|e| format!("cannot start {}: {e}", bin.display()))?;
    guard.0.push(child.id());
    let mut line = String::new();
    std::io::BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    let endpoint = line.trim().to_string();
    if !endpoint.starts_with("opc.tcp://") {
        return Err(format!("the OPC UA simulator did not start; see {}", log.display()));
    }
    Ok(endpoint)
}

/// The counting span for a polled run: from the first quiet point of the poll cycle after
/// `t0 + margin`, a whole number of poll intervals long, ending before `t1 - margin`. Edges in
/// the quiet part of the cycle never cut through a pass, so the expected count is exact.
fn polled_span(c: &Counts, t0: f64, t1: f64, margin_ms: f64, poll_ms: f64) -> (f64, f64, f64) {
    const BUCKET: f64 = 10.0;
    let bins = ((poll_ms / BUCKET).round() as usize).max(1);
    let mut hist = vec![0u64; bins];
    let mut t = t0;
    while t < t1 {
        let k = ((t / BUCKET) as i64).rem_euclid(bins as i64) as usize;
        hist[k] += c.values_between(t, t + BUCKET);
        t += BUCKET;
    }
    let quiet = hist.iter().enumerate().min_by_key(|(_, v)| **v).map(|(k, _)| k).unwrap_or(0) as f64 * BUCKET;
    let mut a = ((t0 + margin_ms) / poll_ms).floor() * poll_ms + quiet;
    while a < t0 + margin_ms {
        a += poll_ms;
    }
    let polls = ((t1 - margin_ms - a) / poll_ms).floor().max(0.0);
    (a, a + polls * poll_ms, polls)
}

struct Snapshot {
    at: f64,
    counts: HashMap<Class, Counts>,
    cpu: Vec<f64>,
}

fn snapshot(obs: &Observer, pids: &[u32]) -> Snapshot {
    Snapshot {
        at: now_ms(),
        counts: Class::ALL.iter().map(|c| (*c, obs.counts(*c))).collect(),
        cpu: pids.iter().map(|p| procs::cpu_seconds(*p).unwrap_or(0.0)).collect(),
    }
}

/// Measure one run. `stack` must be up.
pub async fn measure(paths: &Paths, stack: &Stack, run: &Run, checks: Option<Checks>) -> Result<ResultLine, String> {
    let bin = paths.binary(&run.implementation).to_path_buf();
    let run_dir = stack.dir.join("runs").join(run.label());
    let _ = std::fs::remove_dir_all(&run_dir);
    std::fs::create_dir_all(&run_dir).map_err(|e| e.to_string())?;
    check_module(&bin, &run.implementation, &run_dir)?;

    if set_flows(&paths.repo, stack, run.flows)? {
        tokio::time::sleep(Duration::from_secs(3)).await; // let the mapper reload its flows
    }
    stack::register(stack.broker_port, &run.device_names(), &run.module).await?;

    let mut sim = Guard::default();
    let endpoint = if run.module == "opcua" {
        Some(start_opcua_sim(&paths.conformance_bin, run.devices * run.points_per_device, &run_dir.join("opcua-sim.log"), &mut sim)?)
    } else {
        None
    };
    let configs = scenario::write_configs(run, &run_dir, stack.broker_port, endpoint.as_deref())?;

    let (broker_pid, mapper_pid) = stack.pids();
    wait_idle(mapper_pid, Duration::from_secs(60)).await?;
    let observer = Observer::start(stack.broker_port, &Class::ALL).await?;

    let mut connectors = Guard::default();
    let spawned_at = now_ms();
    for (i, cfg) in configs.iter().enumerate() {
        let child = procs::spawn_logged(
            Command::new(&bin).arg("run").arg("-c").arg(cfg).arg("--no-watch"),
            &run_dir.join(format!("connector-{i}.log")),
        )
        .map_err(|e| format!("cannot start {}: {e}", bin.display()))?;
        connectors.0.push(child.id());
    }
    // What carries the readings: samples, or in measurement output the measurements.
    let readings = if run.measurements() { Class::Measurements } else { Class::Samples };
    let first_by = tokio::time::Instant::now() + Duration::from_secs(180);
    while observer.counts(readings).msgs == 0 {
        if tokio::time::Instant::now() >= first_by {
            return Err(format!("no reading within 180 s of starting the connector; see {}", run_dir.display()));
        }
        if connectors.0.iter().any(|p| !procs::alive(*p)) {
            return Err(format!("a connector instance exited; see {}", run_dir.display()));
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let startup_s = (observer.counts(readings).first_ms - spawned_at) / 1000.0;
    tokio::time::sleep(Duration::from_secs_f64(run.warmup_s)).await;

    let me = std::process::id();
    let mut pids = connectors.0.clone();
    pids.extend([broker_pid, mapper_pid, me]);
    let s0 = snapshot(&observer, &pids);
    tokio::time::sleep(Duration::from_secs_f64(run.window_s)).await;
    let s1 = snapshot(&observer, &pids);
    let rss: u64 = connectors.0.iter().filter_map(|p| procs::rss_bytes(*p)).sum();
    connectors.stop_all();
    let last = observer.wait_quiet(&[Class::Samples, Class::Measurements, Class::C8y], Duration::from_secs(2), Duration::from_secs(600)).await;
    sim.stop_all();
    let drain_s = ((last - s1.at) / 1000.0).max(0.0);

    // Readings are counted by their own timestamp, inside the span.
    let samples = observer.counts(readings);
    let margin = run.margin_s() * 1000.0;
    let (a, b) = if run.push {
        (s0.at + margin, s1.at - margin)
    } else {
        let (a, b, _) = polled_span(&samples, s0.at, s1.at, margin, run.poll_interval_s * 1000.0);
        (a, b)
    };
    if b <= a {
        return Err(format!("the window ({} s) is too short for the {} s margins", run.window_s, run.margin_s()));
    }
    let span_s = (b - a) / 1000.0;
    let expected = (run.rate() * span_s).round() as u64;
    let observed = samples.values_between(a, b);
    // Samples carry a seq, so their gaps are the losses. Measurements carry none: a value that
    // did not reach Cumulocity is one the mapper lost or rejected.
    let (missing, received) = if run.measurements() {
        let c8y = observer.counts(Class::C8y).values_between(a, b);
        (observed.saturating_sub(c8y), observed.min(c8y))
    } else {
        observer.lost_samples()
    };

    let window_s = (s1.at - s0.at) / 1000.0;
    let in_window = samples.values_between(s0.at, s1.at).max(1) as f64;
    let n = connectors_count(&pids);
    let dcpu = |i: usize| s1.cpu[i] - s0.cpu[i];
    let connector_cpu: f64 = (0..n).map(dcpu).sum();
    let (broker_cpu, mapper_cpu, own_cpu) = (dcpu(n), dcpu(n + 1), dcpu(n + 2));
    let per_s = |class: Class| observer.counts(class).values_between(a, b) as f64 / span_s;
    let msgs = |class: Class| (s1.counts[&class].msgs - s0.counts[&class].msgs) as f64 / window_s;
    let errors = observer.counts(Class::Errors).msgs;

    let host = host::host();
    let line = ResultLine {
        schema: 1,
        at: time::OffsetDateTime::now_utc().format(&time::format_description::well_known::Rfc3339).unwrap_or_default(),
        scenario: run.scenario.clone(),
        implementation: run.implementation.clone(),
        params: run.clone(),
        git: host::git(&paths.repo),
        versions: host::versions(&bin),
        host,
        target_rate: run.target_rate,
        configured_rate: run.rate(),
        achieved_rate: observed as f64 / span_s,
        values_per_s: PerClass { samples: per_s(Class::Samples), measurements: per_s(Class::Measurements), c8y: per_s(Class::C8y) },
        msgs_per_s: PerClass { samples: msgs(Class::Samples), measurements: msgs(Class::Measurements), c8y: msgs(Class::C8y) },
        values_per_message: {
            let m = s1.counts[&Class::Measurements].msgs - s0.counts[&Class::Measurements].msgs;
            let v = s1.counts[&Class::Measurements].values - s0.counts[&Class::Measurements].values;
            if m > 0 { v as f64 / m as f64 } else { 0.0 }
        },
        cpu_pct: PerProcess {
            connector: 100.0 * connector_cpu / window_s,
            broker: 100.0 * broker_cpu / window_s,
            mapper: 100.0 * mapper_cpu / window_s,
            observer: Some(100.0 * own_cpu / window_s),
        },
        us_per_reading: PerProcess {
            connector: connector_cpu * 1e6 / in_window,
            broker: broker_cpu * 1e6 / in_window,
            mapper: mapper_cpu * 1e6 / in_window,
            observer: None,
        },
        connector_rss_mb: rss as f64 / 1e6,
        c8y_bytes_per_s: (s1.counts[&Class::C8y].bytes - s0.counts[&Class::C8y].bytes) as f64 / window_s,
        te_errors: errors,
        drain_s,
        expected,
        observed,
        rate_shortfall_pct: if expected > 0 { 100.0 * expected.saturating_sub(observed) as f64 / expected as f64 } else { 0.0 },
        missing,
        missing_pct: if missing + received > 0 { 100.0 * missing as f64 / (missing + received) as f64 } else { 0.0 },
        startup_s,
        window_s,
        span_s,
    };

    if let Some(c) = checks {
        let mut failed = Vec::new();
        if received == 0 {
            failed.push(if run.measurements() {
                "no measurement value reached Cumulocity".into()
            } else {
                "no sample carried a seq, so losses cannot be counted".into()
            });
        }
        if errors > 0 {
            failed.push(format!("{errors} message(s) on te/errors, first: {}", observer.errors().first().cloned().unwrap_or_default()));
        }
        if line.missing_pct > c.max_missing_pct {
            failed.push(format!("{:.3} % of samples lost ({missing} of {})", line.missing_pct, missing + received));
        }
        if drain_s > c.max_drain_s {
            failed.push(format!("drain_s {drain_s:.1} above {}", c.max_drain_s));
        }
        if !failed.is_empty() {
            println!("{}", serde_json::to_string(&line).unwrap());
            return Err(format!("{}: {}", run.label(), failed.join("; ")));
        }
    }
    Ok(line)
}

fn connectors_count(pids: &[u32]) -> usize {
    pids.len() - 3
}

/// Append `line` to `results/<host>-<date>.jsonl`.
pub fn record(results: &Path, line: &ResultLine) -> Result<PathBuf, String> {
    use std::io::Write;
    std::fs::create_dir_all(results).map_err(|e| e.to_string())?;
    let date = time::OffsetDateTime::now_utc().date();
    let path = results.join(format!("{}-{date}.jsonl", line.host.name));
    let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    writeln!(f, "{}", serde_json::to_string(line).unwrap()).map_err(|e| e.to_string())?;
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn span_is_whole_polls_between_passes() {
        // Passes at 1000 + k*1000 ms, 100 values each, spread over 30 ms.
        let mut c = Counts::default();
        let mut add = |ts: f64| c.add_for_test(ts, 1);
        for k in 0..20 {
            for v in 0..100 {
                add(1000.0 + k as f64 * 1000.0 + (v % 3) as f64 * 10.0);
            }
        }
        let (a, b, polls) = polled_span(&c, 1000.0, 21000.0, 1000.0, 1000.0);
        assert!(a >= 2000.0 && b <= 20000.0, "{a}..{b}");
        assert_eq!(polls, (b - a) / 1000.0);
        assert_eq!(c.values_between(a, b), polls as u64 * 100);
    }
}
