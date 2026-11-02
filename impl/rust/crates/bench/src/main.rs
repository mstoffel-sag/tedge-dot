//! `tedge-dot-bench`: the performance harness (doc/performance.md).
//!
//! It measures the real connector runtimes and the real `tedge-mapper c8y` on a private,
//! offline stack, one scenario run at a time, and records one JSON result line per run.

mod host;
mod observe;
mod procs;
mod run;
mod scenario;
mod stack;
mod summary;

use clap::{Parser, Subcommand};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;

#[derive(Parser)]
#[command(name = "tedge-dot-bench", version, about = "Performance harness for tedge-dot")]
struct Cli {
    /// The repository root (default: found from the current directory).
    #[arg(long, global = true)]
    repo: Option<PathBuf>,
    /// The stack's private directory (default: <repo>/bench/.stack).
    #[arg(long, global = true)]
    stack_dir: Option<PathBuf>,
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Start or stop the offline stack (private mosquitto + tedge-mapper c8y).
    Stack {
        #[arg(value_parser = ["up", "down"])]
        action: String,
    },
    /// Write the connector configs of one scenario run.
    GenConfig {
        scenario: String,
        #[arg(long)]
        rate: Option<f64>,
        #[arg(long = "impl", default_value = "rust")]
        implementation: String,
        #[arg(long)]
        out: PathBuf,
        /// Broker port (default: the stack's).
        #[arg(long)]
        port: Option<u16>,
    },
    /// Count what arrives on the stack's broker for a while and print it.
    Observe {
        #[arg(long, default_value_t = 10.0)]
        seconds: f64,
    },
    /// Check the observer counts a flood of messages without loss.
    Selftest {
        #[arg(long, default_value_t = 2_000_000)]
        messages: usize,
    },
    /// Run scenarios (`all` for every one) and record a result line per run.
    Run {
        scenarios: Vec<String>,
        /// Only this implementation (default: the scenario's, normally both).
        #[arg(long = "impl", env = "IMPL")]
        implementation: Option<String>,
        /// Only this rate instead of the scenario's rates.
        #[arg(long)]
        rate: Option<f64>,
        /// Window length, e.g. 10s.
        #[arg(long)]
        window: Option<String>,
        /// Runs per scenario rate.
        #[arg(long, default_value_t = 1)]
        repeat: usize,
        /// Fail on any te/errors message, more than 0.1 % of samples lost or drain_s above 5.
        #[arg(long)]
        check: bool,
        /// Leave a stack this command started running.
        #[arg(long)]
        keep_stack: bool,
        /// Do not append the result lines to bench/results.
        #[arg(long)]
        no_record: bool,
    },
    /// Write bench/results/baseline-<host>.md from the recorded result lines.
    Summary {
        /// Result files (default: bench/results/<this host>-*.jsonl).
        files: Vec<PathBuf>,
        /// Print instead of writing the baseline file.
        #[arg(long)]
        stdout: bool,
    },
}

fn find_repo() -> Option<PathBuf> {
    let mut dir = std::env::current_dir().ok()?;
    loop {
        if dir.join("bench/scenarios.toml").exists() {
            return Some(dir);
        }
        if !dir.pop() {
            return None;
        }
    }
}

fn paths(repo: &Path, stack_dir: Option<PathBuf>) -> run::Paths {
    let env = |k: &str, d: PathBuf| std::env::var_os(k).map(PathBuf::from).unwrap_or(d);
    run::Paths {
        repo: repo.to_path_buf(),
        rust_bin: env("TDOT_BENCH_RUST_BIN", repo.join("impl/rust/target/release/tedge-dot")),
        c_bin: env("TDOT_BENCH_C_BIN", repo.join("impl/c/build-bench/tedge-dot")),
        conformance_bin: env("TDOT_BENCH_CONFORMANCE_BIN", repo.join("impl/rust/target/release/ot-conformance")),
        stack_dir: stack_dir.unwrap_or_else(|| repo.join("bench/.stack")),
        results_dir: repo.join("bench/results"),
    }
}

fn parse_window(s: &str) -> Result<Duration, String> {
    let secs: f64 = s.trim_end_matches('s').parse().map_err(|_| format!("--window '{s}' is not a number of seconds such as 10s"))?;
    Ok(Duration::from_secs_f64(secs))
}

async fn main_inner(cli: Cli) -> Result<(), String> {
    let repo = cli.repo.or_else(find_repo).ok_or("cannot find the repository root (bench/scenarios.toml); pass --repo")?;
    let p = paths(&repo, cli.stack_dir);
    let scenarios_file = repo.join("bench/scenarios.toml");
    match cli.command {
        Cmd::Stack { action } => {
            if action == "up" {
                let s = stack::up(&p.stack_dir).await?;
                println!("stack up: broker port {}, broker pid {}, mapper pid {}, logs {}", s.broker_port, s.broker_pid, s.mapper_pid, s.logs().display());
            } else {
                stack::down(&p.stack_dir)?;
                println!("stack down");
            }
        }
        Cmd::GenConfig { scenario, rate, implementation, out, port } => {
            let file = scenario::load(&scenarios_file)?;
            let s = file.scenarios.iter().find(|s| s.name == scenario).ok_or(format!("no scenario {scenario}"))?;
            let runs = scenario::expand(s, &scenario::Overrides { rate, window: None, implementation: Some(implementation) })?;
            let port = port.or_else(|| stack::Stack::load(&p.stack_dir).map(|s| s.broker_port)).unwrap_or(1883);
            for r in runs {
                let dir = out.join(r.label());
                let files = scenario::write_configs(&r, &dir, port, Some("opc.tcp://127.0.0.1:4840"))?;
                println!("{}: {} file(s) in {}", r.label(), files.len(), dir.display());
            }
        }
        Cmd::Observe { seconds } => {
            let s = stack::Stack::load(&p.stack_dir).ok_or("the stack is not up (tedge-dot-bench stack up)")?;
            let o = observe::Observer::start(s.broker_port, &observe::Class::ALL).await?;
            tokio::time::sleep(Duration::from_secs_f64(seconds)).await;
            let out: serde_json::Map<String, serde_json::Value> = observe::Class::ALL
                .iter()
                .map(|c| {
                    let n = o.counts(*c);
                    (serde_json::to_value(c).unwrap().as_str().unwrap().to_string(), serde_json::json!({"msgs": n.msgs, "bytes": n.bytes, "values": n.values}))
                })
                .collect();
            println!("{}", serde_json::Value::Object(out));
        }
        Cmd::Selftest { messages } => selftest(&p, messages).await?,
        Cmd::Run { scenarios, implementation, rate, window, repeat, check, keep_stack, no_record } => {
            let file = scenario::load(&scenarios_file)?;
            let names: Vec<String> = if scenarios.is_empty() || scenarios == ["all"] {
                file.scenarios.iter().map(|s| s.name.clone()).collect()
            } else {
                scenarios
            };
            let window = window.as_deref().map(parse_window).transpose()?;
            let mut runs = Vec::new();
            for name in &names {
                let s = file.scenarios.iter().find(|s| &s.name == name).ok_or(format!("no scenario {name} in {}", scenarios_file.display()))?;
                if !s.description.is_empty() {
                    eprintln!("{name}: {}", s.description);
                }
                runs.extend(scenario::expand(s, &scenario::Overrides { rate, window, implementation: implementation.clone() })?);
            }
            // Fail before starting anything when a binary lacks the module.
            for imp in runs.iter().map(|r| r.implementation.clone()).collect::<std::collections::BTreeSet<_>>() {
                run::check_module(p.binary(&imp), &imp, &std::env::temp_dir().join("tedge-dot-bench-check"))?;
            }
            let owned = stack::Stack::load(&p.stack_dir).is_none();
            let st = stack::up(&p.stack_dir).await?;
            let checks = check.then_some(run::Checks { max_missing_pct: 0.1, max_drain_s: 5.0 });
            let mut failed = Vec::new();
            for r in &runs {
                for i in 0..repeat {
                    eprintln!("==> {} (run {}/{repeat})", r.label(), i + 1);
                    match run::measure(&p, &st, r, checks).await {
                        Ok(line) => {
                            println!("{}", serde_json::to_string(&line).unwrap());
                            if !no_record {
                                run::record(&p.results_dir, &line)?;
                            }
                        }
                        Err(e) => {
                            eprintln!("error: {e}");
                            failed.push(e);
                        }
                    }
                }
            }
            if owned && !keep_stack {
                stack::down(&p.stack_dir)?;
            }
            if !failed.is_empty() {
                return Err(format!("{} run(s) failed", failed.len()));
            }
        }
        Cmd::Summary { files, stdout } => {
            let host = host::host().name;
            let files = if files.is_empty() { summary::files_for(&p.results_dir, &host) } else { files };
            if files.is_empty() {
                return Err(format!("no result files for host {host} in {}", p.results_dir.display()));
            }
            let lines = summary::load(&files)?;
            let restricted: Vec<(String, String)> = scenario::load(&scenarios_file)?
                .scenarios
                .into_iter()
                .filter_map(|s| s.impls.as_ref().map(|i| (s.name.clone(), format!("{} only. {}", i.join(", "), s.impls_reason.clone().unwrap_or_default()))))
                .collect();
            for w in summary::warnings(&lines) {
                eprintln!("warning: {w}");
            }
            let md = summary::markdown(&lines, &restricted);
            if stdout {
                print!("{md}");
            } else {
                let out = p.results_dir.join(format!("baseline-{host}.md"));
                std::fs::write(&out, md).map_err(|e| e.to_string())?;
                println!("wrote {}", out.display());
            }
        }
    }
    Ok(())
}

/// Publish `messages` small sample-shaped messages as fast as one client can and check the
/// observer counted every one. Needs the stack.
async fn selftest(p: &run::Paths, messages: usize) -> Result<(), String> {
    use rumqttc::{AsyncClient, MqttOptions, QoS};
    let s = stack::Stack::load(&p.stack_dir).ok_or("the stack is not up (tedge-dot-bench stack up)")?;
    let o = observe::Observer::start(s.broker_port, &[observe::Class::Samples]).await?;
    let mut opts = MqttOptions::new("tedge-dot-bench-selftest", "127.0.0.1", s.broker_port);
    opts.set_max_packet_size(1 << 20, 1 << 20);
    let (client, mut events) = AsyncClient::new(opts, 10_000);
    let pump = tokio::spawn(async move { while events.poll().await.is_ok() {} });
    let t0 = observe::now_ms();
    for i in 0..messages {
        let payload = format!("{{\"ts_ms\":{t0},\"value\":{i}}}");
        client
            .publish(format!("te/device/selftest/ot/bench/sample/p{}", i % 1000), QoS::AtMostOnce, false, payload)
            .await
            .map_err(|e| e.to_string())?;
    }
    let sent_s = (observe::now_ms() - t0) / 1000.0;
    o.wait_quiet(&[observe::Class::Samples], Duration::from_secs(2), Duration::from_secs(120)).await;
    let n = o.counts(observe::Class::Samples);
    let count_s = (n.last_ms - n.first_ms) / 1000.0;
    pump.abort();
    println!(
        "published {messages} in {sent_s:.2} s ({:.0} msg/s); observed {} in {count_s:.2} s ({:.0} msg/s); lost {}",
        messages as f64 / sent_s,
        n.msgs,
        n.msgs as f64 / count_s.max(1e-9),
        messages as u64 - n.msgs.min(messages as u64)
    );
    if n.msgs as usize != messages {
        return Err(format!("the observer counted {} of {messages} messages", n.msgs));
    }
    Ok(())
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let rt = tokio::runtime::Builder::new_multi_thread().enable_all().build().expect("tokio runtime");
    match rt.block_on(main_inner(cli)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}
