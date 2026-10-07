//! OPC-UA connector module.
//!
//! Implements the [`Connector`](tedge_dot_sdk::Connector) trait against
//! [`async-opcua`]. Like the Modbus reference module the driver stays "dumb": it connects, reads
//! node values (polling), writes node values, and pushes data-change notifications
//! (`subscribe`, via one OPC-UA subscription per device with one monitored item per point).
//! All scaling, renaming, units, alarms and thin-edge JSON shaping are handled by
//! thin-edge.io flows.
//!
//! Addressing uses OPC-UA `NodeId`s (textual `ns=2;s=Temperature` or structured
//! `namespace`+`identifier`) instead of Modbus register tables, exercising the contract's opaque
//! address slots with a very different protocol.

mod config;
pub mod pki;
pub mod pki_cli;
mod privilege;
mod resolve;
pub mod structure;
pub mod security;

pub use config::{
    DeviceSecurity, Identity, NodeAddress, OpcuaConnection, OpcuaEndpoint, Policy, SecurityMode,
};

use async_trait::async_trait;
use std::collections::HashMap;
use std::str::FromStr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};
use tedge_dot_sdk::{
    Access, Capabilities, CommandRequest, CommandResult, ConfigError, Connector, ConnectorConfig,
    ConnectorError, DataType, DeviceId, LinkReport, LinkStatus, Mode, PointRef, Quality, Sample,
    SampleSink, Transform, Value,
};
use time::OffsetDateTime;

use futures::StreamExt;
use opcua::client::{
    ClientBuilder, DataChangeCallback, IdentityToken, Session, SessionPollResult,
    SubscriptionActivity,
};
use opcua::crypto::{CertificateStore, PrivateKey, SecurityPolicy, X509};
use opcua::types::{
    AttributeId, BinaryEncodable, DataValue, MessageSecurityMode, MonitoredItemCreateRequest,
    MonitoringMode, MonitoringParameters, NodeId, NumericRange, ReadValueId, StatusCode,
    TimestampsToReturn, UAString, UserTokenPolicy, Variant, WriteValue,
};
use pki::{OwnCertificate, Pki};
use tracing::warn;

const PROTOCOL: &str = "opcua";

/// Sampling-interval fallback for a monitored item without one. Unreachable from the runtime,
/// which always resolves a pushed point's sampling interval (`sampling_interval`, falling back
/// to `poll_interval`); kept for modules driven directly, as by the CLI.
const DEFAULT_SAMPLING_INTERVAL: Duration = Duration::from_millis(500);

/// Largest integer representable exactly as an `f64` (JS `Number.MAX_SAFE_INTEGER`).
const MAX_SAFE_INT: i64 = 9_007_199_254_740_991;

/// A fully-resolved OPC-UA point (node id + decode parameters), built in `configure`.
#[derive(Clone)]
struct OpcuaPoint {
    node_id: NodeId,
    /// `address.field`: the path into a structured value (openspec `opcua-custom-datatypes`).
    field: Option<Vec<structure::Segment>>,
    /// `address.index`: one element of an array value.
    index: Option<u32>,
    mode: Mode,
    datatype: Option<DataType>,
    access: Access,
    unit: Option<String>,
    transform: Transform,
}

struct DeviceModel {
    endpoint: OpcuaEndpoint,
    security: DeviceSecurity,
    /// The X.509 user identity, loaded at configure.
    user_x509: Option<(X509, PrivateKey)>,
    points: HashMap<String, OpcuaPoint>,
}

/// How long closing a session waits for the server to answer (see [`close_session`]).
const CLOSE_SESSION_TIMEOUT: Duration = Duration::from_secs(2);

/// A live session, what its event loop last reported, and the task driving that loop.
struct SessionHandle {
    session: Arc<Session>,
    health: Arc<Mutex<SessionHealth>>,
    event_loop: AbortOnDrop,
    /// What this session resolved for structured values; replaced at every connect.
    types: Arc<resolve::SessionTypes>,
}

/// A spawned task aborted when its handle is dropped. The runtime cancels a module call that
/// outlives `operation_timeout`; a plain `JoinHandle` dropped with the cancelled future would
/// leave the session's event loop running, free to activate an orphaned session on a server
/// that answers later and to keep it alive there.
struct AbortOnDrop(tokio::task::JoinHandle<()>);

impl AbortOnDrop {
    fn abort(&self) {
        self.0.abort();
    }
}

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
    }
}

/// What a session's event loop last reported, read back by `check_subscription`.
///
/// async-opcua hides an outage from its caller in two ways. It re-establishes a dropped session
/// on its own only `session_retry_limit` times and then ends the event loop, silently; and it
/// does not notice a server that stops answering at all (failed keep-alives are not counted by
/// default). A device whose points are all pushed makes no reads that would fail instead, so
/// without this record such a device goes quiet for good behind a `connected` link.
struct SessionHealth {
    /// A session is active: set on every (re)connect, cleared when the transport drops.
    connected: bool,
    /// Why the session is not usable, for the link status.
    reason: Option<String>,
    /// The status code of the last failed (re)connect, to categorise security failures.
    last_status: Option<StatusCode>,
    /// The last publish response (a notification or a keep-alive), or the last (re)connect. A
    /// server with a live subscription answers at least once per keep-alive window.
    last_publish: Instant,
}

fn lock_health(health: &Mutex<SessionHealth>) -> MutexGuard<'_, SessionHealth> {
    health.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// A live push subscription for one device: how many monitored items it was created with, and
/// the forwarder task bridging data-change callbacks into the runtime's sample channel.
struct SubscriptionHandle {
    items: usize,
    forwarder: tokio::task::JoinHandle<()>,
}

/// The OPC-UA connector. One instance manages all configured OPC-UA servers.
#[derive(Default)]
pub struct OpcuaConnector {
    conn: OpcuaConnection,
    /// The PKI directory (resolved against the configuration file).
    pki_root: std::path::PathBuf,
    /// The application instance certificate, when a device is secured: loaded (or generated)
    /// at configure, its failure reported per secured device.
    own: Option<Result<OwnCertificate, String>>,
    /// When the expiry of `own` was last warned about.
    expiry_warned: Option<Instant>,
    devices: HashMap<String, DeviceModel>,
    sessions: HashMap<String, SessionHandle>,
    subscriptions: HashMap<String, SubscriptionHandle>,
}

impl OpcuaConnector {
    /// Connect one device and describe the outcome as its link report.
    async fn connect_one(&mut self, name: &str) -> LinkReport {
        self.warn_expiry();
        let dev = &self.devices[name];
        let attempt = Attempt {
            conn: &self.conn,
            endpoint: &dev.endpoint,
            security: &dev.security,
            user_x509: dev.user_x509.as_ref(),
            own: self.own.as_ref(),
            pki_root: &self.pki_root,
        };
        let (result, info) = attempt.connect().await;
        match result {
            Ok(mut handle) => {
                handle.types = Arc::new(self.resolve_types(name, &handle.session).await);
                self.sessions.insert(name.to_string(), handle);
                LinkReport {
                    device: name.to_string(),
                    status: LinkStatus::Connected,
                    reason: None,
                    info: Some(info),
                }
            }
            Err(reason) => LinkReport {
                device: name.to_string(),
                status: LinkStatus::Disconnected,
                reason: Some(reason),
                info: Some(info),
            },
        }
    }

    /// Resolve, for a new session, what the device's structure and raw points need: the
    /// namespace array and the data type definitions (design D3, D6). Bounded, so a server that
    /// stops answering cannot stall the connect; its field points then report why.
    async fn resolve_types(&self, name: &str, session: &Session) -> resolve::SessionTypes {
        let wanted: Vec<resolve::Wanted> = self.devices[name]
            .points
            .iter()
            .filter(|(_, p)| p.field.is_some() || p.mode == Mode::Raw)
            .map(|(id, p)| resolve::Wanted {
                point: id.clone(),
                node: p.node_id.clone(),
                field: p.field.clone(),
                index: p.index,
                datatype: p.datatype,
            })
            .collect();
        let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1) * 4);
        match tokio::time::timeout(timeout, resolve::resolve(session, &wanted)).await {
            Ok(types) => {
                for (point, plan) in &types.plans {
                    if let Err(e) = plan {
                        warn!("device '{name}' point '{point}': {e}");
                    }
                }
                types
            }
            Err(_) => {
                let reason = format!("{}: timed out after {}s", resolve::UNAVAILABLE, timeout.as_secs());
                warn!("device '{name}': {reason}");
                resolve::SessionTypes {
                    plans: wanted.iter().map(|w| (w.point.clone(), Err(reason.clone()))).collect(),
                    ..Default::default()
                }
            }
        }
    }

    /// Warn (at most daily) while the application certificate is close to or past its expiry.
    fn warn_expiry(&mut self) {
        let Some(Ok(own)) = &self.own else { return };
        if self
            .expiry_warned
            .is_some_and(|at| at.elapsed() < Duration::from_secs(86_400))
        {
            return;
        }
        let Ok(not_after) = own.certificate.not_after() else { return };
        let now = OffsetDateTime::now_utc().unix_timestamp();
        match security::expiry(not_after.timestamp(), now, pki::EXPIRY_WARNING_DAYS) {
            security::Expiry::Valid => return,
            security::Expiry::ExpiresSoon(days) => warn!(
                "the application certificate {} expires in {days} day(s), on {not_after}; renew it with `tedge-dot pki create --force`",
                own.certificate_path.display()
            ),
            security::Expiry::Expired => warn!(
                "the application certificate {} expired on {not_after}; secured devices cannot connect",
                own.certificate_path.display()
            ),
        }
        self.expiry_warned = Some(Instant::now());
    }
}

/// Load an X.509 user identity (certificate DER or PEM, key PEM). Errors name paths only.
fn load_user_certificate(cert: &std::path::Path, key: &std::path::Path) -> Result<(X509, PrivateKey), String> {
    let bytes = std::fs::read(cert)
        .map_err(|e| format!("user_certificate '{}' cannot be read: {e}", cert.display()))?;
    let certificate = opcua::crypto::trust_list::parse_certificates(&bytes)
        .first()
        .and_then(|der| X509::from_der(der).ok())
        .ok_or_else(|| format!("user_certificate '{}' is not a certificate", cert.display()))?;
    let private_key = PrivateKey::read_pem_file(key)
        .map_err(|_| format!("user_private_key '{}' cannot be read as a PEM private key", key.display()))?;
    if !certificate.matches_private_key(&private_key) {
        return Err(format!(
            "user_private_key '{}' does not belong to user_certificate '{}'",
            key.display(),
            cert.display()
        ));
    }
    Ok((certificate, private_key))
}

/// Factory used by the binary to instantiate the module behind its feature flag.
pub fn factory() -> Box<dyn Connector> {
    Box::<OpcuaConnector>::default()
}

#[async_trait]
impl Connector for OpcuaConnector {
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError> {
        self.conn = OpcuaConnection::from_value(&config.connection).map_err(ConfigError::Invalid)?;
        let base_dir = config.base_dir.as_deref();
        self.pki_root = self.conn.pki_dir(base_dir);
        self.devices.clear();
        self.own = None;
        self.expiry_warned = None;

        for d in &config.devices {
            let endpoint: OpcuaEndpoint = serde_json::from_value(d.protocol_address.clone())
                .map_err(|e| {
                    ConfigError::Invalid(format!("device '{}' protocol_address: {e}", d.name))
                })?;
            let security = config::device_security(&self.conn, &endpoint, base_dir)
                .map_err(|e| ConfigError::Invalid(format!("device '{}': {e}", d.name)))?;
            let user_x509 = match &security.identity {
                Identity::X509 { certificate, private_key } => Some(
                    load_user_certificate(certificate, private_key)
                        .map_err(|e| ConfigError::Invalid(format!("device '{}': {e}", d.name)))?,
                ),
                _ => None,
            };

            let mut points = HashMap::new();
            for p in &d.points {
                let addr: NodeAddress = serde_json::from_value(p.address.clone()).map_err(|e| {
                    ConfigError::Invalid(format!("point '{}' address: {e}", p.id))
                })?;
                let node_id = node_id_from(&addr).map_err(|e| {
                    ConfigError::Invalid(format!("point '{}' address: {e}", p.id))
                })?;
                let mode = p.resolved_mode(d.default_mode);
                if mode == Mode::Typed && p.datatype.is_none() {
                    return Err(ConfigError::Invalid(format!(
                        "point '{}' is typed but has no datatype",
                        p.id
                    )));
                }
                let field = match &addr.field {
                    Some(path) => Some(structure::parse_path(path).map_err(|e| {
                        ConfigError::Invalid(format!("point '{}' address: {e}", p.id))
                    })?),
                    None => None,
                };
                if field.is_some() && mode == Mode::Raw {
                    return Err(ConfigError::Invalid(format!(
                        "point '{}': address.field selects a typed value; a raw point reads the whole structure body",
                        p.id
                    )));
                }
                let access = Access::parse(p.access.as_deref());
                if (field.is_some() || addr.index.is_some()) && access != Access::Read {
                    return Err(ConfigError::Invalid(format!(
                        "point '{}': points with address.field or address.index are read-only",
                        p.id
                    )));
                }
                points.insert(
                    p.id.clone(),
                    OpcuaPoint {
                        node_id,
                        field,
                        index: addr.index,
                        mode,
                        datatype: p.datatype,
                        access,
                        unit: p.unit.clone(),
                        transform: p.transform.unwrap_or_default(),
                    },
                );
            }
            self.devices
                .insert(d.name.clone(), DeviceModel { endpoint, security, user_x509, points });
        }

        // Only a configuration with a secured device needs (and may create) a certificate.
        if self.devices.values().any(|d| d.security.is_secure()) {
            let own = Pki::new(&self.pki_root).load_or_create_own(&self.conn, base_dir);
            match &own {
                Ok(own) if own.generated => tracing::info!(
                    "generated the application certificate {} (thumbprint {})",
                    own.certificate_path.display(),
                    pki::thumbprint(&own.certificate.to_der().unwrap_or_default())
                ),
                Ok(_) => {}
                Err(e) => warn!("application certificate: {e}"),
            }
            self.own = Some(own);
            self.warn_expiry();
        }
        Ok(())
    }

    fn local_only_settings(&self) -> &'static [&'static str] {
        config::LOCAL_ONLY_SETTINGS
    }

    fn capabilities(&self) -> Capabilities {
        Capabilities {
            protocol: PROTOCOL,
            version: env!("CARGO_PKG_VERSION"),
            modes: vec![Mode::Raw, Mode::Typed],
            datatypes: vec![
                DataType::Bool,
                DataType::Int8,
                DataType::Uint8,
                DataType::Int16,
                DataType::Uint16,
                DataType::Int32,
                DataType::Uint32,
                DataType::Int64,
                DataType::Uint64,
                DataType::Float32,
                DataType::Float64,
                DataType::String,
                DataType::Bytes,
            ],
            point_kinds: vec!["variable".into()],
            command_verbs: vec!["write".into()],
            features: vec!["polling".into(), "subscribe".into()],
            subscribe: true,
        }
    }

    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError> {
        let names: Vec<String> = self.devices.keys().cloned().collect();
        let mut reports = Vec::new();
        for name in names {
            let report = self.connect_one(&name).await;
            reports.push(report);
        }
        Ok(reports)
    }

    async fn read_points(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
    ) -> Result<Vec<Sample>, ConnectorError> {
        // Resolve per-point models first, ending the borrow on `self.devices`.
        let models: Vec<(String, Option<OpcuaPoint>)> = match self.devices.get(device) {
            Some(dev) => points
                .iter()
                .map(|p| (p.id.clone(), dev.points.get(&p.id).cloned()))
                .collect(),
            None => points.iter().map(|p| (p.id.clone(), None)).collect(),
        };

        let (session, types) = match self.sessions.get(device) {
            Some(h) => (h.session.clone(), h.types.clone()),
            None => {
                return Ok(models
                    .into_iter()
                    .map(|(id, model)| {
                        bad_sample(&id, model.as_ref(), "device not connected")
                    })
                    .collect());
            }
        };

        // Build the read request for the known points (skip unknown ones, reported separately).
        // Points on the same node and element share one read (design D5).
        let mut known: Vec<(String, OpcuaPoint, usize)> = Vec::new();
        let mut reads: Vec<ReadValueId> = Vec::new();
        let mut slots: HashMap<(NodeId, Option<u32>), usize> = HashMap::new();
        let mut out: Vec<Sample> = Vec::new();
        for (id, model) in models {
            match model {
                Some(m) => {
                    let slot = *slots.entry((m.node_id.clone(), m.index)).or_insert_with(|| {
                        reads.push(ReadValueId {
                            node_id: m.node_id.clone(),
                            attribute_id: AttributeId::Value as u32,
                            index_range: index_range(m.index),
                            data_encoding: Default::default(),
                        });
                        reads.len() - 1
                    });
                    known.push((id, m, slot));
                }
                None => out.push(bad_sample(&id, None, "unknown point")),
            }
        }

        if !reads.is_empty() {
            // Bound the service call: while the transport is down the client queues requests
            // waiting to resurrect the session, and an unbounded await here would wedge the
            // runtime's poll loop instead of reporting the outage.
            let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
            match tokio::time::timeout(
                timeout,
                session.read(&reads, TimestampsToReturn::Neither, 0.0),
            )
            .await
            {
                Ok(Ok(values)) => {
                    for (id, model, slot) in &known {
                        let Some(dv) = values.get(*slot) else {
                            out.push(bad_sample(id, Some(model), "no value returned"));
                            continue;
                        };
                        let mut sample = build_sample(id, model, dv, &types);
                        // Contract §5: polled samples carry the read-completion time. Servers
                        // may return a (stale) source timestamp even for TimestampsToReturn::
                        // Neither; only the push path reports event time.
                        sample.ts = OffsetDateTime::now_utc();
                        out.push(sample);
                    }
                }
                Ok(Err(status)) => {
                    for (id, model, _) in &known {
                        out.push(bad_sample(id, Some(model), &format!("read failed: {status}")));
                    }
                }
                Err(_) => {
                    let reason =
                        format!("read timed out after {}s (transport down?)", timeout.as_secs());
                    for (id, model, _) in &known {
                        out.push(bad_sample(id, Some(model), &reason));
                    }
                }
            }
        }
        Ok(out)
    }

    async fn subscribe(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
        sink: SampleSink,
    ) -> Result<(), ConnectorError> {
        if points.is_empty() {
            return Ok(());
        }
        let dev = self
            .devices
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;

        // Resolve the requested points and their sampling intervals up front.
        let mut items: Vec<(String, OpcuaPoint, Duration)> = Vec::with_capacity(points.len());
        for r in points {
            let model = dev.points.get(&r.id).cloned().ok_or_else(|| {
                ConnectorError::UnknownPoint {
                    device: device.clone(),
                    point: r.id.clone(),
                }
            })?;
            items.push((
                r.id.clone(),
                model,
                r.interval.unwrap_or(DEFAULT_SAMPLING_INTERVAL),
            ));
        }

        let handle = self
            .sessions
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        let (session, types) = (handle.session.clone(), handle.types.clone());

        // Notifications are matched back to points by node id and element: several points may
        // share one monitored item (design D5).
        let mut by_node: HashMap<(NodeId, Option<u32>), Vec<(String, OpcuaPoint)>> = HashMap::new();
        let mut monitored: Vec<(NodeId, Option<u32>, Duration)> = Vec::new();
        for (id, model, interval) in &items {
            let key = (model.node_id.clone(), model.index);
            match monitored.iter_mut().find(|(n, i, _)| (n, i) == (&key.0, &key.1)) {
                Some(item) => item.2 = item.2.min(*interval),
                None => monitored.push((key.0.clone(), key.1, *interval)),
            }
            by_node.entry(key).or_default().push((id.clone(), model.clone()));
        }

        // The data-change callback is synchronous, so it forwards through an unbounded channel
        // to a spawned task that awaits the runtime's (bounded) sink.
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Sample>();
        let device_name = device.clone();
        let callback = DataChangeCallback::new(move |dv, item| {
            let watched = item.item_to_monitor();
            let node_id = &watched.node_id;
            tracing::trace!(%node_id, "data change notification");
            let index = match watched.index_range {
                NumericRange::Index(i) => Some(i),
                _ => None,
            };
            if let Some(models) = by_node.get(&(node_id.clone(), index)) {
                for (id, model) in models {
                    let mut sample = build_sample(id, model, &dv, &types);
                    sample.device = device_name.clone();
                    // Ignore send errors: the forwarder has already shut down.
                    let _ = tx.send(sample);
                }
            } else {
                tracing::debug!(%node_id, "data change for unrequested node id, dropped");
            }
        });

        // One subscription per device, publishing at the fastest requested sampling rate.
        let publishing_interval =
            publishing_interval_for(items.iter().map(|(_, _, interval)| *interval));
        let subscription_id = session
            .create_subscription(publishing_interval, 60, 20, 0, 0, true, callback)
            .await
            .map_err(|s| ConnectorError::Transport(format!("create_subscription failed: {s}")))?;

        // One monitored item per node and element, sampled at the fastest effective sampling
        // interval of its points.
        let requests: Vec<MonitoredItemCreateRequest> = monitored
            .iter()
            .map(|(node_id, index, interval)| {
                let mut watch: ReadValueId = node_id.clone().into();
                watch.index_range = index_range(*index);
                MonitoredItemCreateRequest::new(
                    watch,
                    MonitoringMode::Reporting,
                    MonitoringParameters {
                        sampling_interval: interval.as_millis() as f64,
                        queue_size: 1,
                        discard_oldest: true,
                        ..Default::default()
                    },
                )
            })
            .collect();
        let results = match session
            .create_monitored_items(subscription_id, TimestampsToReturn::Both, requests)
            .await
        {
            Ok(results) => results,
            Err(status) => {
                let _ = session.delete_subscription(subscription_id).await;
                return Err(ConnectorError::Transport(format!(
                    "create_monitored_items failed: {status}"
                )));
            }
        };
        // All-or-nothing per device: on any rejected item, drop the subscription so the runtime
        // keeps every point of this device on the polling schedule.
        let failed: Vec<String> = monitored
            .iter()
            .zip(&results)
            .filter(|(_, r)| !r.result.status_code.is_good())
            .map(|((node, index, _), r)| {
                let ids: Vec<&str> = items
                    .iter()
                    .filter(|(_, m, _)| (&m.node_id, &m.index) == (node, index))
                    .map(|(id, _, _)| id.as_str())
                    .collect();
                format!("{}: {}", ids.join(", "), r.result.status_code)
            })
            .collect();
        if !failed.is_empty() {
            let _ = session.delete_subscription(subscription_id).await;
            return Err(ConnectorError::Transport(format!(
                "monitored items rejected: {}",
                failed.join(", ")
            )));
        }
        // The server may grant other rates than requested (a sampling interval below what it
        // supports, zero included). Say so, so a "200ms" that behaves like "1s" is explained.
        let granted = {
            let state = session.subscription_state.lock();
            state.get(subscription_id).map(|s| s.publishing_interval())
        };
        if let Some(revised) = granted.and_then(|g| revised(publishing_interval, g.as_secs_f64() * 1000.0)) {
            tracing::info!(
                device = %device, requested = ?publishing_interval, revised = ?revised,
                "the server revised the subscription's publishing interval"
            );
        }
        for ((node, index, interval), r) in monitored.iter().zip(&results) {
            if let Some(revised) = revised(*interval, r.result.revised_sampling_interval) {
                let ids: Vec<&str> = items
                    .iter()
                    .filter(|(_, m, _)| (&m.node_id, &m.index) == (node, index))
                    .map(|(id, _, _)| id.as_str())
                    .collect();
                tracing::info!(
                    device = %device, point = %ids.join(", "), requested = ?interval, revised = ?revised,
                    "the server revised the point's sampling interval"
                );
            }
        }

        // Forward pushed samples into the runtime sink; exit cleanly when either side closes.
        let forwarder = tokio::spawn(async move {
            while let Some(sample) = rx.recv().await {
                if sink.send(sample).await.is_err() {
                    break; // runtime dropped the sink (shutdown or reload)
                }
            }
        });
        // Start the keep-alive clock at the subscription rather than at a connect that may be long
        // past: until the first publish response arrives, this is the last sign of life.
        if let Some(handle) = self.sessions.get(device) {
            lock_health(&handle.health).last_publish = Instant::now();
        }
        let replaced = self.subscriptions.insert(
            device.clone(),
            SubscriptionHandle {
                // Monitored items, not points: points on one node and element share an item.
                items: monitored.len(),
                forwarder,
            },
        );
        if let Some(old) = replaced {
            old.forwarder.abort();
        }
        Ok(())
    }

    async fn check_subscription(&mut self, device: &DeviceId) -> Result<(), ConnectorError> {
        let handle = self
            .sessions
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        let expected = self
            .subscriptions
            .get(device)
            .map(|sub| sub.items)
            .ok_or_else(|| ConnectorError::Transport("no subscription armed".into()))?;
        let (connected, reason, since_publish) = {
            let health = lock_health(&handle.health);
            (health.connected, health.reason.clone(), health.last_publish.elapsed())
        };
        if !connected {
            return Err(ConnectorError::Transport(
                reason.unwrap_or_else(|| "session is not connected".into()),
            ));
        }
        // After reconnecting within its retry limit the client re-creates subscriptions itself,
        // but it does not report a re-creation that failed: the subscription, or its monitored
        // items, is then simply missing from the session. There is one subscription per session,
        // so the first one is ours, whatever id a re-creation gave it.
        let live = {
            let state = handle.session.subscription_state.lock();
            state
                .subscription_ids()
                .and_then(|ids| ids.first().and_then(|id| state.get(*id)))
                .map(|s| {
                    (
                        s.monitored_items().count(),
                        s.publishing_interval(),
                        s.max_keep_alive_count(),
                    )
                })
        };
        let Some((monitored, publishing_interval, keep_alive_count)) = live else {
            return Err(ConnectorError::Transport("subscription lost with the session".into()));
        };
        if monitored < expected {
            return Err(ConnectorError::Transport(format!(
                "subscription lost monitored items ({monitored} of {expected} left)"
            )));
        }
        // A server answers a publish request at least once per keep-alive window (the revised
        // one). On top, allow one publishing interval for the client's own request schedule and
        // one request timeout for the answer to arrive.
        let window = publishing_interval.saturating_mul(keep_alive_count.max(1))
            + publishing_interval
            + Duration::from_secs(self.conn.request_timeout_s.max(1));
        if since_publish > window {
            return Err(ConnectorError::Transport(format!(
                "no publish response for {}s (keep-alive window {}s); server not answering?",
                since_publish.as_secs(),
                window.as_secs()
            )));
        }
        Ok(())
    }

    async fn reconnect(&mut self, device: &DeviceId) -> Result<LinkReport, ConnectorError> {
        if !self.devices.contains_key(device) {
            return Err(ConnectorError::Other(format!("unknown device '{device}'")));
        }
        // Tear down the old session first. It is not necessarily dead -- a reconnect also follows
        // reads failing at application level, or a server that stopped publishing -- so it is
        // closed on the server when that is still possible (see `close_session`).
        if let Some(sub) = self.subscriptions.remove(device) {
            sub.forwarder.abort();
        }
        if let Some(handle) = self.sessions.remove(device) {
            close_session(handle).await;
        }
        Ok(self.connect_one(device).await)
    }

    async fn execute(
        &mut self,
        device: &DeviceId,
        verb: &str,
        request: &CommandRequest,
    ) -> Result<CommandResult, ConnectorError> {
        if verb != "write" {
            return Err(ConnectorError::Unsupported(verb.to_string()));
        }
        let model = self
            .devices
            .get(device)
            .and_then(|d| d.points.get(&request.point))
            .cloned()
            .ok_or_else(|| ConnectorError::UnknownPoint {
                device: device.clone(),
                point: request.point.clone(),
            })?;

        if !model.access.can_write() {
            return Err(ConnectorError::AccessDenied(request.point.clone()));
        }
        let datatype = model
            .datatype
            .ok_or_else(|| ConnectorError::Decode("write requires a point datatype".into()))?;
        let value = request
            .value
            .as_ref()
            .ok_or_else(|| ConnectorError::Decode("write requires a value".into()))?;
        let variant = build_variant(datatype, value)
            .map_err(ConnectorError::Decode)?;

        let session = self
            .sessions
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?
            .session
            .clone();

        let write = WriteValue {
            node_id: model.node_id.clone(),
            attribute_id: AttributeId::Value as u32,
            index_range: Default::default(),
            value: DataValue {
                value: Some(variant),
                ..Default::default()
            },
        };
        let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
        let results = tokio::time::timeout(timeout, session.write(&[write]))
            .await
            .map_err(|_| {
                ConnectorError::Transport(format!(
                    "write timed out after {}s (transport down?)",
                    timeout.as_secs()
                ))
            })?
            .map_err(|s| ConnectorError::Transport(format!("write failed: {s}")))?;
        let status = results.first().copied().unwrap_or(StatusCode::Good);
        if !status.is_good() {
            return Err(ConnectorError::Transport(format!("write rejected: {status}")));
        }
        Ok(CommandResult {
            point: request.point.clone(),
            value: request.value.clone(),
            raw: request.raw.clone(),
        })
    }

    async fn disconnect(&mut self) -> Result<(), ConnectorError> {
        // Abort the forwarders first, so no stale task keeps pushing into an old sink after a
        // config reload re-subscribes. Closing a session deletes its server-side subscription.
        for (_, sub) in self.subscriptions.drain() {
            sub.forwarder.abort();
        }
        for (_, handle) in self.sessions.drain() {
            close_session(handle).await;
        }
        Ok(())
    }
}

/// Build a `NodeId` from the configured address (textual or structured).
fn node_id_from(addr: &NodeAddress) -> Result<NodeId, String> {
    if let Some(text) = &addr.node_id {
        return NodeId::from_str(text).map_err(|_| format!("invalid node_id '{text}'"));
    }
    let ns = addr
        .namespace
        .ok_or_else(|| "missing 'node_id' or 'namespace'/'identifier'".to_string())?;
    match &addr.identifier {
        Some(serde_json::Value::String(s)) => Ok(NodeId::new(ns, s.clone())),
        Some(serde_json::Value::Number(n)) => {
            let i = n
                .as_u64()
                .ok_or_else(|| "numeric identifier must be a non-negative integer".to_string())?;
            Ok(NodeId::new(ns, i as u32))
        }
        _ => Err("missing or invalid 'identifier'".to_string()),
    }
}

/// Everything one connect attempt needs.
struct Attempt<'a> {
    conn: &'a OpcuaConnection,
    endpoint: &'a OpcuaEndpoint,
    security: &'a DeviceSecurity,
    user_x509: Option<&'a (X509, PrivateKey)>,
    own: Option<&'a Result<OwnCertificate, String>>,
    pki_root: &'a std::path::Path,
}

impl Attempt<'_> {
    /// Connect to one OPC-UA server endpoint and wait for the session to activate. Returns the
    /// link `info` either way (spec §8): the endpoint, the effective policy and mode and, once
    /// known, the server certificate's thumbprint and whether it was verified.
    async fn connect(&self) -> (Result<SessionHandle, String>, serde_json::Value) {
        let mut info = serde_json::json!({
            "endpoint": self.endpoint.endpoint,
            "security_policy": self.security.policy.name(),
            "security_mode": self.security.mode.name(),
        });
        let result = self.try_connect(&mut info).await;
        (result, info)
    }

    async fn try_connect(&self, info: &mut serde_json::Value) -> Result<SessionHandle, String> {
        let security = self.security;
        let secure = security.is_secure();
        let url = self.endpoint.endpoint.as_str();

        let own = if secure {
            let own = match self.own {
                Some(Ok(own)) => own,
                Some(Err(e)) => return Err(format!("{} {e}", security::APPLICATION_CERTIFICATE)),
                None => {
                    return Err(format!(
                        "{} none loaded",
                        security::APPLICATION_CERTIFICATE
                    ))
                }
            };
            if let Ok(not_after) = own.certificate.not_after() {
                let now = OffsetDateTime::now_utc().unix_timestamp();
                if security::expiry(not_after.timestamp(), now, 0) == security::Expiry::Expired {
                    return Err(format!(
                        "{} {} expired on {not_after}",
                        security::APPLICATION_CERTIFICATE,
                        own.certificate_path.display()
                    ));
                }
            }
            Some(own)
        } else {
            None
        };

        let mut builder = ClientBuilder::new()
            .application_name(self.conn.application_name.clone())
            .application_uri(self.conn.application_uri.clone())
            .pki_dir(self.pki_root)
            .trust_server_certs(security.trust_any_server_certificate)
            .create_sample_keypair(false)
            .session_retry_limit(3);
        if let Some(own) = own {
            builder = builder
                .certificate_path(&own.certificate_path)
                .private_key_path(&own.private_key_path);
        }
        let mut client = builder
            .client()
            .map_err(|e| format!("client build failed: {e:?}"))?;

        let identity = match (&security.identity, self.user_x509) {
            (Identity::UserName { user, password }, _) => {
                IdentityToken::UserName(user.clone(), password.expose().to_string().into())
            }
            (Identity::X509 { .. }, Some((cert, key))) => {
                IdentityToken::X509(Box::new(cert.clone()), Box::new(key.clone()))
            }
            _ => IdentityToken::Anonymous,
        };
        let connect_timeout = Duration::from_secs(self.conn.connect_timeout_s.max(1));

        // An anonymous session without message security needs nothing endpoint discovery
        // provides (no server certificate, no token policy), so it dials the configured address
        // directly -- which also keeps working with servers whose discovery answers are broken.
        // Everything else discovers, then still dials the configured address (see
        // `security::select_endpoint`): industrial servers routinely sit behind NAT/gateways
        // and advertise endpoint URLs the client cannot reach.
        let (session, event_loop, server_verified) =
            if !secure && security.identity == Identity::Anonymous {
                let desc = (
                    url,
                    SecurityPolicy::None.to_str(),
                    MessageSecurityMode::None,
                    UserTokenPolicy::anonymous(),
                );
                let (session, event_loop) = client
                    .connect_to_endpoint_directly(desc, identity)
                    .map_err(|e| format!("connect failed: {e}"))?;
                (session, event_loop, false)
            } else {
                let endpoints = tokio::time::timeout(
                    connect_timeout,
                    client.get_server_endpoints_from_url(url),
                )
                .await
                .map_err(|_| {
                    format!(
                        "timed out after {}s asking the server for its endpoints",
                        connect_timeout.as_secs()
                    )
                })?
                .map_err(|e| format!("connect failed: {e}"))?;
                let chosen = security::select_endpoint(&endpoints, security, url)?;

                if matches!(security.identity, Identity::UserName { .. })
                    && security::password_in_plaintext(&chosen)
                {
                    if !security.allow_plaintext_password {
                        return Err(format!(
                            "{} the password would be readable by the server's endpoint without an authenticated server certificate, or in clear (a channel without message security, or a signed-only channel whose token policy is None); use sign_and_encrypt or set allow_plaintext_password = true",
                            security::PLAINTEXT_PASSWORD_REFUSED
                        ));
                    }
                    warn!(endpoint = url, "sending the password unencrypted (allow_plaintext_password)");
                }

                let verified = if secure {
                    self.check_server_certificate(&chosen, info)?
                } else {
                    false
                };
                let (session, event_loop) = client
                    .connect_to_endpoint_directly(chosen, identity)
                    .map_err(|e| format!("connect failed: {e}"))?;
                (session, event_loop, verified)
            };

        let health = Arc::new(Mutex::new(SessionHealth {
            connected: false,
            reason: None,
            last_status: None,
            last_publish: Instant::now(),
        }));
        let handle = AbortOnDrop(tokio::spawn(drive_session(event_loop.enter(), health.clone())));
        let failed = wait_for_security_failure(&health);
        let outcome = tokio::time::timeout(connect_timeout, async {
            tokio::select! {
                connected = session.wait_for_connection() => connected,
                () = failed => false,
            }
        })
        .await;
        match outcome {
            Ok(true) => {
                // The event loop reports the connect as well, but a check made right after this
                // returns can run before that report is recorded. A loss recorded since keeps its
                // reason, and wins.
                {
                    let mut state = lock_health(&health);
                    if state.reason.is_none() {
                        state.connected = true;
                    }
                }
                Ok(SessionHandle {
                    session,
                    health,
                    event_loop: handle,
                    types: Arc::default(),
                })
            }
            Ok(false) => {
                handle.abort();
                let status = lock_health(&health).last_status;
                Err(self.failure_reason(status, server_verified, own))
            }
            Err(_) => {
                handle.abort();
                let status = lock_health(&health).last_status;
                match status.and_then(security::category) {
                    Some(_) => Err(self.failure_reason(status, server_verified, own)),
                    None => Err(format!(
                        "timed out after {}s waiting for connection",
                        connect_timeout.as_secs()
                    )),
                }
            }
        }
    }

    /// Validate the certificate the chosen endpoint advertises against the PKI directory
    /// before dialling, recording its thumbprint in `info`. `Ok(true)`: verified and trusted;
    /// `Ok(false)`: accepted unverified (`trust_any_server_certificate`).
    fn check_server_certificate(
        &self,
        chosen: &opcua::types::EndpointDescription,
        info: &mut serde_json::Value,
    ) -> Result<bool, String> {
        let cert = X509::from_byte_string(&chosen.server_certificate).map_err(|_| {
            format!(
                "{} the server advertises no usable certificate",
                security::CERTIFICATE_INVALID
            )
        })?;
        let der = cert.to_der().unwrap_or_default();
        let thumbprint = pki::thumbprint(&der);
        info["server_thumbprint"] = thumbprint.clone().into();
        // The key length is a property of the security policy (OPC UA Part 7), not a trust
        // decision, so it is checked before both the trust store AND
        // `trust_any_server_certificate`: that option says "do not judge who this server is",
        // not "use a key the policy forbids". open62541 enforces this in the policy itself and
        // refuses whatever the trust settings say, so skipping it here would also mean the two
        // implementations disagree about which servers are usable.
        //
        // It has to come before the trust store for a second reason: async-opcua's validator
        // logs the real cause but returns a plain BadCertificateUntrusted for a short key, so
        // the reason would tell the operator to run `tedge-dot pki trust` -- which they may
        // already have done, and which cannot help, because trusting a certificate does not
        // make its key longer.
        if let Some((min, max)) = self.security.policy.key_bits() {
            // An unreadable key length is a refusal, not a pass: `X509::key_length()` only
            // parses RSA, so an EC certificate lands here, and every policy with a key range is
            // an RSA policy. Letting it through would also split the two builds -- the C side
            // reads the size with `mbedtls_pk_get_bitlen`, which answers for EC keys too and
            // refuses them on the range.
            let Ok(bits) = cert.key_length() else {
                return Err(format!(
                    "{} the server certificate's key is not RSA, which {} requires (thumbprint {thumbprint}, subject {})",
                    security::CERTIFICATE_INVALID,
                    self.security.policy.name(),
                    cert.subject_text()
                ));
            };
            if bits < min || bits > max {
                return Err(format!(
                    "{} the server certificate's key is {bits} bits, which {} does not allow (it requires {min}-{max}) (thumbprint {thumbprint}, subject {})",
                    security::CERTIFICATE_INVALID,
                    self.security.policy.name(),
                    cert.subject_text()
                ));
            }
        }
        if self.security.trust_any_server_certificate {
            warn!(
                endpoint = %self.endpoint.endpoint,
                "server certificate {thumbprint} is accepted without verification (trust_any_server_certificate)"
            );
            info["server_certificate"] = "not_verified".into();
            return Ok(false);
        }
        let host = security::url_host(&self.endpoint.endpoint).unwrap_or_default();
        let policy = SecurityPolicy::from_uri(&self.security.policy.uri());
        let store = CertificateStore::new(self.pki_root);
        if let Err(status) = store.validate_or_reject_application_instance_cert(
            &cert,
            policy,
            Some(&host),
            Some(chosen.server.application_uri.as_ref()),
        ) {
            let category = security::category(status).unwrap_or(security::CERTIFICATE_INVALID);
            let mut reason = format!(
                "{category} {} ({status}; thumbprint {thumbprint}, subject {})",
                security::describe(status),
                cert.subject_text()
            );
            if category == security::CERTIFICATE_UNTRUSTED {
                reason.push_str(&format!(
                    "; trust it with `tedge-dot pki trust {}`",
                    &thumbprint[..8]
                ));
            }
            return Err(reason);
        }
        info["server_certificate"] = "trusted".into();
        Ok(true)
    }

    /// The link reason for a session that did not activate.
    fn failure_reason(
        &self,
        status: Option<StatusCode>,
        server_verified: bool,
        own: Option<&OwnCertificate>,
    ) -> String {
        let own_thumbprint = own
            .and_then(|o| o.certificate.to_der().ok())
            .map(|d| pki::thumbprint(&d));
        // Reaching here means our own check of the server certificate is already behind us: it
        // passed, or `trust_any_server_certificate` skipped it. Either way a distrust reported
        // now is the server's judgement of us, not ours of it.
        security::session_failure_reason(
            status,
            server_verified || own.is_some(),
            own_thumbprint.as_deref(),
            self.security.identity.kind(),
        )
    }
}

/// Resolves once the event loop has recorded a failure that retrying cannot fix (a security
/// failure), so the attempt ends without waiting out the retries and the timeout.
async fn wait_for_security_failure(health: &Mutex<SessionHealth>) {
    loop {
        let status = lock_health(health).last_status;
        if status.and_then(security::category).is_some() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

/// Drive a session's event loop until it ends, recording what it reports in `health`.
async fn drive_session(
    events: impl futures::Stream<Item = Result<SessionPollResult, StatusCode>>,
    health: Arc<Mutex<SessionHealth>>,
) {
    futures::pin_mut!(events);
    loop {
        let event = events.next().await;
        let mut state = lock_health(&health);
        match event {
            Some(Ok(SessionPollResult::Reconnected(_))) => {
                state.connected = true;
                state.reason = None;
                state.last_publish = Instant::now();
            }
            Some(Ok(SessionPollResult::ConnectionLost(status))) => {
                state.connected = false;
                state.last_status = Some(status);
                state.reason = Some(format!("connection lost: {status}"));
            }
            Some(Ok(SessionPollResult::ReconnectFailed(status))) => {
                state.connected = false;
                state.last_status = Some(status);
                state.reason = Some(format!("reconnect failed: {status}"));
            }
            Some(Ok(SessionPollResult::Subscription(SubscriptionActivity::Publish))) => {
                state.last_publish = Instant::now();
            }
            Some(Ok(_)) => {}
            Some(Err(status)) => {
                state.connected = false;
                state.last_status = Some(status);
                state.reason = Some(format!("session gave up reconnecting: {status}"));
                return;
            }
            None => {
                state.connected = false;
                state.reason.get_or_insert_with(|| "session closed".into());
                return;
            }
        }
    }
}

/// End a session, closing it on the server first while that is still possible.
///
/// Servers cap concurrent sessions -- embedded PLC servers often at a handful -- and a session
/// dropped without CloseSession keeps its slot until its timeout expires. Reconnects run on a
/// backoff schedule also while the server is up (reads failing at application level, a
/// subscription that stopped publishing), so abandoning each old session piles them up until
/// the server refuses new ones (BadTooManySessions) and reconnecting fails until they expire.
/// Bounded, and skipped when the transport is known to be down, so a dead server cannot stall
/// the reconnect.
async fn close_session(handle: SessionHandle) {
    let connected = lock_health(&handle.health).connected;
    if connected {
        let _ = tokio::time::timeout(CLOSE_SESSION_TIMEOUT, handle.session.disconnect()).await;
    }
    handle.event_loop.abort();
}

/// Convert an OPC-UA `Variant` into the SDK value model plus a best-effort raw byte echo.
fn variant_to_value(v: &Variant) -> Option<(Value, DataType, Vec<u8>)> {
    Some(match v {
        Variant::Boolean(b) => (Value::Bool(*b), DataType::Bool, vec![*b as u8]),
        Variant::SByte(i) => (Value::Number(*i as f64), DataType::Int8, vec![*i as u8]),
        Variant::Byte(u) => (Value::Number(*u as f64), DataType::Uint8, vec![*u]),
        Variant::Int16(i) => (Value::Number(*i as f64), DataType::Int16, i.to_be_bytes().to_vec()),
        Variant::UInt16(u) => {
            (Value::Number(*u as f64), DataType::Uint16, u.to_be_bytes().to_vec())
        }
        Variant::Int32(i) => (Value::Number(*i as f64), DataType::Int32, i.to_be_bytes().to_vec()),
        Variant::UInt32(u) => {
            (Value::Number(*u as f64), DataType::Uint32, u.to_be_bytes().to_vec())
        }
        Variant::Int64(i) => (int64_value(*i), DataType::Int64, i.to_be_bytes().to_vec()),
        Variant::UInt64(u) => (uint64_value(*u), DataType::Uint64, u.to_be_bytes().to_vec()),
        Variant::Float(f) => (Value::Number(*f as f64), DataType::Float32, f.to_be_bytes().to_vec()),
        Variant::Double(d) => (Value::Number(*d), DataType::Float64, d.to_be_bytes().to_vec()),
        Variant::String(s) => {
            let t = s.as_ref().to_string();
            let raw = t.clone().into_bytes();
            (Value::Text(t), DataType::String, raw)
        }
        _ => return None,
    })
}

/// 64-bit signed: keep as a number while exactly representable, else stringify.
fn int64_value(i: i64) -> Value {
    if i.abs() <= MAX_SAFE_INT {
        Value::Number(i as f64)
    } else {
        Value::Text(i.to_string())
    }
}

/// 64-bit unsigned: keep as a number while exactly representable, else stringify.
fn uint64_value(u: u64) -> Value {
    if u <= MAX_SAFE_INT as u64 {
        Value::Number(u as f64)
    } else {
        Value::Text(u.to_string())
    }
}

/// Build an OPC-UA `Variant` for a write, coercing the JSON value to the point's datatype.
fn build_variant(dt: DataType, value: &serde_json::Value) -> Result<Variant, String> {
    let num_err = || format!("value {value} is not valid for datatype {dt:?}");
    Ok(match dt {
        DataType::Bool => Variant::Boolean(value.as_bool().ok_or_else(num_err)?),
        DataType::Int8 => Variant::SByte(value.as_i64().ok_or_else(num_err)? as i8),
        DataType::Uint8 => Variant::Byte(value.as_u64().ok_or_else(num_err)? as u8),
        DataType::Int16 => Variant::Int16(value.as_i64().ok_or_else(num_err)? as i16),
        DataType::Uint16 => Variant::UInt16(value.as_u64().ok_or_else(num_err)? as u16),
        DataType::Int32 => Variant::Int32(value.as_i64().ok_or_else(num_err)? as i32),
        DataType::Uint32 => Variant::UInt32(value.as_u64().ok_or_else(num_err)? as u32),
        DataType::Int64 => Variant::Int64(int_from_json(value).ok_or_else(num_err)?),
        DataType::Uint64 => Variant::UInt64(uint_from_json(value).ok_or_else(num_err)?),
        DataType::Float32 => Variant::Float(value.as_f64().ok_or_else(num_err)? as f32),
        DataType::Float64 => Variant::Double(value.as_f64().ok_or_else(num_err)?),
        DataType::String => Variant::String(UAString::from(
            value.as_str().ok_or_else(num_err)?.to_string(),
        )),
        other => return Err(format!("datatype {other:?} is not writable over OPC-UA")),
    })
}

/// Accept a JSON number or a numeric string for 64-bit integers (outside JS safe range).
fn int_from_json(value: &serde_json::Value) -> Option<i64> {
    value
        .as_i64()
        .or_else(|| value.as_str().and_then(|s| s.parse::<i64>().ok()))
}

fn uint_from_json(value: &serde_json::Value) -> Option<u64> {
    value
        .as_u64()
        .or_else(|| value.as_str().and_then(|s| s.parse::<u64>().ok()))
}

/// Sample timestamp for a `DataValue`: prefer the source timestamp, then the server timestamp,
/// then "now" (polled reads request no timestamps, so they always fall back to "now").
fn data_value_ts(dv: &DataValue) -> OffsetDateTime {
    dv.source_timestamp
        .as_ref()
        .or(dv.server_timestamp.as_ref())
        .and_then(opcua_datetime_to_ts)
        .unwrap_or_else(OffsetDateTime::now_utc)
}

/// Convert an OPC-UA `DateTime` (100 ns ticks since 1601-01-01) to an `OffsetDateTime`.
fn opcua_datetime_to_ts(dt: &opcua::types::DateTime) -> Option<OffsetDateTime> {
    if dt.is_null() {
        return None;
    }
    // Ticks between the OPC-UA epoch (1601-01-01) and the Unix epoch (1970-01-01).
    const UNIX_EPOCH_TICKS: i128 = 116_444_736_000_000_000;
    let nanos = (dt.ticks() as i128 - UNIX_EPOCH_TICKS) * 100;
    OffsetDateTime::from_unix_timestamp_nanos(nanos).ok()
}

/// The publishing interval of a device's subscription: the fastest sampling interval of its
/// subscribed points (zero included: the server then publishes at its fastest rate). The C
/// module (impl/c/connectors/opcua/connector_opcua.c `publishing_interval_ms`) derives the same.
fn publishing_interval_for(sampling: impl Iterator<Item = Duration>) -> Duration {
    sampling.min().unwrap_or(DEFAULT_SAMPLING_INTERVAL)
}

/// The interval the server granted, when it is not the one requested. Both travel as whole
/// milliseconds here (the client floors a revised publishing interval), so a difference under a
/// millisecond is not a revision.
fn revised(requested: Duration, revised_ms: f64) -> Option<Duration> {
    let requested_ms = requested.as_secs_f64() * 1000.0;
    (revised_ms.is_finite() && (revised_ms - requested_ms).abs() >= 1.0)
        .then(|| Duration::from_secs_f64(revised_ms.max(0.0) / 1000.0))
}

/// Build a contract sample from an OPC-UA `DataValue`. Shared by the polling path
/// (`read_points`) and the push path (`subscribe`) so both decode identically.
fn build_sample(id: &str, model: &OpcuaPoint, dv: &DataValue, types: &resolve::SessionTypes) -> Sample {
    let status = dv.status.unwrap_or(StatusCode::Good);
    if let (Some(index), StatusCode::BadIndexRangeNoData) = (model.index, status) {
        return bad_sample(id, Some(model), &format!("index {index} out of range (the server reports {status})"));
    }
    if !status.is_good() {
        return bad_sample(id, Some(model), &format!("bad status: {status}"));
    }
    let mut variant = match &dv.value {
        Some(v) => v,
        None => return bad_sample(id, Some(model), "no value returned"),
    };
    // A read with an IndexRange returns an array holding just the selected element. A server
    // that ignores the range (python-asyncua does) returns the whole array instead: select the
    // element here. Only a one-element array cannot be told apart, and is taken as the answer.
    if let (Some(index), Variant::Array(array)) = (model.index, variant) {
        match array.values.as_slice() {
            [element] => variant = element,
            [] => return bad_sample(id, Some(model), &format!("index {index} out of range (no element returned)")),
            all => match all.get(index as usize) {
                Some(element) => variant = element,
                None => {
                    let len = all.len();
                    return bad_sample(id, Some(model), &format!("index {index} out of range (length {len})"));
                }
            },
        }
    }
    let decoded = match decode_value(id, model, variant, types) {
        Ok(d) => d,
        Err(e) => return bad_sample(id, Some(model), &e),
    };
    let (out_value, datatype) = match model.mode {
        Mode::Raw => (None, model.datatype),
        Mode::Typed => (Some(model.transform.apply(decoded.value)), Some(decoded.datatype)),
    };
    let raw = decoded.raw;
    Sample {
        source_value: None,
        ts: data_value_ts(dv),
        device: String::new(),
        protocol: PROTOCOL,
        point: id.to_string(),
        mode: model.mode,
        datatype,
        value: out_value,
        raw,
        raw_group: 1,
        quality: Quality::Good,
        unit: model.unit.clone(),
        addr: decoded.addr,
        seq: None,
        error: None,
    }
}

/// A value read from the server, before the point's mode and transform are applied.
struct Decoded {
    value: Value,
    datatype: DataType,
    raw: Vec<u8>,
    addr: serde_json::Value,
}

/// Decode a value as the point selects it: a structure field, a raw structure body, a
/// primitive, or one of the built-in types rendered as an SDK datatype (design D3, D4, D8).
fn decode_value(
    id: &str,
    model: &OpcuaPoint,
    variant: &Variant,
    types: &resolve::SessionTypes,
) -> Result<Decoded, String> {
    let mut addr = addr_echo(model);
    let ns = &types.namespaces;
    if model.field.is_some() {
        let plan = match types.plans.get(id) {
            Some(Ok(plan)) => plan,
            Some(Err(e)) => return Err(e.clone()),
            None => return Err(format!("{}: not resolved", resolve::UNAVAILABLE)),
        };
        let Variant::ExtensionObject(eo) = variant else {
            return Err(format!("value is {}, not a structure", variant_type_name(variant)));
        };
        let (encoding, body) = structure::split_extension_object(&eo.encode_to_vec(&encoding_context()))?;
        if let Some(expected) = &plan.encoding {
            let expected = ua_node_id_text(expected, ns);
            let got = structure::node_id_text(&encoding, ns);
            if got != expected {
                return Err(format!("value is encoded as {got}, expected the structure's encoding {expected}"));
            }
        }
        let datatype = model.datatype.unwrap_or(DataType::Float64);
        let (value, raw) = structure::decode(&plan.plan, &types.types, &body, ns)?;
        return Ok(Decoded { value, datatype, raw, addr });
    }
    if let Variant::ExtensionObject(eo) = variant {
        if model.mode == Mode::Typed {
            return Err("value is a structure; select one of its fields with address.field".into());
        }
        let (encoding, body) = structure::split_extension_object(&eo.encode_to_vec(&encoding_context()))?;
        addr["encoding_id"] = structure::node_id_text(&encoding, ns).into();
        if let Some(dt) = types.data_types.get(&model.node_id) {
            addr["data_type"] = ua_node_id_text(dt, ns).into();
        }
        // A raw sample carries no value; `build_sample` drops it.
        return Ok(Decoded { value: Value::Text(String::new()), datatype: DataType::Bytes, raw: body, addr });
    }
    if let Some((value, native_dt, raw)) = variant_to_value(variant) {
        let datatype = model.datatype.unwrap_or(native_dt);
        return Ok(Decoded { value, datatype, raw, addr });
    }
    if let Variant::Array(_) = variant {
        return Err("value is an array; select one element with address.index".into());
    }
    // A built-in type with no SDK primitive of its own: decode its binary form like a field.
    let bytes = variant.encode_to_vec(&encoding_context());
    let builtin = bytes
        .first()
        .and_then(|mask| structure::Builtin::from_id((mask & 0x3F) as u32))
        .ok_or("unsupported OPC-UA value type")?;
    let datatype = match (model.mode, model.datatype) {
        (Mode::Typed, Some(dt)) => dt,
        _ => structure::accepted(&structure::Leaf::Builtin(builtin))
            .first()
            .copied()
            .ok_or_else(|| format!("value is {}, which a point cannot read", builtin.name()))?,
    };
    let (value, raw) = structure::decode_builtin(builtin, &bytes[1..], datatype, ns)?;
    Ok(Decoded { value, datatype, raw, addr })
}

/// A context to re-encode values the client has decoded; encoding needs no server state.
pub(crate) fn encoding_context() -> opcua::types::Context<'static> {
    static CONTEXT: std::sync::OnceLock<opcua::types::ContextOwned> = std::sync::OnceLock::new();
    CONTEXT
        .get_or_init(|| {
            opcua::types::ContextOwned::new_default(
                opcua::types::NamespaceMap::new(),
                opcua::types::DecodingOptions::default(),
            )
        })
        .context()
}

/// A node id in the text form samples use, with `nsu=` for namespaces the server lists.
fn ua_node_id_text(id: &NodeId, namespaces: &[String]) -> String {
    match structure::read_node_id(&id.encode_to_vec(&encoding_context())) {
        Ok(n) => structure::node_id_text(&n, namespaces),
        Err(_) => id.to_string(),
    }
}

fn variant_type_name(v: &Variant) -> String {
    match v {
        Variant::Array(_) => "an array".into(),
        Variant::Empty => "empty".into(),
        other => {
            let bytes = other.encode_to_vec(&encoding_context());
            bytes
                .first()
                .and_then(|mask| structure::Builtin::from_id((mask & 0x3F) as u32))
                .map_or_else(|| "of an unknown type".into(), |b| b.name().to_string())
        }
    }
}

/// The `IndexRange` that selects one array element, or none.
fn index_range(index: Option<u32>) -> NumericRange {
    index.map_or(NumericRange::None, NumericRange::Index)
}

/// Build a `bad` quality sample carrying the error reason. A point we know nothing about
/// reports as `raw`: the contract requires a `datatype` for `typed` and we have none.
fn bad_sample(id: &str, model: Option<&OpcuaPoint>, error: &str) -> Sample {
    Sample {
        source_value: None,
        ts: OffsetDateTime::now_utc(),
        device: String::new(),
        protocol: PROTOCOL,
        point: id.to_string(),
        mode: model.map(|m| m.mode).unwrap_or(Mode::Raw),
        datatype: model.and_then(|m| m.datatype),
        value: None,
        raw: Vec::new(),
        raw_group: 1,
        quality: Quality::Bad,
        unit: model.and_then(|m| m.unit.clone()),
        addr: model.map(addr_echo).unwrap_or(serde_json::Value::Null),
        seq: None,
        error: Some(error.to_string()),
    }
}

/// Echo the node id (textual form) for the sample `addr` field.
fn addr_echo(model: &OpcuaPoint) -> serde_json::Value {
    let mut addr = serde_json::json!({ "node_id": model.node_id.to_string() });
    if let Some(field) = &model.field {
        let path: Vec<String> = field
            .iter()
            .map(|s| match s.index {
                Some(i) => format!("{}[{i}]", s.name),
                None => s.name.clone(),
            })
            .collect();
        addr["field"] = path.join(".").into();
    }
    if let Some(index) = model.index {
        addr["index"] = index.into();
    }
    addr
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The subscription publishes at the fastest sampling interval of its points, zero included.
    /// Mirrors `check_publishing_interval` in impl/c/tests/opcua_sampling.c.
    #[test]
    fn publishing_interval_is_the_fastest_sampling_interval() {
        let ms = Duration::from_millis;
        assert_eq!(publishing_interval_for([ms(1000), ms(250)].into_iter()), ms(250));
        assert_eq!(publishing_interval_for([ms(2000), ms(5000)].into_iter()), ms(2000));
        assert_eq!(publishing_interval_for([ms(1000), ms(0)].into_iter()), ms(0));
        assert_eq!(publishing_interval_for(std::iter::empty()), DEFAULT_SAMPLING_INTERVAL);
    }

    /// Only a revision of a millisecond or more is one: both sides are whole milliseconds.
    #[test]
    fn revised_intervals() {
        let ms = Duration::from_millis;
        assert_eq!(revised(ms(50), 1000.0), Some(ms(1000)));
        assert_eq!(revised(ms(0), 100.0), Some(ms(100)));
        assert_eq!(revised(ms(200), 200.0), None);
        assert_eq!(revised(ms(200), 200.4), None);
        assert_eq!(revised(ms(200), f64::NAN), None);
    }

    #[test]
    fn node_id_textual() {
        let addr = NodeAddress {
            node_id: Some("ns=2;s=Temperature".into()),
            namespace: None,
            identifier: None,
                   field: None,
            index: None,
        };
        let nid = node_id_from(&addr).unwrap();
        assert_eq!(nid.namespace, 2);
    }

    #[test]
    fn node_id_structured_numeric() {
        let addr = NodeAddress {
            node_id: None,
            namespace: Some(3),
            identifier: Some(serde_json::json!(1001)),
                   field: None,
            index: None,
        };
        let nid = node_id_from(&addr).unwrap();
        assert_eq!(nid.namespace, 3);
    }

    #[test]
    fn variant_number_roundtrip() {
        let (v, dt, raw) = variant_to_value(&Variant::UInt16(17001)).unwrap();
        assert_eq!(v, Value::Number(17001.0));
        assert_eq!(dt, DataType::Uint16);
        assert_eq!(raw, 17001u16.to_be_bytes().to_vec());
    }

    #[test]
    fn variant_bool() {
        let (v, dt, _) = variant_to_value(&Variant::Boolean(true)).unwrap();
        assert_eq!(v, Value::Bool(true));
        assert_eq!(dt, DataType::Bool);
    }

    #[test]
    fn build_variant_float() {
        let v = build_variant(DataType::Float32, &serde_json::json!(404.17)).unwrap();
        assert!(matches!(v, Variant::Float(_)));
    }

    #[test]
    fn build_variant_bool_type_mismatch() {
        assert!(build_variant(DataType::Bool, &serde_json::json!(5)).is_err());
    }

    #[test]
    fn big_uint64_is_text() {
        assert_eq!(uint64_value(u64::MAX), Value::Text(u64::MAX.to_string()));
    }

    #[test]
    fn opcua_datetime_roundtrip() {
        let dt = opcua::types::DateTime::ymd_hms(2026, 7, 1, 12, 30, 45);
        let ts = opcua_datetime_to_ts(&dt).unwrap();
        assert_eq!(ts.year(), 2026);
        assert_eq!(u8::from(ts.month()), 7);
        assert_eq!(ts.day(), 1);
        assert_eq!((ts.hour(), ts.minute(), ts.second()), (12, 30, 45));
    }

    #[test]
    fn opcua_datetime_null_is_none() {
        assert!(opcua_datetime_to_ts(&opcua::types::DateTime::null()).is_none());
    }

    #[test]
    fn data_value_ts_prefers_source_timestamp() {
        let source = opcua::types::DateTime::ymd_hms(2026, 1, 2, 3, 4, 5);
        let server = opcua::types::DateTime::ymd_hms(2026, 6, 7, 8, 9, 10);
        let dv = DataValue {
            source_timestamp: Some(source),
            server_timestamp: Some(server),
            ..Default::default()
        };
        let ts = data_value_ts(&dv);
        assert_eq!((ts.year(), u8::from(ts.month()), ts.day()), (2026, 1, 2));
    }

    #[test]
    fn data_value_ts_falls_back_to_now() {
        let before = OffsetDateTime::now_utc();
        let ts = data_value_ts(&DataValue::default());
        assert!(ts >= before);
    }
}
