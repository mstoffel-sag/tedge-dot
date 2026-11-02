//! `summary`: the median of the last runs per scenario, rate, variant and implementation, as
//! a markdown table with Rust and C side by side. Results only compare on one host with one
//! set of tool versions, so any difference between the lines summarized is printed as a
//! warning naming the fields.

use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

/// Runs per group that go into the median (the most recent ones).
const RUNS: usize = 3;

fn get<'a>(v: &'a Value, path: &str) -> Option<&'a Value> {
    path.split('.').try_fold(v, |v, k| v.get(k))
}

fn num(v: &Value, path: &str) -> Option<f64> {
    get(v, path).and_then(Value::as_f64)
}

fn text(v: &Value, path: &str) -> String {
    get(v, path).and_then(Value::as_str).unwrap_or("").to_string()
}

fn median(mut xs: Vec<f64>) -> Option<f64> {
    if xs.is_empty() {
        return None;
    }
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let m = xs.len() / 2;
    Some(if xs.len() % 2 == 1 { xs[m] } else { (xs[m - 1] + xs[m]) / 2.0 })
}

pub fn load(files: &[PathBuf]) -> Result<Vec<Value>, String> {
    let mut lines = Vec::new();
    for f in files {
        let t = std::fs::read_to_string(f).map_err(|e| format!("{}: {e}", f.display()))?;
        for (i, l) in t.lines().enumerate().filter(|(_, l)| !l.trim().is_empty()) {
            lines.push(serde_json::from_str(l).map_err(|e| format!("{}:{}: {e}", f.display(), i + 1))?);
        }
    }
    Ok(lines)
}

/// The `.jsonl` files in `dir` for `host`.
pub fn files_for(dir: &Path, host: &str) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.extension().is_some_and(|e| e == "jsonl")
                && p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with(&format!("{host}-")))
        })
        .collect();
    v.sort();
    v
}

/// Fields that must agree for results to compare.
const SAME: [(&str, &str); 4] = [
    ("host.name", "host"),
    ("host.cpu", "CPU"),
    ("versions.tedge", "tedge version"),
    ("versions.mosquitto", "mosquitto version"),
];

pub fn warnings(lines: &[Value]) -> Vec<String> {
    SAME.iter()
        .filter_map(|(path, label)| {
            let seen: BTreeSet<String> = lines.iter().map(|l| text(l, path)).collect();
            (seen.len() > 1).then(|| format!("the results differ in {label}: {}", seen.into_iter().collect::<Vec<_>>().join(" / ")))
        })
        .collect()
}

const COLUMNS: [(&str, &str, usize); 10] = [
    ("Achieved readings/s", "achieved_rate", 0),
    ("Connector µs/reading", "us_per_reading.connector", 2),
    ("Connector CPU %", "cpu_pct.connector", 0),
    ("Broker CPU %", "cpu_pct.broker", 0),
    ("Mapper CPU %", "cpu_pct.mapper", 0),
    ("c8y msgs/s", "msgs_per_s.c8y", 0),
    ("Rate shortfall %", "rate_shortfall_pct", 1),
    ("Missing %", "missing_pct", 2),
    ("drain_s", "drain_s", 1),
    ("te/errors", "te_errors", 0),
];

/// Markdown summary of `lines`; `restricted` lists scenarios run for some implementations only,
/// with the reason.
pub fn markdown(lines: &[Value], restricted: &[(String, String)]) -> String {
    // (scenario, rate, variant) -> impl -> runs in file order
    let mut groups: BTreeMap<(String, u64, String), BTreeMap<String, Vec<&Value>>> = BTreeMap::new();
    for l in lines {
        let mut variant = Vec::new();
        if let Some(i) = num(l, "params.instances").filter(|i| *i > 1.0) {
            variant.push(format!("{i} instances"));
        }
        if let Some(r) = get(l, "params.ts_resolution").and_then(Value::as_str) {
            variant.push(format!("ts_resolution {r}"));
        }
        let key = (text(l, "scenario"), num(l, "target_rate").unwrap_or(0.0) as u64, variant.join(", "));
        groups.entry(key).or_default().entry(text(l, "impl")).or_default().push(l);
    }

    let mut md = String::new();
    let first = lines.first();
    let _ = writeln!(md, "# Performance baseline: {}\n", first.map(|l| text(l, "host.name")).unwrap_or_default());
    if let Some(l) = first {
        let _ = writeln!(
            md,
            "- Host: {} ({} cores), {}\n- Tools: {}, {}\n- Tree: {}{}\n- Each cell is the median of the last {RUNS} runs, as `Rust / C`.\n",
            text(l, "host.cpu"),
            num(l, "host.cores").unwrap_or(0.0),
            text(l, "host.os"),
            text(l, "versions.tedge"),
            text(l, "versions.mosquitto"),
            text(l, "git.sha"),
            if get(l, "git.dirty").and_then(Value::as_bool).unwrap_or(false) { " (uncommitted changes)" } else { "" },
        );
    }
    for w in warnings(lines) {
        let _ = writeln!(md, "> **Warning:** {w}. Compare only results from one host and one set of tool versions.\n");
    }
    if !restricted.is_empty() {
        let _ = writeln!(md, "Scenarios that run for some implementations only:\n");
        for (name, why) in restricted {
            let _ = writeln!(md, "- `{name}`: {why}");
        }
        md.push('\n');
    }

    let mut scenario = String::new();
    for ((name, rate, variant), impls) in &groups {
        if *name != scenario {
            scenario = name.clone();
            let _ = writeln!(md, "\n## {name}\n");
            let header: Vec<&str> = COLUMNS.iter().map(|c| c.0).collect();
            let _ = writeln!(md, "| Rate | Variant | {} |", header.join(" | "));
            let _ = writeln!(md, "|---:|---|{}", "---:|".repeat(COLUMNS.len()));
        }
        let cell = |imp: &str, path: &str, digits: usize| -> String {
            let runs = impls.get(imp).map(|r| &r[r.len().saturating_sub(RUNS)..]).unwrap_or(&[]);
            median(runs.iter().filter_map(|l| num(l, path)).collect())
                .map(|m| format!("{m:.digits$}"))
                .unwrap_or_else(|| "–".into())
        };
        let cells: Vec<String> = COLUMNS
            .iter()
            .map(|(_, path, d)| format!("{} / {}", cell("rust", path, *d), cell("c", path, *d)))
            .collect();
        let _ = writeln!(md, "| {rate} | {} | {} |", if variant.is_empty() { "–" } else { variant }, cells.join(" | "));
    }
    md
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn line(imp: &str, achieved: f64, tedge: &str) -> Value {
        json!({"scenario": "s", "impl": imp, "target_rate": 1000.0, "achieved_rate": achieved,
               "params": {"instances": 1}, "host": {"name": "h", "cpu": "x", "cores": 8, "os": "o"},
               "versions": {"tedge": tedge, "mosquitto": "m"}, "git": {"sha": "abc", "dirty": false}})
    }

    #[test]
    fn median_of_last_runs_side_by_side() {
        let lines = vec![
            line("rust", 1.0, "t"),
            line("rust", 900.0, "t"),
            line("rust", 1000.0, "t"),
            line("rust", 950.0, "t"),
            line("c", 800.0, "t"),
        ];
        let md = markdown(&lines, &[]);
        assert!(md.contains("| 1000 | – | 950 / 800 |"), "{md}");
        assert!(!md.contains("Warning"));
    }

    #[test]
    fn differing_versions_warn() {
        let md = markdown(&[line("rust", 1.0, "2.0.1"), line("rust", 1.0, "2.0.2")], &[]);
        assert!(md.contains("tedge version: 2.0.1 / 2.0.2"), "{md}");
    }
}
