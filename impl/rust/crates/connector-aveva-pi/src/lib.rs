//! AVEVA PI Web API connector module (spec: `doc/connectors/aveva-pi-connector-spec.md`).
//!
//! tedge-dot's first connector for a REST/JSON system rather than a fieldbus, and the worked
//! example of `doc/connectors/rest-connectors.md`. The layers are kept apart:
//!
//! - [`client`]: HTTP and the Channels WebSocket (TLS, authentication, chunking, PI errors);
//! - [`value`]: the pure decoder of a PI value, checked against the shared golden vectors;
//! - [`resolve`]: WebIds and digital sets, looked up once per connection;
//! - [`push`] and [`cursor`]: push delivery, recorded polling and outage backfill;
//! - this file: the [`Connector`] glue.

pub mod client;
pub mod config;
pub mod cursor;
pub mod push;
pub mod resolve;
pub mod sample;
pub mod value;

use async_trait::async_trait;
use client::{chunks, query, ChunkSize, Client, HttpError};
use config::{Delivery, Device, Point};
use push::{Push, StartError, Target};
use resolve::{DigitalSets, Stream};
use serde_json::{json, Value as Json};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tedge_dot_sdk::{
    parse_duration, Capabilities, CommandRequest, CommandResult, ConfigError, Connector, ConnectorConfig,
    ConnectorError, DeviceId, LinkReport, LinkStatus, Mode, PointRef, Sample, SampleSink,
};
use tokio::task::JoinSet;
use tracing::{info, warn};

pub use sample::PROTOCOL;

/// What a connected device knows.
struct DeviceState {
    client: Arc<Client>,
    chunk: Arc<ChunkSize>,
    /// Point id → its stream.
    streams: HashMap<String, Stream>,
    /// Points the server did not have, looked up again after `resolve_retry`.
    unresolved: HashSet<String>,
    next_retry: Instant,
    /// Channels were refused: recorded delivery until the next reconnect (spec §5.2).
    fallback: bool,
    push: Option<Push>,
}

#[derive(Default)]
pub struct AvevaPiConnector {
    devices: Vec<Device>,
    service: String,
    default_interval: Duration,
    states: HashMap<String, DeviceState>,
    /// One client per `(base_url, auth, tls, timeouts)`, shared by devices (design D1).
    clients: HashMap<String, Arc<Client>>,
}

pub fn factory() -> Box<dyn Connector> {
    Box::<AvevaPiConnector>::default()
}

fn transport(e: impl ToString) -> ConnectorError {
    ConnectorError::Transport(e.to_string())
}

/// Look up the streams of `points` on `client`, `max` at a time.
async fn resolve_all(client: &Arc<Client>, points: Vec<Point>, max: usize) -> Result<HashMap<String, Option<Stream>>, HttpError> {
    let semaphore = Arc::new(tokio::sync::Semaphore::new(max.max(1)));
    let mut set = JoinSet::new();
    for p in points {
        let client = client.clone();
        let semaphore = semaphore.clone();
        set.spawn(async move {
            let _permit = semaphore.acquire_owned().await;
            (p.id.clone(), resolve::resolve(&client, &p.address).await)
        });
    }
    let mut out = HashMap::new();
    while let Some(joined) = set.join_next().await {
        let (id, result) = joined.map_err(|e| HttpError::Transport(e.to_string()))?;
        out.insert(id, result?);
    }
    Ok(out)
}

impl AvevaPiConnector {
    fn device(&self, name: &str) -> Result<&Device, ConnectorError> {
        self.devices
            .iter()
            .find(|d| d.name == name)
            .ok_or_else(|| ConnectorError::NotConnected(name.to_string()))
    }

    fn client_for(&mut self, d: &Device) -> Result<Arc<Client>, String> {
        let s = &d.settings;
        let key = format!(
            "{}|{:?}|{:?}|{:?}|{:?}",
            d.base_url, s.auth, s.tls, s.request_timeout, s.connect_timeout
        );
        if let Some(c) = self.clients.get(&key) {
            return Ok(c.clone());
        }
        let c = Arc::new(Client::new(&d.base_url, s)?);
        self.clients.insert(key, c.clone());
        Ok(c)
    }

    /// Connect one device (spec §7): check the root and the credentials, then resolve its points.
    async fn connect_device(&mut self, d: Device) -> LinkReport {
        if let Some(old) = self.states.remove(&d.name) {
            if let Some(p) = old.push {
                p.stop();
            }
        }
        let info = json!({ "base_url": d.base_url, "delivery": d.delivery.as_str() });
        let report = |status, reason: Option<String>| LinkReport {
            device: d.name.clone(),
            status,
            reason,
            info: Some(info.clone()),
        };
        if d.base_url.starts_with("http://") {
            warn!(device = %d.name, "base_url is plain HTTP: readings and writes are not encrypted");
        }
        if d.settings.tls.insecure_skip_verify && d.base_url.starts_with("https://") {
            warn!(device = %d.name, "tls.insecure_skip_verify is set: the server's certificate is not verified");
        }
        let client = match self.client_for(&d) {
            Ok(c) => c,
            Err(e) => return report(LinkStatus::Disconnected, Some(e)),
        };
        if let Err(e) = client.get("", "").await {
            return report(LinkStatus::Disconnected, Some(e.to_string()));
        }
        let resolved = match resolve_all(&client, d.points.clone(), d.settings.max_concurrent_requests).await {
            Ok(r) => r,
            Err(e) => return report(LinkStatus::Disconnected, Some(e.to_string())),
        };
        let mut sets = DigitalSets::default();
        let mut streams = HashMap::new();
        let mut unresolved = HashSet::new();
        for p in &d.points {
            match resolved.get(&p.id).cloned().flatten() {
                Some(mut s) => {
                    if let (Some(set), Some(server)) = (s.digital_set.clone(), resolve::server_of(&s.path)) {
                        match sets.get(&client, server, &set).await {
                            Ok(states) => s.digital = states,
                            Err(e) => return report(LinkStatus::Disconnected, Some(e.to_string())),
                        }
                    }
                    streams.insert(p.id.clone(), s);
                }
                None => {
                    warn!(device = %d.name, point = %p.id, "not found: {}", p.address.text());
                    unresolved.insert(p.id.clone());
                }
            }
        }
        info!(device = %d.name, resolved = streams.len(), unresolved = unresolved.len(), "connected to {}", d.base_url);
        self.states.insert(
            d.name.clone(),
            DeviceState {
                client,
                chunk: Arc::new(ChunkSize::new(d.settings.max_web_ids_per_request)),
                streams,
                unresolved,
                next_retry: Instant::now() + d.settings.resolve_retry,
                fallback: false,
                push: None,
            },
        );
        report(LinkStatus::Connected, None)
    }

    /// Look the unresolved points of a device up again once `resolve_retry` has passed. Returns
    /// whether any was found.
    async fn retry_unresolved(&mut self, d: &Device) -> bool {
        let Some(state) = self.states.get_mut(&d.name) else { return false };
        if state.unresolved.is_empty() || Instant::now() < state.next_retry {
            return false;
        }
        state.next_retry = Instant::now() + d.settings.resolve_retry;
        let points: Vec<Point> = d.points.iter().filter(|p| state.unresolved.contains(&p.id)).cloned().collect();
        let client = state.client.clone();
        let Ok(found) = resolve_all(&client, points, d.settings.max_concurrent_requests).await else { return false };
        let mut any = false;
        for (id, s) in found {
            if let Some(s) = s {
                info!(device = %d.name, point = %id, "resolved {}", s.path);
                state.unresolved.remove(&id);
                state.streams.insert(id, s);
                any = true;
            }
        }
        any
    }

    fn state_file(&self, d: &Device) -> PathBuf {
        d.settings.state_dir.join(&self.service).join(format!("{}.json", d.name))
    }
}

/// The values of a `streamsets/value` answer, by WebId.
fn values_by_web_id(doc: &Json) -> HashMap<String, Result<Json, String>> {
    let mut out = HashMap::new();
    for item in doc.get("Items").and_then(Json::as_array).into_iter().flatten() {
        let Some(web_id) = item.get("WebId").and_then(Json::as_str) else { continue };
        let entry = match (value::errors_of(item), item.get("Value")) {
            (Some(e), _) => Err(e),
            (None, Some(v)) => Ok(v.clone()),
            (None, None) => Err("no value".into()),
        };
        out.insert(web_id.to_string(), entry);
    }
    out
}

#[async_trait]
impl Connector for AvevaPiConnector {
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError> {
        let conn = config::connection(&config.connection).map_err(ConfigError::Invalid)?;
        let mut devices = Vec::with_capacity(config.devices.len());
        for d in &config.devices {
            devices.push(
                config::device(&d.name, &d.protocol_address, &conn, &d.points, d.default_mode)
                    .map_err(|e| ConfigError::Invalid(format!("device '{}': {e}", d.name)))?,
            );
        }
        self.service = config.connector.service_name();
        self.default_interval = config
            .connector
            .sampling_interval
            .as_deref()
            .and_then(parse_duration)
            .or_else(|| parse_duration(&config.connector.poll_interval))
            .unwrap_or(Duration::from_secs(10));
        self.devices = devices;
        Ok(())
    }

    fn capabilities(&self) -> Capabilities {
        Capabilities {
            protocol: PROTOCOL,
            version: env!("CARGO_PKG_VERSION"),
            modes: vec![Mode::Raw, Mode::Typed],
            datatypes: config::DATATYPES.to_vec(),
            point_kinds: vec!["point".into(), "attribute".into(), "stream".into()],
            command_verbs: vec!["write".into()],
            features: vec!["polling".into(), "subscribe".into(), "bulk_read".into(), "backfill".into()],
            subscribe: true,
        }
    }

    fn local_only_settings(&self) -> &'static [&'static str] {
        config::LOCAL_ONLY
    }

    fn pushes_point(&self, device: &DeviceId, _point: &PointRef) -> bool {
        self.devices
            .iter()
            .find(|d| &d.name == device)
            .is_some_and(|d| d.delivery != Delivery::Snapshot)
    }

    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError> {
        let devices = self.devices.clone();
        let mut reports = Vec::with_capacity(devices.len());
        for d in devices {
            reports.push(self.connect_device(d).await);
        }
        Ok(reports)
    }

    async fn reconnect(&mut self, device: &DeviceId) -> Result<LinkReport, ConnectorError> {
        let d = self.device(device)?.clone();
        Ok(self.connect_device(d).await)
    }

    async fn read_points(&mut self, device: &DeviceId, points: &[PointRef]) -> Result<Vec<Sample>, ConnectorError> {
        let d = self.device(device)?.clone();
        self.retry_unresolved(&d).await;
        let state = self.states.get(&d.name).ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        let wanted: Vec<&Point> = points
            .iter()
            .filter_map(|r| d.points.iter().find(|p| p.id == r.id))
            .collect();
        let mut ids: Vec<String> = Vec::new();
        for p in &wanted {
            if let Some(s) = state.streams.get(&p.id) {
                if !ids.contains(&s.web_id) {
                    ids.push(s.web_id.clone());
                }
            }
        }
        // All chunks in flight together, at most max_concurrent_requests at once.
        let mut values: HashMap<String, Result<Json, String>> = HashMap::new();
        let mut failure: Option<HttpError> = None;
        let mut pending = chunks(&ids, state.chunk.get(), d.settings.max_url_bytes, d.base_url.len() + 120);
        while !pending.is_empty() {
            let semaphore = Arc::new(tokio::sync::Semaphore::new(d.settings.max_concurrent_requests.max(1)));
            let mut set = JoinSet::new();
            for chunk in pending.drain(..) {
                let client = state.client.clone();
                let semaphore = semaphore.clone();
                set.spawn(async move {
                    let _permit = semaphore.acquire_owned().await;
                    let mut pairs: Vec<(&str, &str)> = chunk.iter().map(|w| ("webId", w.as_str())).collect();
                    pairs.push(("selectedFields", "Items.WebId;Items.Value;Items.Errors"));
                    let result = client.get("streamsets/value", &query(&pairs)).await;
                    (chunk, result)
                });
            }
            while let Some(joined) = set.join_next().await {
                let (chunk, result) = joined.map_err(transport)?;
                match result {
                    Ok(doc) => values.extend(values_by_web_id(&doc)),
                    Err(HttpError::TooLong(code)) if state.chunk.halve(&d.name, chunk.len(), code) => {
                        let half = chunk.len().div_ceil(2);
                        pending.push(chunk[..half].to_vec());
                        pending.push(chunk[half..].to_vec());
                    }
                    Err(e) => {
                        for w in &chunk {
                            values.insert(w.clone(), Err(e.to_string()));
                        }
                        if e.is_link_failure() {
                            failure.get_or_insert(e);
                        }
                    }
                }
            }
        }
        let mut samples = Vec::with_capacity(wanted.len());
        for p in wanted {
            let s = match state.streams.get(&p.id) {
                None => sample::bad(&d.name, p, p.address.text(), format!("not found: {}", p.address.text())),
                Some(stream) => match values.get(&stream.web_id) {
                    Some(Ok(v)) => sample::build(&d.name, p, stream, v, None, false),
                    Some(Err(e)) => sample::bad(&d.name, p, &stream.path, e.clone()),
                    None => sample::bad(&d.name, p, &stream.path, "no value".into()),
                },
            };
            samples.push(s);
        }
        match failure {
            None => Ok(samples),
            Some(e) if values.values().any(Result::is_ok) => Err(ConnectorError::LinkLost { samples, reason: e.to_string() }),
            Some(e) => Err(transport(e)),
        }
    }

    async fn subscribe(&mut self, device: &DeviceId, points: &[PointRef], sink: SampleSink) -> Result<(), ConnectorError> {
        let d = self.device(device)?.clone();
        let path = self.state_file(&d);
        let state = self.states.get_mut(&d.name).ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        if let Some(old) = state.push.take() {
            old.stop();
        }
        let mut targets = Vec::new();
        for r in points {
            let Some(p) = d.points.iter().find(|p| p.id == r.id) else { continue };
            match state.streams.get(&p.id) {
                Some(stream) => targets.push(Target { point: p.clone(), stream: stream.clone() }),
                None => {
                    let bad = sample::bad(&d.name, p, p.address.text(), format!("not found: {}", p.address.text()));
                    let _ = sink.send(bad).await;
                }
            }
        }
        if targets.is_empty() {
            return Ok(());
        }
        let expected: HashMap<String, String> =
            targets.iter().map(|t| (t.point.id.clone(), t.stream.web_id.clone())).collect();
        let every = d.settings.backfill.as_ref().map_or(Duration::from_secs(10), |b| b.cursor_flush);
        let store = Arc::new(Mutex::new(cursor::Store::load(path, &expected, every)));
        let interval = points
            .iter()
            .filter_map(|r| r.interval)
            .filter(|i| !i.is_zero())
            .min()
            .unwrap_or(self.default_interval);
        let recorded = d.delivery == Delivery::Recorded || state.fallback;
        let started = push::start(
            &d.name,
            state.client.clone(),
            state.chunk.clone(),
            &d.settings,
            recorded,
            targets.clone(),
            store.clone(),
            interval,
            sink.clone(),
        )
        .await;
        let push = match started {
            Ok(p) => p,
            Err(StartError::Refused(code, errors)) => {
                warn!(device = %d.name, "the server refused the Channels WebSocket (HTTP {code}{}{errors}); using recorded polling until the next reconnect",
                      if errors.is_empty() { "" } else { ": " });
                state.fallback = true;
                push::start(&d.name, state.client.clone(), state.chunk.clone(), &d.settings, true, targets, store, interval, sink)
                    .await
                    .map_err(|e| match e {
                        StartError::Refused(code, _) => transport(format!("HTTP {code}")),
                        StartError::Http(e) => transport(e),
                    })?
            }
            Err(StartError::Http(e)) => return Err(transport(e)),
        };
        state.push = Some(push);
        Ok(())
    }

    async fn check_subscription(&mut self, device: &DeviceId) -> Result<(), ConnectorError> {
        let d = self.device(device)?.clone();
        let state = self.states.get(&d.name).ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        if let Some(p) = &state.push {
            p.check().map_err(transport)?;
        }
        // A pushed point that was missing has appeared: subscribe again to deliver it.
        if d.delivery != Delivery::Snapshot && self.retry_unresolved(&d).await {
            return Err(transport("points were resolved after a retry; subscribing again"));
        }
        Ok(())
    }

    async fn execute(&mut self, device: &DeviceId, verb: &str, request: &CommandRequest) -> Result<CommandResult, ConnectorError> {
        if verb != "write" {
            return Err(ConnectorError::Unsupported(verb.to_string()));
        }
        let d = self.device(device)?.clone();
        let point = d
            .points
            .iter()
            .find(|p| p.id == request.point)
            .ok_or_else(|| ConnectorError::UnknownPoint { device: device.clone(), point: request.point.clone() })?;
        if !point.access.can_write() {
            return Err(ConnectorError::AccessDenied(request.point.clone()));
        }
        let state = self.states.get(&d.name).ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        let stream = state
            .streams
            .get(&point.id)
            .ok_or_else(|| ConnectorError::Other(format!("not found: {}", point.address.text())))?;
        let requested = match (&request.value, &request.raw) {
            (Some(v), _) => v.clone(),
            (None, Some(raw)) => serde_json::from_str(raw).unwrap_or(Json::String(raw.clone())),
            (None, None) => return Err(ConnectorError::Decode("the write has no value".into())),
        };
        let value = match (&stream.digital, stream.is_digital()) {
            (Some(set), _) => {
                let (name, code) = resolve::digital_state(set, &requested).ok_or_else(|| {
                    ConnectorError::Decode(format!(
                        "{requested} is not a state of the digital set {}",
                        stream.digital_set.as_deref().unwrap_or("of the point")
                    ))
                })?;
                json!({ "Name": name, "Value": code, "IsSystem": false })
            }
            // An AF enumeration attribute: PI checks the state itself.
            (None, true) => requested.clone(),
            (None, false) => {
                let dt = if point.mode == Mode::Typed { point.datatype } else { None };
                value::encode_write(&requested, dt).map_err(ConnectorError::Decode)?
            }
        };
        let q = query(&[("updateOption", &d.settings.update_option), ("bufferOption", &d.settings.buffer_option)]);
        state
            .client
            .post(&format!("streams/{}/value", stream.web_id), &q, &json!({ "Value": value }))
            .await
            .map_err(|e| match e {
                HttpError::Status { code, .. } if code < 500 => ConnectorError::Other(e.to_string()),
                other => transport(other),
            })?;
        Ok(CommandResult { point: request.point.clone(), value: request.value.clone(), raw: request.raw.clone() })
    }

    async fn disconnect(&mut self) -> Result<(), ConnectorError> {
        for (_, state) in self.states.drain() {
            if let Some(p) = state.push {
                p.stop();
            }
        }
        self.clients.clear();
        Ok(())
    }
}
