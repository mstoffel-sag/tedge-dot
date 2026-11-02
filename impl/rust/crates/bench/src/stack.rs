//! The offline bench stack: a private mosquitto and the host's real `tedge-mapper c8y`, from a
//! private configuration directory, with no connection to Cumulocity.
//!
//! The mapper converts measurements only when it believes the cloud bridge is up and the
//! devices are registered, so the stack points `c8y.url` at a placeholder host, moves the
//! mapper's proxy and HTTP servers to free ports, publishes a retained `{"status":"up"}` as the
//! bridge's health, and registers child devices. `stack up` checks that a test measurement is
//! converted before reporting ready.

use crate::observe::{Class, Observer};
use crate::procs;
use rumqttc::{AsyncClient, Event, MqttOptions, Packet, QoS};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

/// How long `stack up` waits for its test measurement to be converted.
const CHECK_LIMIT: Duration = Duration::from_secs(60);

pub const BRIDGE_HEALTH: &str = "te/device/main/service/mosquitto-c8y-bridge/status/health";

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Stack {
    pub dir: PathBuf,
    pub broker_port: u16,
    pub broker_pid: u32,
    pub broker_name: String,
    pub mapper_pid: u32,
    pub mapper_name: String,
}

impl Stack {
    pub fn etc(&self) -> PathBuf {
        self.dir.join("etc")
    }
    pub fn logs(&self) -> PathBuf {
        self.dir.join("logs")
    }
    pub fn flows_dir(&self) -> PathBuf {
        self.etc().join("mappers/c8y/flows")
    }
    pub fn mapper_log(&self) -> PathBuf {
        self.logs().join("tedge-mapper-c8y.log")
    }
    fn state_file(dir: &Path) -> PathBuf {
        dir.join("stack.json")
    }

    /// The stack recorded in `dir`, if its processes are still the ones it started.
    pub fn load(dir: &Path) -> Option<Stack> {
        let text = std::fs::read_to_string(Self::state_file(dir)).ok()?;
        let s: Stack = serde_json::from_str(&text).ok()?;
        let ours = |pid: u32, name: &str| procs::alive(pid) && procs::name(pid).as_deref() == Some(name);
        (ours(s.broker_pid, &s.broker_name) && ours(s.mapper_pid, &s.mapper_name)).then_some(s)
    }

    pub fn pids(&self) -> (u32, u32) {
        (self.broker_pid, self.mapper_pid)
    }
}

fn free_port() -> std::io::Result<u16> {
    Ok(std::net::TcpListener::bind(("127.0.0.1", 0))?.local_addr()?.port())
}

fn command_output(cmd: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(cmd).args(args).output().ok()?;
    Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// Publish `messages` retained with QoS 1 and wait for every acknowledgement.
pub async fn publish_retained(port: u16, messages: &[(String, String)]) -> Result<(), String> {
    publish(port, messages, true).await
}

pub async fn publish(port: u16, messages: &[(String, String)], retain: bool) -> Result<(), String> {
    let mut opts = MqttOptions::new(format!("tedge-dot-bench-pub-{}", std::process::id()), "127.0.0.1", port);
    opts.set_max_packet_size(4 << 20, 4 << 20);
    let (client, mut events) = AsyncClient::new(opts, messages.len().max(1) + 8);
    for (topic, payload) in messages {
        client
            .publish(topic.as_str(), QoS::AtLeastOnce, retain, payload.as_bytes().to_vec())
            .await
            .map_err(|e| e.to_string())?;
    }
    let mut acked = 0;
    let wait = async {
        while acked < messages.len() {
            match events.poll().await {
                Ok(Event::Incoming(Packet::PubAck(_))) => acked += 1,
                Ok(_) => {}
                Err(e) => return Err(format!("cannot publish to the broker on port {port}: {e}")),
            }
        }
        Ok(())
    };
    tokio::time::timeout(Duration::from_secs(30), wait)
        .await
        .map_err(|_| format!("publishing {} messages timed out", messages.len()))??;
    let _ = client.disconnect().await;
    Ok(())
}

/// Register `devices` as child devices of the main device (retained), with the same message
/// the `ot-registration` flow publishes for a `protocol` device: the mapper rejects a later
/// registration of an existing device that adds fields, so the two must not differ.
pub async fn register(port: u16, devices: &[String], protocol: &str) -> Result<(), String> {
    let msgs: Vec<(String, String)> = devices
        .iter()
        .map(|d| {
            let body = serde_json::json!({
                "@type": "child-device",
                "name": d,
                "type": format!("{protocol}-device"),
                "ot-protocol": protocol,
            });
            (format!("te/device/{d}//"), body.to_string())
        })
        .collect();
    publish_retained(port, &msgs).await
}

async fn wait_for_port(port: u16, limit: Duration) -> bool {
    let end = tokio::time::Instant::now() + limit;
    while tokio::time::Instant::now() < end {
        if tokio::net::TcpStream::connect(("127.0.0.1", port)).await.is_ok() {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    false
}

pub async fn up(dir: &Path) -> Result<Stack, String> {
    if let Some(s) = Stack::load(dir) {
        return Ok(s);
    }
    let dir = std::fs::canonicalize(dir).or_else(|_| {
        std::fs::create_dir_all(dir)?;
        std::fs::canonicalize(dir)
    }).map_err(|e| format!("{}: {e}", dir.display()))?;
    let etc = dir.join("etc");
    let logs = dir.join("logs");
    for d in [etc.join("mappers/c8y/flows"), logs.clone(), dir.join("data")] {
        std::fs::create_dir_all(&d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let port = |what: &str| free_port().map_err(|e| format!("no free port for {what}: {e}"));
    let (broker_port, proxy_port, http_port) = (port("mosquitto")?, port("the c8y proxy")?, port("the HTTP server")?);

    let user = std::env::var("USER").ok().or_else(|| command_output("id", &["-un"])).unwrap_or_default();
    let group = command_output("id", &["-gn"]).unwrap_or_default();
    let write = |path: PathBuf, text: String| std::fs::write(&path, text).map_err(|e| format!("{}: {e}", path.display()));
    write(
        etc.join("tedge.toml"),
        format!(
            "[config]\nversion = \"2\"\n\
             [mqtt.bind]\nport = {broker_port}\n[mqtt.client]\nport = {broker_port}\n\
             [mqtt.bridge]\nbuilt_in = false\n\
             [logs]\npath = \"{logs}\"\n[data]\npath = \"{data}\"\n\
             [run]\nlock_files = false\n[device]\nid = \"bench\"\n\
             [c8y]\nurl = \"bench.invalid\"\n[c8y.bridge]\ntopic_prefix = \"c8y\"\n\
             [c8y.proxy.bind]\nport = {proxy_port}\n[c8y.proxy.client]\nport = {proxy_port}\n\
             [http.bind]\nport = {http_port}\n[http.client]\nport = {http_port}\n",
            logs = logs.display(),
            data = dir.join("data").display(),
        ),
    )?;
    write(etc.join("system.toml"), format!("user = \"{user}\"\ngroup = \"{group}\"\n"))?;
    let conf = dir.join("mosquitto.conf");
    write(
        conf.clone(),
        format!(
            "listener {broker_port} 127.0.0.1\nallow_anonymous true\n\
             max_queued_messages 1000000\nmax_inflight_messages 0\npersistence false\nlog_dest none\n"
        ),
    )?;

    let mut started = procs::Guard::default();
    let broker = procs::spawn_logged(Command::new("mosquitto").arg("-c").arg(&conf), &logs.join("mosquitto.log"))
        .map_err(|e| format!("cannot start mosquitto: {e}"))?;
    started.0.push(broker.id());
    if !wait_for_port(broker_port, Duration::from_secs(5)).await {
        return Err(format!("mosquitto did not accept connections on port {broker_port}; see {}", logs.join("mosquitto.log").display()));
    }
    let mapper_log = logs.join("tedge-mapper-c8y.log");
    let mapper = procs::spawn_logged(Command::new("tedge-mapper").arg("--config-dir").arg(&etc).arg("c8y"), &mapper_log)
        .map_err(|e| format!("cannot start tedge-mapper: {e}"))?;
    started.0.push(mapper.id());

    let stack = Stack {
        dir: dir.clone(),
        broker_port,
        broker_pid: broker.id(),
        broker_name: procs::name(broker.id()).unwrap_or_default(),
        mapper_pid: mapper.id(),
        mapper_name: procs::name(mapper.id()).unwrap_or_default(),
    };

    publish_retained(broker_port, &[(BRIDGE_HEALTH.into(), "{\"status\":\"up\"}".into())]).await?;
    register(broker_port, &["plc0".to_string()], "bench").await?;

    // The check: a measurement must come out converted. Offline, the mapper starts converting
    // about 30 s after it starts (measured with thin-edge.io 2.0.2), so allow 60 s.
    let observer = Observer::start(broker_port, &[Class::C8y]).await?;
    let end = tokio::time::Instant::now() + CHECK_LIMIT;
    while observer.counts(Class::C8y).msgs == 0 {
        if tokio::time::Instant::now() >= end {
            return Err(format!(
                "stack check failed: a measurement on te/device/plc0///m/bench did not reach \
                 c8y/measurement/measurements/create within {} s; see the mapper log {}",
                CHECK_LIMIT.as_secs(),
                stack.mapper_log().display()
            ));
        }
        publish(broker_port, &[("te/device/plc0///m/bench".into(), "{\"check\":1}".into())], false).await?;
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
    drop(observer);

    let text = serde_json::to_string_pretty(&stack).unwrap();
    std::fs::write(Stack::state_file(&dir), text).map_err(|e| e.to_string())?;
    started.0.clear(); // keep them running
    Ok(stack)
}

/// Stop the processes `stack up` started in `dir`, and only those.
pub fn down(dir: &Path) -> Result<(), String> {
    let file = Stack::state_file(dir);
    let Ok(text) = std::fs::read_to_string(&file) else {
        return Ok(());
    };
    let s: Stack = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", file.display()))?;
    for (pid, name) in [(s.mapper_pid, &s.mapper_name), (s.broker_pid, &s.broker_name)] {
        if procs::alive(pid) && procs::name(pid).as_deref() == Some(name.as_str()) {
            procs::terminate(pid, Duration::from_secs(5));
        }
    }
    let _ = std::fs::remove_file(file);
    Ok(())
}
