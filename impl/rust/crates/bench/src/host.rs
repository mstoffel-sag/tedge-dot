//! What a result line records about where it was measured: host, tool versions and the git
//! state of the tree.

use serde::Serialize;
use std::path::Path;
use std::process::Command;

#[derive(Clone, Debug, Serialize)]
pub struct Host {
    pub name: String,
    pub cpu: String,
    pub cores: usize,
    pub os: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct Versions {
    pub tedge: String,
    pub mosquitto: String,
    pub tedge_dot: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct Git {
    pub sha: String,
    pub dirty: bool,
}

fn first_line(cmd: &str, args: &[&str]) -> String {
    Command::new(cmd)
        .args(args)
        .output()
        .ok()
        .map(|o| {
            let text = if o.stdout.is_empty() { o.stderr } else { o.stdout };
            String::from_utf8_lossy(&text).lines().next().unwrap_or("").trim().to_string()
        })
        .unwrap_or_default()
}

pub fn host() -> Host {
    let cpu = if cfg!(target_os = "macos") {
        first_line("sysctl", &["-n", "machdep.cpu.brand_string"])
    } else {
        std::fs::read_to_string("/proc/cpuinfo")
            .ok()
            .and_then(|c| {
                c.lines()
                    .find(|l| l.starts_with("model name") || l.starts_with("Model"))
                    .and_then(|l| l.split_once(':'))
                    .map(|(_, v)| v.trim().to_string())
            })
            .unwrap_or_default()
    };
    let name = first_line("hostname", &["-s"]);
    Host {
        name: if name.is_empty() { "host".into() } else { name },
        cpu,
        cores: std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0),
        os: first_line("uname", &["-sr"]),
    }
}

pub fn versions(tedge_dot: &Path) -> Versions {
    // `mosquitto -h` prints "mosquitto version 2.0.x" first and exits non-zero.
    Versions {
        tedge: first_line("tedge", &["--version"]),
        mosquitto: first_line("mosquitto", &["-h"]),
        tedge_dot: first_line(&tedge_dot.to_string_lossy(), &["--version"]),
    }
}

pub fn git(repo: &Path) -> Git {
    let run = |args: &[&str]| {
        Command::new("git")
            .arg("-C")
            .arg(repo)
            .args(args)
            .output()
            .ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_default()
    };
    Git { sha: run(&["rev-parse", "HEAD"]), dirty: !run(&["status", "--porcelain"]).is_empty() }
}
