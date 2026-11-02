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
mod namespaces;
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
use tedge_dot_sdk::runtime::CALL_METHOD_VERB;
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
    TimestampsToReturn, UAString, UserTokenPolicy, VariableId, Variant, WriteValue,
    CallMethodRequest,
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

/// How often the untranslated points of a push subscription are tried again (openspec
/// `opcua-namespace-table`, design D5). A polled point is tried again at its next poll.
const RETRY_INTERVAL: Duration = Duration::from_secs(30);

/// A fully-resolved OPC-UA point (node id + decode parameters), built in `configure`.
#[derive(Clone)]
struct OpcuaPoint {
    /// The node id to read. For a point with a namespace `table` this is the configured id until
    /// [`effective`] substitutes the session's translation.
    node_id: NodeId,
    /// The node id as configured, echoed in the sample's `addr`.
    addr_node: String,
    /// The namespace table the configured id was written against; `None` for a point without
    /// one and for a namespace-0 id, which are read as configured.
    table: Option<namespaces::Table>,
    /// `address.field`: the path into a structured value (openspec `opcua-custom-datatypes`).
    field: Option<Vec<structure::Segment>>,
    /// `address.index`: one element of an array value.
    index: Option<u32>,
    /// The effective monitored-item queue size: point, device, `[connection]`, then 16.
    queue_size: u32,
    mode: Mode,
    datatype: Option<DataType>,
    access: Access,
    unit: Option<String>,
    transform: Transform,
    /// A `call_method` point: `node_id` is then the method, and this the rest of the call.
    method: Option<MethodTarget>,
}

/// What a `call_method` point calls besides its method id (openspec `opcua-method-calls`).
#[derive(Clone)]
struct MethodTarget {
    /// The object the method is called on, as configured; translated like `node_id`.
    object: NodeId,
    /// The datatypes of the input arguments, in order.
    inputs: Vec<DataType>,
}

/// The key under which a method point's object id is translated, beside the point's own id for
/// its method id. A point id cannot contain the NUL, so no point's key can collide with it.
fn object_key(point: &str) -> String {
    format!("{point}\u{0}object")
}

/// The point (and node) a translation key names, for the log.
fn key_label(key: &str) -> String {
    match key.strip_suffix("\u{0}object") {
        Some(point) => format!("{point} (object_id)"),
        None => key.to_string(),
    }
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
    /// The node ids this session translated through namespace tables (design D3).
    translation: Arc<Mutex<namespaces::Translation>>,
    /// The most nodes one `Read` of this session carries; see [`nodes_per_read`].
    nodes_per_read: usize,
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
    /// Tries the subscribed points whose namespace was not translated again, and adds their
    /// monitored items once it is (design D5).
    _retry: Option<AbortOnDrop>,
}

/// The points behind each monitored item (node and array element): several points may share one.
type ByNode = HashMap<(NodeId, Option<u32>), Vec<(String, OpcuaPoint)>>;

/// What a subscription's data-change callback matches notifications against. Shared with the
/// retry task, which adds the points it translates later.
struct PushState {
    by_node: ByNode,
    types: Arc<resolve::SessionTypes>,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
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
    /// Call a `call_method` point's method with `request.value` as its arguments (openspec
    /// `opcua-method-calls`, design D8). The reasons are the C build's. Never retried: the method
    /// may have run even when no answer came back.
    async fn call_method(&self, device: &DeviceId, request: &CommandRequest) -> Result<CommandResult, String> {
        let model = self
            .devices
            .get(device)
            .and_then(|d| d.points.get(&request.point))
            .ok_or_else(|| format!("unknown point {} on device {device}", request.point))?;
        let method = model
            .method
            .as_ref()
            .ok_or_else(|| format!("point {} is not a method", request.point))?;
        let handle = self
            .sessions
            .get(device)
            .ok_or_else(|| format!("device {device} is not connected"))?;
        let (object_id, method_id) =
            effective_method(&request.point, model, method, &lock(&handle.translation))?;
        let empty = Vec::new();
        let arguments = request.value.as_ref().and_then(|v| v.as_array()).unwrap_or(&empty);
        let input_arguments = method_arguments(&method.inputs, arguments)?;

        let call = CallMethodRequest { object_id, method_id, input_arguments: Some(input_arguments) };
        let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
        let results = tokio::time::timeout(timeout, handle.session.call(vec![call]))
            .await
            .map_err(|_| format!("call failed: {}", StatusCode::BadTimeout))?
            .map_err(|status| format!("call failed: {status}"))?;
        let result = results
            .into_iter()
            .next()
            .ok_or_else(|| format!("call failed: {}", StatusCode::BadUnexpectedError))?;
        for (k, status) in result.input_argument_results.iter().flatten().enumerate() {
            if !status.is_good() {
                return Err(format!("call rejected: argument {}: {status}", k + 1));
            }
        }
        if !result.status_code.is_good() {
            return Err(format!("call rejected: {}", result.status_code));
        }
        let outputs = result.output_arguments.unwrap_or_default();
        Ok(CommandResult {
            point: request.point.clone(),
            value: Some(serde_json::Value::Array(outputs.iter().map(output_json).collect())),
            raw: None,
        })
    }

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
                let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
                let limit = read_limit(&handle.session, timeout).await;
                handle.nodes_per_read = nodes_per_read(limit);
                if handle.nodes_per_read < NODES_PER_READ {
                    tracing::info!(
                        "{name}: the server reads at most {} node(s) per request; polls go out in blocks of that size",
                        handle.nodes_per_read
                    );
                }
                let namespaces = self.translate(name, &handle, false).await;
                handle.types = Arc::new(
                    resolve_types(
                        name,
                        &self.devices[name].points,
                        &handle.session,
                        &handle.translation,
                        namespaces,
                        self.conn.request_timeout_s,
                    )
                    .await,
                );
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

    /// Translate the device's points that have a namespace table, for `handle`'s session: all of
    /// them, or with `retry` only those not translated yet (design D3, D5). Logs each namespace
    /// the server lists at another index than the table, once per session. Returns the server's
    /// namespace array when it was read.
    async fn translate(&self, name: &str, handle: &SessionHandle, retry: bool) -> Option<Vec<String>> {
        let dev = &self.devices[name];
        let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
        translate(name, &dev.points, &handle.session, &handle.translation, retry, timeout).await
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

/// The subscribed points of one device whose namespace was not translated when the subscription
/// was created (design D5). Every [`RETRY_INTERVAL`] they are translated again; those that now
/// have a node get their monitored items in the existing subscription, the others report why.
struct Retry {
    device: String,
    points: HashMap<String, OpcuaPoint>,
    session: Arc<Session>,
    translation: Arc<Mutex<namespaces::Translation>>,
    state: Arc<Mutex<PushState>>,
    subscription_id: u32,
    pending: Vec<(String, OpcuaPoint, Duration)>,
    tx: tokio::sync::mpsc::UnboundedSender<Sample>,
    request_timeout_s: u64,
}

impl Retry {
    async fn run(mut self) {
        while !self.pending.is_empty() {
            tokio::time::sleep(RETRY_INTERVAL).await;
            let timeout = Duration::from_secs(self.request_timeout_s.max(1));
            let namespaces =
                translate(&self.device, &self.points, &self.session, &self.translation, true, timeout).await;
            let mut ready = Vec::new();
            let mut waiting = Vec::new();
            {
                let translation = lock(&self.translation);
                for (id, model, interval) in self.pending.drain(..) {
                    match effective(&id, &model, &translation) {
                        Ok(translated) => ready.push((id, model, translated, interval)),
                        Err(reason) => waiting.push((id, model, interval, reason)),
                    }
                }
            }
            if !ready.is_empty() {
                waiting.extend(self.subscribe(ready, namespaces).await);
            }
            for (id, model, _, reason) in &waiting {
                let mut sample = bad_sample(id, Some(model), reason);
                sample.device = self.device.clone();
                if self.tx.send(sample).is_err() {
                    return; // the subscription is gone
                }
            }
            self.pending = waiting.into_iter().map(|(id, model, interval, _)| (id, model, interval)).collect();
        }
    }

    /// Add monitored items for points that now have a node. Returns those that still could not
    /// be subscribed, with the reason.
    async fn subscribe(
        &mut self,
        ready: Vec<(String, OpcuaPoint, OpcuaPoint, Duration)>,
        namespaces: Option<Vec<String>>,
    ) -> Vec<(String, OpcuaPoint, Duration, String)> {
        if ready.iter().any(|(_, _, m, _)| m.field.is_some() || m.mode == Mode::Raw) {
            let types = resolve_types(
                &self.device,
                &self.points,
                &self.session,
                &self.translation,
                namespaces,
                self.request_timeout_s,
            )
            .await;
            lock(&self.state).types = Arc::new(types);
        }
        // A node already monitored for another point is shared, as at subscribe (design D5 of
        // `opcua-custom-datatypes`); only new nodes get an item.
        let monitored = {
            let state = lock(&self.state);
            group_monitored(
                ready
                    .iter()
                    .filter(|(_, _, m, _)| !state.by_node.contains_key(&(m.node_id.clone(), m.index)))
                    .map(|(_, _, m, interval)| (m.node_id.clone(), m.index, *interval, m.queue_size)),
            )
        };
        let requests: Vec<MonitoredItemCreateRequest> = monitored.iter().map(monitored_item).collect();
        let session = self.session.clone();
        let subscription_id = self.subscription_id;
        let rejected: HashMap<(NodeId, Option<u32>), String> =
            match create_in_batches(requests, MONITORED_ITEMS_PER_REQUEST, |batch| {
                session.create_monitored_items(subscription_id, TimestampsToReturn::Both, batch)
            })
            .await
            {
                Ok(results) => monitored
                    .iter()
                    .zip(&results)
                    .filter(|(_, r)| !r.result.status_code.is_good())
                    .map(|(item, r)| {
                        ((item.node_id.clone(), item.index), format!("monitored item rejected: {}", r.result.status_code))
                    })
                    .collect(),
                Err(status) => monitored
                    .iter()
                    .map(|item| ((item.node_id.clone(), item.index), format!("create_monitored_items failed: {status}")))
                    .collect(),
            };
        let mut waiting = Vec::new();
        let mut added = Vec::new();
        let mut state = lock(&self.state);
        for (id, model, translated, interval) in ready {
            let key = (translated.node_id.clone(), translated.index);
            match rejected.get(&key) {
                Some(reason) => waiting.push((id, model, interval, reason.clone())),
                None => {
                    added.push(id.clone());
                    state.by_node.entry(key).or_default().push((id, translated));
                }
            }
        }
        if !added.is_empty() {
            tracing::info!(device = %self.device, points = %added.join(", "), "subscribed after translating their namespace");
        }
        waiting
    }
}

/// The create request of one monitored item, as `subscribe` and its retry task make them.
fn monitored_item(item: &MonitoredItem) -> MonitoredItemCreateRequest {
    let mut watch: ReadValueId = item.node_id.clone().into();
    watch.index_range = index_range(item.index);
    MonitoredItemCreateRequest::new(
        watch,
        MonitoringMode::Reporting,
        MonitoringParameters {
            sampling_interval: item.interval.as_millis() as f64,
            queue_size: item.queue_size,
            discard_oldest: true,
            ..Default::default()
        },
    )
}

/// The node ids of `points` to translate, each under its translation key: every point with a
/// namespace table, or with `retry` only those not translated yet. A method point translates two
/// ids, its method under its own id and its object under [`object_key`] (design D7).
fn translation_keys<'a>(
    points: &'a HashMap<String, OpcuaPoint>,
    translation: &namespaces::Translation,
    retry: bool,
) -> Vec<(String, &'a NodeId, &'a namespaces::Table)> {
    let failed = |key: &str| matches!(translation.nodes.get(key), Some(Err(_)));
    points
        .iter()
        .filter(|(id, p)| !retry || failed(id) || (p.method.is_some() && failed(&object_key(id))))
        .filter_map(|(id, p)| p.table.as_ref().map(|table| (id, p, table)))
        .flat_map(|(id, p, table)| {
            let object = p.method.as_ref().map(|m| (object_key(id), &m.object, table));
            std::iter::once((id.clone(), &p.node_id, table)).chain(object)
        })
        .collect()
}

/// See [`OpcuaConnector::translate`]; free so the retry task of a subscription can call it.
async fn translate(
    name: &str,
    points: &HashMap<String, OpcuaPoint>,
    session: &Session,
    translation: &Mutex<namespaces::Translation>,
    retry: bool,
    timeout: Duration,
) -> Option<Vec<String>> {
    let keyed = translation_keys(points, &lock(translation), retry);
    let mapped: Vec<namespaces::Mapped> = keyed
        .iter()
        .map(|(key, node, table)| namespaces::Mapped { point: key, node, table })
        .collect();
    if mapped.is_empty() {
        return None;
    }
    let server = namespaces::read_array(session, timeout).await;
    let mut translation = lock(translation);
    for moved in translation.apply(&mapped, &server) {
        tracing::info!("{name}: namespace {} local {} -> server {}", moved.uri, moved.local, moved.server);
    }
    if !retry {
        for p in &mapped {
            if let Some(Err(e)) = translation.nodes.get(p.point) {
                warn!("device '{name}' point '{}': {e}", key_label(p.point));
            }
        }
    }
    server.ok()
}

/// The point as this session reads it: a point with a namespace table gets the node id the
/// server uses for it now, or the reason it has none (design D3, D4).
fn effective(id: &str, model: &OpcuaPoint, translation: &namespaces::Translation) -> Result<OpcuaPoint, String> {
    if model.table.is_none() {
        return Ok(model.clone());
    }
    match translation.nodes.get(id) {
        Some(Ok(node)) => {
            let mut model = model.clone();
            model.node_id = node.clone();
            Ok(model)
        }
        Some(Err(e)) => Err(e.clone()),
        None => Err("namespace not translated".into()),
    }
}

/// The object and method ids a method point calls in this session, or why it has none: each is
/// translated like a variable's node id when the point has a namespace table (design D7).
fn effective_method(
    id: &str,
    model: &OpcuaPoint,
    method: &MethodTarget,
    translation: &namespaces::Translation,
) -> Result<(NodeId, NodeId), String> {
    if model.table.is_none() {
        return Ok((method.object.clone(), model.node_id.clone()));
    }
    let node = |key: &str| match translation.nodes.get(key) {
        Some(Ok(node)) => Ok(node.clone()),
        Some(Err(e)) => Err(e.clone()),
        None => Err("namespace not translated".to_string()),
    };
    Ok((node(&object_key(id))?, node(id)?))
}

/// Resolve, for a new session, what the device's structure and raw points need: the
/// namespace array and the data type definitions (openspec `opcua-custom-datatypes`, design D3,
/// D6). Points whose namespace is not translated are left out. Bounded, so a server that stops
/// answering cannot stall the connect; its field points then report why.
async fn resolve_types(
    name: &str,
    points: &HashMap<String, OpcuaPoint>,
    session: &Session,
    translation: &Mutex<namespaces::Translation>,
    namespaces: Option<Vec<String>>,
    request_timeout_s: u64,
) -> resolve::SessionTypes {
    let wanted: Vec<resolve::Wanted> = {
        let translation = lock(translation);
        points
        .iter()
        .filter(|(_, p)| p.field.is_some() || p.mode == Mode::Raw)
        .filter_map(|(id, p)| effective(id, p, &translation).ok().map(|p| (id, p)))
        .map(|(id, p)| resolve::Wanted {
            point: id.clone(),
            node: p.node_id,
            field: p.field,
            index: p.index,
            datatype: p.datatype,
        })
        .collect()
    };
    let timeout = Duration::from_secs(request_timeout_s.max(1) * 4);
    match tokio::time::timeout(timeout, resolve::resolve(session, &wanted, namespaces)).await {
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
        let conn_queue_size = config::queue_size(&self.conn.queue_size)
            .map_err(|e| ConfigError::Invalid(format!("[connection]: {e}")))?;
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
            let device_queue_size = config::queue_size(&endpoint.queue_size).map_err(|e| {
                ConfigError::Invalid(format!("device '{}' protocol_address: {e}", d.name))
            })?;
            let security = config::device_security(&self.conn, &endpoint, base_dir)
                .map_err(|e| ConfigError::Invalid(format!("device '{}': {e}", d.name)))?;
            let device_table = endpoint
                .namespaces
                .as_ref()
                .map(namespaces::parse_table)
                .transpose()
                .map_err(|e| ConfigError::Invalid(format!("device '{}' protocol_address: {e}", d.name)))?;
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
                let access = Access::parse(p.access.as_deref());
                if access == Access::CallMethod {
                    let library_table = p.library.as_ref().and_then(|l| l.get("namespaces"));
                    let point = method_point(&addr, library_table, device_table.as_ref())
                        .map_err(|e| {
                            let gap = if e.starts_with(':') { "" } else { " " };
                            ConfigError::Invalid(format!("point '{}'{gap}{e}", p.id))
                        })?;
                    points.insert(p.id.clone(), point);
                    continue;
                }
                for (key, set) in [
                    ("object_id", addr.object_id.is_some()),
                    ("method_id", addr.method_id.is_some()),
                    ("inputs", addr.inputs.is_some()),
                ] {
                    if set {
                        return Err(ConfigError::Invalid(format!(
                            "point '{}' address: {key} applies only to a call_method point",
                            p.id
                        )));
                    }
                }
                let node_id = node_id_from(&addr).map_err(|e| {
                    ConfigError::Invalid(format!("point '{}' address: {e}", p.id))
                })?;
                let queue_size = config::queue_size(&addr.queue_size)
                    .map_err(|e| ConfigError::Invalid(format!("point '{}' address: {e}", p.id)))?
                    .or(device_queue_size)
                    .or(conn_queue_size)
                    .unwrap_or(config::DEFAULT_QUEUE_SIZE);
                // A library's table describes the library's addresses; the device's describes
                // the rest (spec "Table applies to a point").
                let table = match p.library.as_ref().and_then(|l| l.get("namespaces")) {
                    Some(value) => Some(namespaces::parse_table(value).map_err(|e| {
                        ConfigError::Invalid(format!("point '{}': [library] {e}", p.id))
                    })?),
                    None => device_table.clone(),
                };
                let table = match table {
                    Some(table) if node_id.namespace != 0 => {
                        namespaces::check_index(&node_id, &table)
                            .map_err(|e| ConfigError::Invalid(format!("point '{}': {e}", p.id)))?;
                        Some(table)
                    }
                    _ => None,
                };
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
                if (field.is_some() || addr.index.is_some()) && access != Access::Read {
                    return Err(ConfigError::Invalid(format!(
                        "point '{}': points with address.field or address.index are read-only",
                        p.id
                    )));
                }
                points.insert(
                    p.id.clone(),
                    OpcuaPoint {
                        addr_node: node_id.to_string(),
                        node_id,
                        table,
                        field,
                        index: addr.index,
                        queue_size,
                        mode,
                        datatype: p.datatype,
                        access,
                        unit: p.unit.clone(),
                        transform: p.transform.unwrap_or_default(),
                        method: None,
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
            command_verbs: vec!["write".into(), CALL_METHOD_VERB.into()],
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

        // Points whose namespace this session has not translated yet are tried again first, so a
        // namespace the server added since is picked up at the next poll (design D5).
        if let Some(handle) = self.sessions.get(device) {
            let failing = lock(&handle.translation)
                .has_failures(models.iter().map(|(id, _)| id.as_str()));
            if failing {
                let namespaces = self.translate(device, handle, true).await;
                // Structure and raw points need their types resolved once they have a node.
                let typed = {
                    let translation = lock(&handle.translation);
                    self.devices[device].points.iter().any(|(id, p)| {
                        match (p.table.is_some(), translation.nodes.get(id)) {
                            (true, Some(Ok(_))) if p.field.is_some() => !handle.types.plans.contains_key(id),
                            (true, Some(Ok(node))) if p.mode == Mode::Raw => {
                                !handle.types.data_types.contains_key(node)
                            }
                            _ => false,
                        }
                    })
                };
                if let (Some(namespaces), true) = (namespaces, typed) {
                    let types = resolve_types(
                        device,
                        &self.devices[device].points,
                        &handle.session,
                        &handle.translation,
                        Some(namespaces),
                        self.conn.request_timeout_s,
                    )
                    .await;
                    if let Some(handle) = self.sessions.get_mut(device) {
                        handle.types = Arc::new(types);
                    }
                }
            }
        }

        let (session, types, translation, block) = match self.sessions.get(device) {
            Some(h) => (h.session.clone(), h.types.clone(), h.translation.clone(), h.nodes_per_read),
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
        // Each known point as this session reads it, or with the reason it cannot be read.
        let models: Vec<(String, Option<(OpcuaPoint, Option<String>)>)> = {
            let translation = lock(&translation);
            models
                .into_iter()
                .map(|(id, model)| {
                    let model = model.map(|m| match effective(&id, &m, &translation) {
                        _ if m.method.is_some() => {
                            let reason = format!("point {id} is a method");
                            (m, Some(reason))
                        }
                        Ok(translated) => (translated, None),
                        Err(reason) => (m, Some(reason)),
                    });
                    (id, model)
                })
                .collect()
        };
        for (id, model) in models {
            match model {
                Some((m, Some(reason))) => out.push(bad_sample(&id, Some(&m), &reason)),
                Some((m, None)) => {
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
            // Bound each service call: while the transport is down the client queues requests
            // waiting to resurrect the session, and an unbounded await here would wedge the
            // runtime's poll loop instead of reporting the outage.
            let timeout = Duration::from_secs(self.conn.request_timeout_s.max(1));
            let (values, lost) = read_in_blocks(reads, block, |chunk| {
                let session = session.clone();
                async move {
                    match tokio::time::timeout(
                        timeout,
                        session.read(&chunk, TimestampsToReturn::Neither, 0.0),
                    )
                    .await
                    {
                        Ok(Ok(values)) => Ok(values),
                        Ok(Err(status)) => Err(BlockFailure {
                            reason: format!("read failed: {status}"),
                            link_lost: link_lost(status),
                        }),
                        Err(_) => Err(BlockFailure {
                            reason: format!("read timed out after {}s (transport down?)", timeout.as_secs()),
                            link_lost: true,
                        }),
                    }
                }
            })
            .await;
            for (id, model, slot) in &known {
                match values.get(*slot) {
                    // Not read: the link was lost before its block.
                    None => {}
                    Some(Ok(dv)) => {
                        let mut sample = build_sample(id, model, dv, &types);
                        // Contract §5: polled samples carry the read-completion time, which the
                        // runtime sets. Servers may return a (stale) source timestamp even for
                        // TimestampsToReturn::Neither; only the push path reports event time.
                        sample.source_ts = None;
                        out.push(sample);
                    }
                    Some(Err(reason)) => out.push(bad_sample(id, Some(model), reason)),
                }
            }
            if let Some(reason) = lost {
                return Err(ConnectorError::LinkLost { samples: out, reason });
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
        let (session, types, translation) =
            (handle.session.clone(), handle.types.clone(), handle.translation.clone());
        let points = self.devices[device].points.clone();
        // One subscription per device, publishing at the fastest requested sampling rate.
        let publishing_interval =
            publishing_interval_for(items.iter().map(|(_, _, interval)| *interval));

        // A point whose namespace this session has not translated gets no monitored item yet: it
        // reports why and is tried again by the retry task below (design D4, D5).
        let mut pending: Vec<(String, OpcuaPoint, Duration, String)> = Vec::new();
        let items: Vec<(String, OpcuaPoint, Duration)> = {
            let translation = lock(&translation);
            items
                .into_iter()
                .filter_map(|(id, model, interval)| match effective(&id, &model, &translation) {
                    Ok(model) => Some((id, model, interval)),
                    Err(reason) => {
                        pending.push((id, model, interval, reason));
                        None
                    }
                })
                .collect()
        };

        // Notifications are matched back to points by node id and element: several points may
        // share one monitored item (design D5).
        let mut by_node: ByNode = HashMap::new();
        let monitored = group_monitored(
            items.iter().map(|(_, model, interval)| (model.node_id.clone(), model.index, *interval, model.queue_size)),
        );
        for (id, model, _) in &items {
            by_node.entry((model.node_id.clone(), model.index)).or_default().push((id.clone(), model.clone()));
        }
        // The point ids per item, for log lines and errors (`by_node` moves into the callback).
        let names: HashMap<(NodeId, Option<u32>), Vec<String>> = by_node
            .iter()
            .map(|(key, points)| (key.clone(), points.iter().map(|(id, _)| id.clone()).collect()))
            .collect();

        // The data-change callback is synchronous, so it forwards through an unbounded channel
        // to a spawned task that awaits the runtime's (bounded) sink.
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Sample>();
        let retry_tx = tx.clone();
        let device_name = device.clone();
        let state = Arc::new(Mutex::new(PushState { by_node, types }));
        let callback_state = state.clone();
        let callback = DataChangeCallback::new(move |dv, item| {
            let watched = item.item_to_monitor();
            let node_id = &watched.node_id;
            tracing::trace!(%node_id, "data change notification");
            let index = match watched.index_range {
                NumericRange::Index(i) => Some(i),
                _ => None,
            };
            let state = lock(&callback_state);
            if let Some(models) = state.by_node.get(&(node_id.clone(), index)) {
                for (id, model) in models {
                    let mut sample = build_sample(id, model, &dv, &state.types);
                    sample.device = device_name.clone();
                    // Ignore send errors: the forwarder has already shut down.
                    let _ = tx.send(sample);
                }
            } else {
                tracing::debug!(%node_id, "data change for unrequested node id, dropped");
            }
        });

        let subscription_id = session
            .create_subscription(publishing_interval, 60, 20, 0, 0, true, callback)
            .await
            .map_err(|s| ConnectorError::Transport(format!("create_subscription failed: {s}")))?;

        // One monitored item per node and element, sampled at the fastest effective sampling
        // interval of its points, queueing as many values as the largest queue size among them.
        let requests: Vec<MonitoredItemCreateRequest> = monitored
            .iter()
            .map(monitored_item)
            .collect();
        // Every point may be waiting for its namespace: the subscription is still created (with
        // no items), for the retry task to add theirs to.
        let results = match create_in_batches(requests, MONITORED_ITEMS_PER_REQUEST, |batch| {
            session.create_monitored_items(subscription_id, TimestampsToReturn::Both, batch)
        })
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
            .map(|(item, r)| format!("{}: {}", point_ids(&names, item).join(", "), r.result.status_code))
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
        for (item, r) in monitored.iter().zip(&results) {
            if let Some(revised) = revised(item.interval, r.result.revised_sampling_interval) {
                tracing::info!(
                    device = %device, point = %point_ids(&names, item).join(", "),
                    requested = ?item.interval, revised = ?revised,
                    "the server revised the point's sampling interval"
                );
            }
            if r.result.revised_queue_size != item.queue_size {
                tracing::info!(
                    device = %device, point = %point_ids(&names, item).join(", "),
                    requested = item.queue_size, revised = r.result.revised_queue_size,
                    "the server revised the point's queue size"
                );
            }
        }

        let retry = if pending.is_empty() {
            None
        } else {
            for (id, model, _, reason) in &pending {
                let mut sample = bad_sample(id, Some(model), reason);
                sample.device = device.clone();
                let _ = retry_tx.send(sample);
            }
            let retry = Retry {
                device: device.clone(),
                points,
                session: session.clone(),
                translation,
                state,
                subscription_id,
                pending: pending.into_iter().map(|(id, model, interval, _)| (id, model, interval)).collect(),
                tx: retry_tx,
                request_timeout_s: self.conn.request_timeout_s,
            };
            Some(AbortOnDrop(tokio::spawn(retry.run())))
        };

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
                _retry: retry,
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
        if verb == CALL_METHOD_VERB {
            return self.call_method(device, request).await.map_err(ConnectorError::Other);
        }
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

        let handle = self
            .sessions
            .get(device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        let model = effective(&request.point, &model, &lock(&handle.translation))
            .map_err(ConnectorError::Other)?;
        let session = handle.session.clone();

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
/// The model of a `call_method` point (openspec `opcua-method-calls`, spec "Method addressing").
/// Its namespace table is the library's when a library supplied the address, else the device's,
/// as for a variable; it applies when either id has a namespace other than 0. Errors read
/// `address: ...` or `: ...` after the point, as the C build's do.
fn method_point(
    addr: &NodeAddress,
    library_table: Option<&serde_json::Value>,
    device_table: Option<&namespaces::Table>,
) -> Result<OpcuaPoint, String> {
    for (key, set) in [
        ("node_id", addr.node_id.is_some()),
        ("namespace", addr.namespace.is_some()),
        ("identifier", addr.identifier.is_some()),
        ("field", addr.field.is_some()),
        ("index", addr.index.is_some()),
        ("queue_size", addr.queue_size.is_some()),
    ] {
        if set {
            return Err(format!("address: {key} does not apply to a call_method point"));
        }
    }
    let node = |key: &str, text: &Option<String>| {
        let text = text
            .as_ref()
            .ok_or_else(|| format!("address: {key} is required for a call_method point"))?;
        NodeId::from_str(text).map_err(|_| format!("address: invalid {key} '{text}'"))
    };
    let object = node("object_id", &addr.object_id)?;
    let method = node("method_id", &addr.method_id)?;
    let inputs = method_inputs(addr.inputs.as_ref()).map_err(|e| format!("address: {e}"))?;
    let table = match library_table {
        Some(value) => Some(namespaces::parse_table(value).map_err(|e| format!(": [library] {e}"))?),
        None => device_table.cloned(),
    };
    let table = match table {
        Some(table) if object.namespace != 0 || method.namespace != 0 => {
            namespaces::check_index(&object, &table)
                .and_then(|()| namespaces::check_index(&method, &table))
                .map_err(|e| format!(": {e}"))?;
            Some(table)
        }
        _ => None,
    };
    Ok(OpcuaPoint {
        addr_node: method.to_string(),
        node_id: method,
        table,
        field: None,
        index: None,
        queue_size: config::DEFAULT_QUEUE_SIZE,
        mode: Mode::Typed,
        datatype: None,
        access: Access::CallMethod,
        unit: None,
        transform: Transform::default(),
        method: Some(MethodTarget { object, inputs }),
    })
}

/// `address.inputs`: the scalar datatypes a method's input arguments are converted to.
fn method_inputs(inputs: Option<&serde_json::Value>) -> Result<Vec<DataType>, String> {
    let Some(inputs) = inputs else { return Ok(Vec::new()) };
    let entries = inputs.as_array().ok_or("inputs must be an array of datatypes")?;
    entries
        .iter()
        .enumerate()
        .map(|(k, entry)| {
            let name = entry.as_str().ok_or("inputs must be an array of datatypes")?;
            match serde_json::from_value::<DataType>(serde_json::json!(name)) {
                Ok(DataType::Bytes) => {
                    Err(format!("inputs[{k}]: bytes is not supported as a method argument"))
                }
                Ok(datatype) => Ok(datatype),
                Err(_) => Err(format!("inputs[{k}]: unknown datatype '{name}'")),
            }
        })
        .collect()
}

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
            .max_array_length(MAX_ARRAY_LENGTH)
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
                    translation: Arc::default(),
                    nodes_per_read: NODES_PER_READ,
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

/// A method's input arguments, each converted to its declared datatype; the position in a reason
/// is 1-based. Stricter than a write's conversion, and the same as the C build's: an integer must
/// be whole and in range for its type (a 64-bit one beyond 2^53 - 1 only as a decimal string),
/// and a bool, float or string must be a JSON bool, number or string.
fn method_arguments(inputs: &[DataType], arguments: &[serde_json::Value]) -> Result<Vec<Variant>, String> {
    if arguments.len() != inputs.len() {
        return Err(format!("expected {} arguments, got {}", inputs.len(), arguments.len()));
    }
    inputs
        .iter()
        .zip(arguments)
        .enumerate()
        .map(|(k, (datatype, value))| {
            method_argument(*datatype, value).ok_or_else(|| {
                let name = serde_json::to_value(datatype).ok();
                let name = name.as_ref().and_then(|n| n.as_str()).unwrap_or("value");
                format!("argument {}: {value} is not a valid {name}", k + 1)
            })
        })
        .collect()
}

fn method_argument(datatype: DataType, value: &serde_json::Value) -> Option<Variant> {
    use serde_json::Value as Json;
    let int = || value.as_i64();
    let uint = || value.as_u64();
    let safe = |n: i64| (n.abs() <= MAX_SAFE_INT).then_some(n);
    Some(match datatype {
        DataType::Bool => Variant::Boolean(value.as_bool()?),
        DataType::Int8 => Variant::SByte(int()?.try_into().ok()?),
        DataType::Uint8 => Variant::Byte(uint()?.try_into().ok()?),
        DataType::Int16 => Variant::Int16(int()?.try_into().ok()?),
        DataType::Uint16 => Variant::UInt16(uint()?.try_into().ok()?),
        DataType::Int32 => Variant::Int32(int()?.try_into().ok()?),
        DataType::Uint32 => Variant::UInt32(uint()?.try_into().ok()?),
        DataType::Int64 => Variant::Int64(match value {
            Json::String(s) => s.parse().ok()?,
            _ => safe(int()?)?,
        }),
        DataType::Uint64 => Variant::UInt64(match value {
            Json::String(s) => s.parse().ok()?,
            _ => uint().filter(|&n| n <= MAX_SAFE_INT as u64)?,
        }),
        DataType::Float32 => Variant::Float(value.as_f64()? as f32),
        DataType::Float64 => Variant::Double(value.as_f64()?),
        DataType::String => Variant::String(UAString::from(value.as_str()?)),
        DataType::Bytes => return None,
    })
}

/// A method's output argument as JSON: a scalar as a sample's value would carry it (64-bit
/// integers beyond 2^53 - 1 as strings), an array of scalars as an array, anything else `null`
/// (design D8). Integers stay integers, as the C build prints them.
fn output_json(v: &Variant) -> serde_json::Value {
    use serde_json::json;
    match v {
        Variant::Boolean(b) => json!(b),
        Variant::SByte(i) => json!(i),
        Variant::Byte(u) => json!(u),
        Variant::Int16(i) => json!(i),
        Variant::UInt16(u) => json!(u),
        Variant::Int32(i) => json!(i),
        Variant::UInt32(u) => json!(u),
        Variant::Int64(i) if i.abs() <= MAX_SAFE_INT => json!(i),
        Variant::Int64(i) => json!(i.to_string()),
        Variant::UInt64(u) if *u <= MAX_SAFE_INT as u64 => json!(u),
        Variant::UInt64(u) => json!(u.to_string()),
        Variant::Float(f) => serde_json::Number::from_f64(*f as f64).map_or(json!(null), Into::into),
        Variant::Double(d) => serde_json::Number::from_f64(*d).map_or(json!(null), Into::into),
        Variant::String(s) => json!(s.as_ref()),
        Variant::Array(array) => serde_json::Value::Array(
            array
                .values
                .iter()
                .map(|v| match v {
                    Variant::Array(_) => serde_json::Value::Null,
                    scalar => output_json(scalar),
                })
                .collect(),
        ),
        _ => serde_json::Value::Null,
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

/// The source time of a `DataValue`: its source timestamp, else its server timestamp. Polled
/// reads request no timestamps, so a polled value carries the read time instead.
fn data_value_source_ts(dv: &DataValue) -> Option<OffsetDateTime> {
    dv.source_timestamp
        .as_ref()
        .or(dv.server_timestamp.as_ref())
        .and_then(opcua_datetime_to_ts)
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
/// The most monitored items one `CreateMonitoredItems` request carries. async-opcua refuses a
/// reply array longer than its `max_array_length` (1000 by default), and servers often cap
/// `MaxMonitoredItemsPerCall` at 1000; 500 stays under both.
const MONITORED_ITEMS_PER_REQUEST: usize = 500;

/// The client's decoding limit for arrays, raised from async-opcua's default of 1000 so that a
/// device with thousands of points is not refused with `BadDecodingError`.
const MAX_ARRAY_LENGTH: usize = 65_535;

/// One monitored item: the node and element it watches, and what it requests.
#[derive(Debug, Clone, PartialEq)]
struct MonitoredItem {
    node_id: NodeId,
    index: Option<u32>,
    /// The fastest effective sampling interval of the item's points.
    interval: Duration,
    /// The largest effective queue size of the item's points.
    queue_size: u32,
}

/// Group points `(node, element, sampling interval, queue size)` into monitored items, one per
/// node and element, in the order first seen. Linear in the number of points.
fn group_monitored(points: impl Iterator<Item = (NodeId, Option<u32>, Duration, u32)>) -> Vec<MonitoredItem> {
    let mut monitored: Vec<MonitoredItem> = Vec::new();
    let mut at: HashMap<(NodeId, Option<u32>), usize> = HashMap::new();
    for (node_id, index, interval, queue_size) in points {
        match at.get(&(node_id.clone(), index)) {
            Some(&i) => {
                let item = &mut monitored[i];
                item.interval = item.interval.min(interval);
                item.queue_size = item.queue_size.max(queue_size);
            }
            None => {
                at.insert((node_id.clone(), index), monitored.len());
                monitored.push(MonitoredItem { node_id, index, interval, queue_size });
            }
        }
    }
    monitored
}

/// The ids of the points an item serves, for log lines and errors.
fn point_ids<'a>(names: &'a HashMap<(NodeId, Option<u32>), Vec<String>>, item: &MonitoredItem) -> Vec<&'a str> {
    names
        .get(&(item.node_id.clone(), item.index))
        .map(|ids| ids.iter().map(String::as_str).collect())
        .unwrap_or_default()
}

/// Send `requests` through `create` in batches of at most `batch` and return the results in
/// order. The first failed batch ends it: its error is returned, and nothing after it is sent.
async fn create_in_batches<T, R, E, F, Fut>(requests: Vec<T>, batch: usize, mut create: F) -> Result<Vec<R>, E>
where
    F: FnMut(Vec<T>) -> Fut,
    Fut: std::future::Future<Output = Result<Vec<R>, E>>,
{
    let mut results = Vec::with_capacity(requests.len());
    let mut requests = requests.into_iter().peekable();
    while requests.peek().is_some() {
        let chunk: Vec<T> = requests.by_ref().take(batch).collect();
        results.extend(create(chunk).await?);
    }
    Ok(results)
}

/// The most nodes one polling `Read` carries. One request per point does not scale, and one
/// request for every point of a large device outgrows the client's message size (about 10,000
/// nodes) and the server's `MaxNodesPerRead`. 500, like [`MONITORED_ITEMS_PER_REQUEST`].
const NODES_PER_READ: usize = 500;

/// The block size of a session's polling reads: [`NODES_PER_READ`], or the server's
/// `MaxNodesPerRead` when that is lower. A limit of 0, or none read, means the server sets none.
fn nodes_per_read(server_limit: Option<u32>) -> usize {
    match server_limit {
        Some(limit) if limit > 0 => NODES_PER_READ.min(limit as usize),
        _ => NODES_PER_READ,
    }
}

/// The server's `OperationLimits.MaxNodesPerRead`, or `None` when it does not publish one.
async fn read_limit(session: &Session, timeout: Duration) -> Option<u32> {
    let read = ReadValueId {
        node_id: VariableId::Server_ServerCapabilities_OperationLimits_MaxNodesPerRead.into(),
        attribute_id: AttributeId::Value as u32,
        index_range: Default::default(),
        data_encoding: Default::default(),
    };
    let values = tokio::time::timeout(timeout, session.read(&[read], TimestampsToReturn::Neither, 0.0))
        .await
        .ok()?
        .ok()?;
    match values.into_iter().next()?.value {
        Some(Variant::UInt32(limit)) => Some(limit),
        _ => None,
    }
}

/// Why one block of a polling read failed.
#[derive(Debug, Clone, PartialEq)]
struct BlockFailure {
    /// The error every point of the block is reported with.
    reason: String,
    /// The link is gone: the blocks after this one would only wait for their own timeout.
    link_lost: bool,
}

/// A failed service call that means the link is gone, not that the server refused this request.
/// The status codes the C module (connector_opcua.c `read_point`) takes the transport down for,
/// plus the two async-opcua reports for a channel it has no connection on.
fn link_lost(status: StatusCode) -> bool {
    matches!(
        status,
        StatusCode::BadConnectionClosed
            | StatusCode::BadServerNotConnected
            | StatusCode::BadDisconnect
            | StatusCode::BadTimeout
            | StatusCode::BadSessionIdInvalid
            | StatusCode::BadSecureChannelClosed
            | StatusCode::BadInternalError
            | StatusCode::BadNotConnected
            | StatusCode::BadCommunicationError
    )
}

/// Read `items` through `read` in blocks of at most `block`, one after the other, and return one
/// result per item, in order. A block the server refuses fails only its own items. A block
/// answered with fewer values than it asked for fails the missing ones. A block that finds the
/// link gone ends it, and nothing after it is sent: before any value was read, every item fails
/// with its reason; after one was, the results end before that block and its reason is returned
/// too (`read_points` then reports [`ConnectorError::LinkLost`]).
async fn read_in_blocks<T, R, F, Fut>(
    items: Vec<T>,
    block: usize,
    mut read: F,
) -> (Vec<Result<R, String>>, Option<String>)
where
    F: FnMut(Vec<T>) -> Fut,
    Fut: std::future::Future<Output = Result<Vec<R>, BlockFailure>>,
{
    let total = items.len();
    let mut results: Vec<Result<R, String>> = Vec::with_capacity(total);
    let mut items = items.into_iter().peekable();
    while items.peek().is_some() {
        let chunk: Vec<T> = items.by_ref().take(block.max(1)).collect();
        let asked = chunk.len();
        match read(chunk).await {
            Ok(values) => {
                let got = values.len().min(asked);
                results.extend(values.into_iter().take(asked).map(Ok));
                results.extend((got..asked).map(|_| Err("no value returned".to_string())));
            }
            Err(failure) if failure.link_lost => {
                if results.iter().any(Result::is_ok) {
                    return (results, Some(failure.reason));
                }
                results.extend((results.len()..total).map(|_| Err(failure.reason.clone())));
                break;
            }
            Err(failure) => results.extend((0..asked).map(|_| Err(failure.reason.clone()))),
        }
    }
    (results, None)
}

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
        source_ts: data_value_source_ts(dv),
        ts: OffsetDateTime::now_utc(),
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
        source_ts: None,
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
    let mut addr = serde_json::json!({ "node_id": model.addr_node });
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
            queue_size: None,
            object_id: None,
            method_id: None,
            inputs: None,
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
            queue_size: None,
            object_id: None,
            method_id: None,
            inputs: None,
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

    fn configure(toml: &str) -> Result<OpcuaConnector, ConfigError> {
        let config: ConnectorConfig = toml::from_str(toml).unwrap();
        let mut connector = OpcuaConnector::default();
        connector.configure(&config).map(|()| connector)
    }

    /// `[connection]`, the device and the point, each optionally setting `queue_size`.
    fn queue_config(connection: &str, device: &str, point: &str) -> String {
        format!(
            r#"
            [connector]
            protocol = "opcua"
            [connection]
            {connection}
            [[device]]
            name = "plc"
            protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:4840" {device} }}
              [[device.point]]
              id = "own"
              datatype = "float64"
              address = {{ node_id = "ns=2;s=A" {point} }}
              [[device.point]]
              id = "inherited"
              datatype = "float64"
              address = {{ node_id = "ns=2;s=B" }}
            "#
        )
    }

    fn queue_sizes(connector: &OpcuaConnector) -> (u32, u32) {
        let points = &connector.devices["plc"].points;
        (points["own"].queue_size, points["inherited"].queue_size)
    }

    /// Point, then device, then `[connection]`, then 16 (spec §3.8).
    #[test]
    fn queue_size_precedence() {
        let c = configure(&queue_config("", "", "")).unwrap();
        assert_eq!(queue_sizes(&c), (16, 16));
        let c = configure(&queue_config("queue_size = 2", "", "")).unwrap();
        assert_eq!(queue_sizes(&c), (2, 2));
        let c = configure(&queue_config("queue_size = 2", ", queue_size = 4", "")).unwrap();
        assert_eq!(queue_sizes(&c), (4, 4));
        let c = configure(&queue_config("queue_size = 2", ", queue_size = 4", ", queue_size = 1")).unwrap();
        assert_eq!(queue_sizes(&c), (1, 4));
    }

    /// The messages are the C build's too (impl/c/tests/config.c `check_opcua_queue_size`).
    #[test]
    fn queue_size_is_validated_where_it_is() {
        let err = |toml: String| configure(&toml).err().expect("refused").to_string();
        assert!(err(queue_config("queue_size = \"16\"", "", ""))
            .contains("[connection]: queue_size must be an integer from 1 to 65535"));
        assert!(err(queue_config("", ", queue_size = 65536", ""))
            .contains("device 'plc' protocol_address: queue_size must be an integer from 1 to 65535"));
        assert!(err(queue_config("", "", ", queue_size = 0"))
            .contains("point 'own' address: queue_size must be an integer from 1 to 65535"));
        assert!(err(queue_config("", "", ", queue_size = 1.5")).contains("queue_size must be"));
        assert!(configure(&queue_config("queue_size = 65535", ", queue_size = 1", "")).is_ok());
    }

    /// One item per node and element, at the fastest interval and the largest queue size of its
    /// points, in the order first seen.
    #[test]
    fn grouping_merges_points_on_one_node() {
        let ms = Duration::from_millis;
        let a = NodeId::new(2, "A");
        let b = NodeId::new(2, "B");
        let items = group_monitored(
            [
                (a.clone(), None, ms(1000), 1),
                (b.clone(), None, ms(500), 16),
                (a.clone(), None, ms(200), 4),
                (a.clone(), Some(3), ms(100), 2),
            ]
            .into_iter(),
        );
        assert_eq!(
            items,
            vec![
                MonitoredItem { node_id: a.clone(), index: None, interval: ms(200), queue_size: 4 },
                MonitoredItem { node_id: b, index: None, interval: ms(500), queue_size: 16 },
                MonitoredItem { node_id: a, index: Some(3), interval: ms(100), queue_size: 2 },
            ]
        );
    }

    #[test]
    fn grouping_is_linear() {
        // 30,000 distinct nodes: quadratic grouping took seconds here, linear takes milliseconds.
        let started = std::time::Instant::now();
        let items = group_monitored(
            (0..30_000u32).map(|i| (NodeId::new(2, i), None, Duration::from_millis(100), 16)),
        );
        assert_eq!(items.len(), 30_000);
        assert!(started.elapsed() < Duration::from_secs(2), "{:?}", started.elapsed());
    }

    /// Batches of at most `batch`, results in order; the first failed batch ends it.
    #[tokio::test]
    async fn batches_keep_order_and_stop_at_a_failure() {
        let mut sizes = Vec::new();
        let results: Result<Vec<u32>, &str> =
            create_in_batches((0..1200).collect(), 500, |batch: Vec<u32>| {
                sizes.push(batch.len());
                async move { Ok(batch) }
            })
            .await;
        assert_eq!(sizes, vec![500, 500, 200]);
        assert_eq!(results.unwrap(), (0..1200).collect::<Vec<_>>());

        let mut calls = 0;
        let failed: Result<Vec<u32>, &str> = create_in_batches((0..1200).collect(), 500, |batch: Vec<u32>| {
            calls += 1;
            let n = calls;
            async move { if n == 2 { Err("BadTooManyOperations") } else { Ok(batch) } }
        })
        .await;
        assert_eq!(failed, Err("BadTooManyOperations"));
        assert_eq!(calls, 2, "nothing is sent after a failed batch");
    }

    /// The block size is ours, or the server's limit when that is lower; 0 or none is no limit.
    #[test]
    fn the_block_size_follows_a_lower_server_limit() {
        assert_eq!(nodes_per_read(None), NODES_PER_READ);
        assert_eq!(nodes_per_read(Some(0)), NODES_PER_READ);
        assert_eq!(nodes_per_read(Some(100)), 100);
        assert_eq!(nodes_per_read(Some(10_000)), NODES_PER_READ);
    }

    /// Blocks of at most `block`, one result per item in order.
    #[tokio::test]
    async fn blocks_keep_order() {
        let mut sizes = Vec::new();
        let results = read_in_blocks((0..1200).collect(), 500, |block: Vec<u32>| {
            sizes.push(block.len());
            async move { Ok(block) }
        })
        .await;
        assert_eq!(sizes, vec![500, 500, 200]);
        assert_eq!(results, ((0..1200).map(Ok).collect::<Vec<Result<u32, String>>>(), None));
    }

    /// A block the server refuses fails its own items only; the next blocks are still read.
    #[tokio::test]
    async fn a_refused_block_fails_only_its_items() {
        let mut calls = 0;
        let (results, lost) = read_in_blocks((0..1200).collect(), 500, |block: Vec<u32>| {
            calls += 1;
            let n = calls;
            async move {
                if n == 2 {
                    Err(BlockFailure { reason: "read failed: BadTooManyOperations".into(), link_lost: false })
                } else {
                    Ok(block)
                }
            }
        })
        .await;
        assert_eq!(lost, None);
        assert_eq!(calls, 3);
        assert_eq!(results.len(), 1200);
        assert!(results[..500].iter().all(Result::is_ok));
        assert!(results[500..1000].iter().all(|r| r == &Err("read failed: BadTooManyOperations".to_string())));
        assert_eq!(results[1000..], (1000..1200).map(Ok).collect::<Vec<_>>()[..]);
    }

    /// A block that finds the link gone ends the read, and nothing after it is sent: each further
    /// block would only wait for its own timeout. Before any value was read, every item fails
    /// with the reason, as one failed request did.
    #[tokio::test]
    async fn a_lost_link_fails_every_item_unsent() {
        let mut calls = 0;
        let (results, lost) = read_in_blocks((0..1200).collect(), 500, |_: Vec<u32>| {
            calls += 1;
            async move {
                Err::<Vec<u32>, _>(BlockFailure {
                    reason: "read timed out after 1s (transport down?)".into(),
                    link_lost: true,
                })
            }
        })
        .await;
        assert_eq!(lost, None, "nothing was read, so every item fails instead");
        assert_eq!(calls, 1, "nothing is sent after the link is gone");
        assert_eq!(results.len(), 1200);
        assert!(results.iter().all(|r| r == &Err("read timed out after 1s (transport down?)".to_string())));
    }

    /// A link lost after a block was read ends the results before the failed block and returns
    /// the reason, for `read_points` to report the points read so far as `LinkLost`.
    #[tokio::test]
    async fn a_link_lost_mid_read_returns_what_was_read() {
        let mut calls = 0;
        let results = read_in_blocks((0..1200).collect(), 500, |block: Vec<u32>| {
            calls += 1;
            let n = calls;
            async move {
                if n == 2 {
                    Err(BlockFailure { reason: "read failed: BadConnectionClosed".into(), link_lost: true })
                } else {
                    Ok(block)
                }
            }
        })
        .await;
        assert_eq!(calls, 2, "nothing is sent after the link is gone");
        assert_eq!(results.0, (0..500).map(Ok).collect::<Vec<Result<u32, String>>>());
        assert_eq!(results.1, Some("read failed: BadConnectionClosed".to_string()));
    }

    /// A block answered with fewer values than it asked for fails the missing ones, and one
    /// answered with more does not shift the next block's results.
    #[tokio::test]
    async fn a_short_or_long_answer_keeps_items_aligned() {
        let results = read_in_blocks((0..6).collect(), 3, |block: Vec<u32>| async move {
            if block[0] == 0 {
                Ok(block[..2].to_vec())
            } else {
                Ok([block.clone(), vec![99]].concat())
            }
        })
        .await
        .0;
        let expected: Vec<Result<u32, String>> =
            vec![Ok(0), Ok(1), Err("no value returned".into()), Ok(3), Ok(4), Ok(5)];
        assert_eq!(results, expected);
    }

    #[test]
    fn a_refusal_is_not_a_lost_link() {
        assert!(link_lost(StatusCode::BadConnectionClosed));
        assert!(link_lost(StatusCode::BadTimeout));
        assert!(!link_lost(StatusCode::BadTooManyOperations));
        assert!(!link_lost(StatusCode::BadRequestTooLarge));
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
        let ts = data_value_source_ts(&dv).unwrap();
        assert_eq!((ts.year(), u8::from(ts.month()), ts.day()), (2026, 1, 2));
        let dv = DataValue { server_timestamp: Some(server), ..Default::default() };
        let ts = data_value_source_ts(&dv).unwrap();
        assert_eq!((ts.year(), u8::from(ts.month()), ts.day()), (2026, 6, 7));
    }

    #[test]
    fn data_value_without_timestamps_has_no_source_time() {
        assert!(data_value_source_ts(&DataValue::default()).is_none());
    }

    /// A device with a namespace table, a method point and `point` (TOML) as a second point.
    fn method_config(method: &str, point: &str) -> String {
        format!(
            r#"
            [connector]
            protocol = "opcua"
            [[device]]
            name = "plc"
            protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:4840", namespaces = ["http://opcfoundation.org/UA/", "urn:a", "urn:machine"] }}
              [[device.point]]
              id = "multiply"
              access = "call_method"
              address = {{ {method} }}
            {point}
            "#
        )
    }

    const MULTIPLY: &str =
        r#"object_id = "ns=2;s=Machine", method_id = "ns=2;s=Machine.Multiply", inputs = ["int32", "int64"]"#;

    /// A method point needs no datatype; its method is its node, its object and inputs the rest.
    #[test]
    fn method_points_are_configured() {
        let c = configure(&method_config(MULTIPLY, "")).unwrap();
        let point = &c.devices["plc"].points["multiply"];
        assert_eq!(point.node_id, NodeId::new(2, "Machine.Multiply"));
        assert_eq!(point.access, Access::CallMethod);
        assert!(point.table.is_some());
        let method = point.method.as_ref().unwrap();
        assert_eq!(method.object, NodeId::new(2, "Machine"));
        assert_eq!(method.inputs, [DataType::Int32, DataType::Int64]);

        let bare = r#"object_id = "i=2253", method_id = "i=11492""#;
        let c = configure(&method_config(bare, "")).unwrap();
        let point = &c.devices["plc"].points["multiply"];
        assert!(point.method.as_ref().unwrap().inputs.is_empty());
        assert!(point.table.is_none(), "namespace 0 is never translated");
    }

    /// Spec "Method addressing": every refused address, with the point it names.
    #[test]
    fn method_addresses_are_validated() {
        let refused = |method: &str, point: &str| match configure(&method_config(method, point)) {
            Err(ConfigError::Invalid(e)) => e,
            Ok(_) => panic!("accepted: {method} {point}"),
        };
        let o = r#"object_id = "ns=2;s=Machine""#;
        let m = r#"method_id = "ns=2;s=Machine.Multiply""#;
        assert_eq!(
            refused(o, ""),
            "point 'multiply' address: method_id is required for a call_method point"
        );
        assert_eq!(
            refused(m, ""),
            "point 'multiply' address: object_id is required for a call_method point"
        );
        assert_eq!(
            refused(&format!(r#"object_id = "ns=2;x=1", {m}"#), ""),
            "point 'multiply' address: invalid object_id 'ns=2;x=1'"
        );
        for key in [
            r#"node_id = "ns=2;s=A""#,
            "namespace = 2",
            r#"identifier = "A""#,
            r#"field = "A""#,
            "index = 1",
            "queue_size = 2",
        ] {
            let name = key.split(' ').next().unwrap();
            assert_eq!(
                refused(&format!("{o}, {m}, {key}"), ""),
                format!("point 'multiply' address: {name} does not apply to a call_method point")
            );
        }
        assert_eq!(
            refused(&format!(r#"{o}, {m}, inputs = "int32""#), ""),
            "point 'multiply' address: inputs must be an array of datatypes"
        );
        assert_eq!(
            refused(&format!(r#"{o}, {m}, inputs = ["int32", "float"]"#), ""),
            "point 'multiply' address: inputs[1]: unknown datatype 'float'"
        );
        assert_eq!(
            refused(&format!(r#"{o}, {m}, inputs = ["bytes"]"#), ""),
            "point 'multiply' address: inputs[0]: bytes is not supported as a method argument"
        );
        assert_eq!(
            refused(&format!(r#"object_id = "ns=7;s=Machine", {m}"#), ""),
            "point 'multiply': namespace index 7 is not in the namespace table (3 entries)"
        );
        for key in [r#"object_id = "ns=2;s=M""#, r#"method_id = "ns=2;s=M""#, r#"inputs = []"#] {
            let name = key.split(' ').next().unwrap();
            let variable = format!(
                "[[device.point]]\nid = \"temp\"\ndatatype = \"float64\"\naddress = {{ node_id = \"ns=2;s=T\", {key} }}"
            );
            assert_eq!(
                refused(MULTIPLY, &variable),
                format!("point 'temp' address: {name} applies only to a call_method point")
            );
        }
    }

    /// Both ids of a method point go through the namespace table: a moved namespace moves both,
    /// and one missing on the server is the call's reason (spec "Namespace translation").
    #[test]
    fn method_ids_are_translated_through_the_table() {
        let object = r#"object_id = "ns=1;s=Line", method_id = "ns=2;s=Machine.Multiply""#;
        let c = configure(&method_config(object, "")).unwrap();
        let points = &c.devices["plc"].points;
        let model = &points["multiply"];
        let method = model.method.as_ref().unwrap();

        let translate = |server: &[&str]| {
            let mut translation = namespaces::Translation::default();
            let keyed = translation_keys(points, &translation, false);
            let mapped: Vec<namespaces::Mapped> = keyed
                .iter()
                .map(|(key, node, table)| namespaces::Mapped { point: key, node, table })
                .collect();
            let server = Ok(server.iter().map(|u| u.to_string()).collect());
            translation.apply(&mapped, &server);
            effective_method("multiply", model, method, &translation)
        };

        let (object, method_id) =
            translate(&["http://opcfoundation.org/UA/", "urn:x", "urn:machine", "urn:a"]).unwrap();
        assert_eq!(object, NodeId::new(3, "Line"));
        assert_eq!(method_id, NodeId::new(2, "Machine.Multiply"));

        assert_eq!(
            translate(&["http://opcfoundation.org/UA/", "urn:machine"]).unwrap_err(),
            "namespace not found: urn:a"
        );
        assert_eq!(
            effective_method("multiply", model, method, &namespaces::Translation::default()).unwrap_err(),
            "namespace not translated"
        );
    }

    /// Spec "Input arguments": the count first, then each value as a write converts it.
    #[test]
    fn method_arguments_are_converted_by_their_inputs() {
        let inputs = [DataType::Int32, DataType::Uint64, DataType::String];
        let args = |v: serde_json::Value| method_arguments(&inputs, v.as_array().unwrap());
        assert_eq!(
            args(serde_json::json!([6, "18446744073709551615", "x"])).unwrap(),
            [Variant::Int32(6), Variant::UInt64(u64::MAX), Variant::String("x".into())]
        );
        assert_eq!(args(serde_json::json!([6])).unwrap_err(), "expected 3 arguments, got 1");
        // The C build's reasons, word for word.
        assert_eq!(
            args(serde_json::json!(["six", 1, "x"])).unwrap_err(),
            r#"argument 1: "six" is not a valid int32"#
        );
        assert_eq!(
            args(serde_json::json!([2147483648i64, 1, "x"])).unwrap_err(),
            "argument 1: 2147483648 is not a valid int32"
        );
        assert_eq!(args(serde_json::json!([1.5, 1, "x"])).unwrap_err(), "argument 1: 1.5 is not a valid int32");
        assert_eq!(args(serde_json::json!([6, -1, "x"])).unwrap_err(), "argument 2: -1 is not a valid uint64");
        assert_eq!(
            args(serde_json::json!([6, 9007199254740992u64, "x"])).unwrap_err(),
            "argument 2: 9007199254740992 is not a valid uint64"
        );
        assert_eq!(args(serde_json::json!([6, 1, 2])).unwrap_err(), "argument 3: 2 is not a valid string");
        assert!(method_arguments(&[], &[]).unwrap().is_empty());
    }

    /// Spec "Output arguments".
    #[test]
    fn method_outputs_render_as_json() {
        use serde_json::json;
        assert_eq!(output_json(&Variant::Int32(42)), json!(42));
        assert_eq!(output_json(&Variant::Boolean(true)), json!(true));
        assert_eq!(output_json(&Variant::Double(1.5)), json!(1.5));
        assert_eq!(output_json(&Variant::Double(f64::NAN)), json!(null));
        assert_eq!(output_json(&Variant::String("ok".into())), json!("ok"));
        assert_eq!(output_json(&Variant::Int64(i64::MAX)), json!(i64::MAX.to_string()));
        assert_eq!(output_json(&Variant::UInt64(7)), json!(7));
        let array = Variant::from(vec![1i32, 2, 3]);
        assert_eq!(output_json(&array), json!([1, 2, 3]));
        assert_eq!(output_json(&Variant::from(NodeId::new(2, "x"))), json!(null));
    }
}
