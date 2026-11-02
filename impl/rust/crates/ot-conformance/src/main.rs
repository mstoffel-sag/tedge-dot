//! `ot-conformance` binary: run the OT connector conformance suite from a manifest.
//!
//! ```sh
//! # Layers 1 & 2 — fast, no simulator:
//! ot-conformance check --spec ./conformance.toml --static
//!
//! # Layer 3 — with the protocol simulator + a test broker:
//! ot-conformance check --spec ./conformance.toml --behavioural
//!
//! # Everything (the default):
//! ot-conformance check --spec ./conformance.toml --junit report.xml --json report.json
//! ```
//!
//! Exits non-zero on any failing check.

use clap::{Args, Parser, Subcommand};
use ot_conformance::{manifest::Manifest, run_suite, Selection};
use std::path::PathBuf;
use std::process::ExitCode;
use tracing_subscriber::EnvFilter;

#[derive(Parser)]
#[command(name = "ot-conformance", version, about, long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Run the conformance suite against a connector's conformance.toml.
    Check(CheckArgs),
    /// Run a standalone simulator for throughput measurements (bench/, doc/performance.md),
    /// until interrupted.
    #[cfg(feature = "opcua")]
    #[command(subcommand)]
    Serve(Serve),
}

#[cfg(feature = "opcua")]
#[derive(Subcommand)]
enum Serve {
    /// An OPC UA server with N `Double` nodes ns=2;s=Bulk0 .. Bulk<N-1>, anonymous with
    /// security None, each starting at its index. Prints the endpoint on stdout once it
    /// accepts connections.
    Opcua(ServeOpcuaArgs),
}

#[cfg(feature = "opcua")]
#[derive(Args)]
struct ServeOpcuaArgs {
    /// Number of bulk nodes.
    #[arg(long)]
    points: usize,
    /// Nodes changed per second, round robin and spread evenly (each change adds 1).
    #[arg(long, default_value_t = 0.0)]
    change_rate: f64,
    /// Address to listen on and advertise.
    #[arg(long, default_value = "127.0.0.1")]
    host: String,
    /// Port to listen on; 0 picks a free one.
    #[arg(long, default_value_t = 4840)]
    port: u16,
}

#[derive(Args)]
struct CheckArgs {
    /// Path to the connector's conformance manifest.
    #[arg(long, value_name = "conformance.toml")]
    spec: PathBuf,
    /// Run only layers 1–2 (schema examples + golden decode vectors; no simulator).
    #[arg(long = "static")]
    static_only: bool,
    /// Run only layer 3 (behavioural: connector ⇄ simulator ⇄ test broker).
    #[arg(long = "behavioural", conflicts_with = "static_only")]
    behavioural_only: bool,
    /// Override the SDK's builtin golden vector file (JSON).
    #[arg(long, value_name = "vectors.json")]
    vectors: Option<PathBuf>,
    /// Write a JUnit XML report here.
    #[arg(long, value_name = "report.xml")]
    junit: Option<PathBuf>,
    /// Write a JSON report here.
    #[arg(long, value_name = "report.json")]
    json: Option<PathBuf>,
}

#[tokio::main]
async fn main() -> ExitCode {
    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("warn"));
    tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .init();

    let cli = Cli::parse();
    match cli.command {
        Command::Check(args) => match check(args).await {
            Ok(conformant) => {
                if conformant {
                    ExitCode::SUCCESS
                } else {
                    ExitCode::FAILURE
                }
            }
            Err(e) => {
                eprintln!("error: {e}");
                ExitCode::from(2)
            }
        },
        #[cfg(feature = "opcua")]
        Command::Serve(Serve::Opcua(args)) => {
            match ot_conformance::sim::bulk::BulkServer::start(
                args.points,
                args.change_rate,
                &args.host,
                args.port,
            )
            .await
            {
                Ok(server) => {
                    println!("{}", server.endpoint);
                    let _ = tokio::signal::ctrl_c().await;
                    drop(server);
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("error: {e}");
                    ExitCode::from(2)
                }
            }
        }
    }
}

async fn check(args: CheckArgs) -> Result<bool, String> {
    let manifest = Manifest::load(&args.spec)?;
    let selection = Selection {
        static_layers: !args.behavioural_only,
        behavioural: !args.static_only,
    };
    let vectors_text = match &args.vectors {
        Some(path) => Some(
            std::fs::read_to_string(path)
                .map_err(|e| format!("failed to read vectors '{}': {e}", path.display()))?,
        ),
        None => None,
    };

    let report = run_suite(&manifest, selection, vectors_text.as_deref()).await?;
    print!("{}", report.render_text());

    if let Some(path) = &args.junit {
        report.write_junit(path)?;
        eprintln!("junit report written to {}", path.display());
    }
    if let Some(path) = &args.json {
        report.write_json(path)?;
        eprintln!("json report written to {}", path.display());
    }
    Ok(report.conformant())
}
