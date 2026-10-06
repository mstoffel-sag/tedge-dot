//! SNMP connector module (spec: `doc/connectors/snmp-connector-spec.md`).
//!
//! One module talks to SNMP equipment both ways a site needs: object points are polled with GET
//! or GETBULK and written with SET, while trap and varbind points are delivered by the
//! notifications the equipment sends (v1/v2c/v3 traps and informs, received on one UDP socket,
//! directly or through a trusted forwarder).
//!
//! The driver stays "dumb": what a notification means — an event, an alarm, a measurement — is
//! declared on the point's `meta` and handled by the flows.
//!
//! The protocol itself is [`snmp2`]'s (patched in `impl/rust/vendor/snmp2`); this crate owns
//! point matching, the notification semantics of spec §4.3, and the datatype conversions of §6.

pub mod config;
pub mod listener;
pub mod notification;
pub mod poll;
pub mod value;

pub use config::{Device, Point, PointKind, SnmpType, SnmpVersion};
/// The protocol library, so tests and tools speak the same (patched) SNMP as the connector.
pub use snmp2;
pub use notification::Notification;
pub use value::{Oid, VarValue, Varbind};

use async_trait::async_trait;
use config::Connection;
use listener::{Route, Routes, V3Receive};
use poll::{error_name, RequestError, Session};
use snmp2::v3::{LocalEngine, SecurityLevel};
use snmp2::Value as SnmpValue;
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, Mutex};
use tedge_dot_sdk::{
    Access, Capabilities, CommandRequest, CommandResult, ConfigError, Connector, ConnectorConfig,
    ConnectorError, DataType, DeviceId, LinkReport, LinkStatus, Mode, PointRef, Quality, Sample,
    SampleSink, Value,
};
use time::OffsetDateTime;
use tokio::task::JoinHandle;
use tracing::{debug, info, warn};

const PROTOCOL: &str = "snmp";

/// Longest OCTET STRING a SET may carry: a single datagram has to hold it.
const MAX_WRITE_OCTETS: usize = 32_768;

#[derive(Default)]
pub struct SnmpConnector {
    conn: Option<Connection>,
    devices: Vec<Device>,
    /// Where each device was resolved to, once connected.
    resolved: HashMap<usize, SocketAddr>,
    /// Request sessions, created on first use and dropped on reconnect.
    sessions: HashMap<usize, Session>,
    /// A second session for SET when the write community differs from the read community.
    write_sessions: HashMap<usize, Session>,
    routes: Arc<Mutex<Routes>>,
    listener: Option<JoinHandle<()>>,
    /// Set by [`Connector::disable_push`]: a CLI read or write never binds the listen port.
    push_disabled: bool,
    /// This receiver's SNMP engine, authoritative for v3 informs. Kept across reconnects: its
    /// boots and time are what senders synchronise to.
    engine: Option<LocalEngine>,
}

pub fn factory() -> Box<dyn Connector> {
    Box::<SnmpConnector>::default()
}

#[async_trait]
impl Connector for SnmpConnector {
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError> {
        let conn = config::connection(&config.connection)
            .map_err(|e| ConfigError::Invalid(format!("connection: {e}")))?;
        let mut devices = Vec::with_capacity(config.devices.len());
        for d in &config.devices {
            let invalid = |msg: String| ConfigError::Invalid(format!("device '{}': {msg}", d.name));
            let mut points = Vec::with_capacity(d.points.len());
            for p in &d.points {
                points.push(
                    config::point(p, d.default_mode)
                        .map_err(|e| invalid(format!("point '{}': {e}", p.id)))?,
                );
            }
            devices.push(
                config::device(&d.name, &d.protocol_address, &conn, points).map_err(invalid)?,
            );
        }
        config::check_unique_hosts(&devices, &conn.forwarders).map_err(ConfigError::Invalid)?;

        let engine_changed = self
            .engine
            .as_ref()
            .is_none_or(|engine| engine.engine_id() != conn.engine_id.as_slice());
        if engine_changed {
            self.engine = Some(
                LocalEngine::new(&conn.engine_id, config::engine_boots())
                    .map_err(|e| ConfigError::Invalid(format!("connection.engine_id: {e}")))?,
            );
        }
        self.conn = Some(conn);
        self.devices = devices;
        Ok(())
    }

    /// SNMPv3 password files are read at configure: a command must not point them elsewhere.
    fn local_only_settings(&self) -> &'static [&'static str] {
        &["auth_password_file", "priv_password_file"]
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
            ],
            point_kinds: vec!["object".into(), "trap".into(), "varbind".into()],
            command_verbs: vec!["write".into()],
            features: vec![
                "polling".into(),
                "subscribe".into(),
                "bulk_read".into(),
                "snmpv3".into(),
            ],
            subscribe: true,
        }
    }

    /// Notification points are pushed; object points are polled (spec §10). Without this the
    /// runtime would take a device's object points off the polling schedule as well, and nothing
    /// would ever read them.
    fn pushes_point(&self, device: &DeviceId, point: &PointRef) -> bool {
        self.devices
            .iter()
            .find(|d| &d.name == device)
            .and_then(|d| d.points.iter().find(|p| p.id == point.id))
            .is_some_and(Point::is_pushed)
    }

    fn disable_push(&mut self) {
        self.push_disabled = true;
    }

    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError> {
        self.stop_listener().await;
        self.sessions.clear();
        self.write_sessions.clear();
        self.resolved.clear();

        let conn = self
            .conn
            .clone()
            .ok_or_else(|| ConnectorError::Other("the connector is not configured".into()))?;
        let engine = self.engine.clone();
        let devices: Vec<Route> = self
            .devices
            .iter()
            .map(|device| Route {
                name: device.name.clone(),
                communities: device.accepted.clone(),
                v3: v3_receive(device, engine.as_ref()),
                points: Vec::new(),
                sink: None,
            })
            .collect();
        *listener::lock(&self.routes) = Routes {
            devices,
            engine,
            forwarders: resolve_forwarders(&conn.forwarders).await,
            ..Routes::default()
        };

        // No notification point, no socket: the packaged default configuration has no device,
        // and must not take port 162 from a host's own trap receiver just by being installed.
        let listening = if self.wants_listener() {
            self.start_listener()
        } else {
            Ok(())
        };
        let mut reports = Vec::with_capacity(self.devices.len());
        for index in 0..self.devices.len() {
            reports.push(match &listening {
                Ok(()) => self.attach(index).await,
                Err(e) => self.report(index, LinkStatus::Disconnected, Some(e.clone())),
            });
        }
        Ok(reports)
    }

    async fn read_points(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
    ) -> Result<Vec<Sample>, ConnectorError> {
        let index = self.index_of(device)?;
        let model = self.devices[index].clone();
        // Notification points are delivered by the listener, and a write-only object cannot be
        // read: neither produces a sample here.
        let wanted: Vec<Point> = points
            .iter()
            .filter_map(|r| model.points.iter().find(|p| p.id == r.id))
            .filter(|p| p.object_oid().is_some() && p.access != Access::Write)
            .cloned()
            .collect();
        if wanted.is_empty() {
            return Ok(Vec::new());
        }
        let ts = OffsetDateTime::now_utc();
        let session = self.session(index).await?;
        let samples = read_objects(session, &model, &wanted, ts).await;
        // §3.2: a v3 trap is only accepted from the device's engine once that engine is known --
        // configured, learned from a successful request, or from the first authenticated trap.
        // The request case was discovered but never used, so a device with no `engine_id` in its
        // config accepted whatever engine a trap claimed even after a poll had established the
        // real one.
        //
        // Only pin it once a request has actually AUTHENTICATED with it. `Session::discover`
        // learns the engine ID from the RFC 3414 §4 probe, which carries no HMAC -- anything
        // answering at the device's address can choose it. Pinning that would hand an attacker
        // the same permanent trap rejection through the poll path that TEDGE-DOT-PATCH(8)
        // closes on the listener path: every genuine trap would then fail `EngineIdMismatch`
        // until the connector restarts.
        //
        // A good sample is that proof only from authNoPriv up, where the response carried an
        // HMAC over keys localized to this engine. At `level = "noAuthNoPriv"` nothing is
        // verified at all (`Security::need_auth` is false), so a good sample says only that
        // something answered -- exactly the case this guard exists to exclude.
        let authenticated = model
            .v3
            .as_ref()
            .is_some_and(|v3| v3.level >= SecurityLevel::AuthNoPriv);
        let verified = authenticated && samples.iter().any(|s| s.quality != Quality::Bad);
        let discovered = verified.then(|| session.engine_id().map(<[u8]>::to_vec)).flatten();
        if let Some(engine_id) = discovered {
            let mut routes = listener::lock(&self.routes);
            if let Some(v3) = routes.devices.get_mut(index).and_then(|r| r.v3.as_mut()) {
                // Re-pin when it CHANGES, not only when it is empty: an agent that reboots with
                // a new engine ID (one derived from a MAC or a boot identity, so a redeployed
                // container has a different one) would otherwise keep the old pin for the life
                // of the process -- every trap failing `EngineIdMismatch` behind a link the
                // runtime reports as connected, with no reconnect able to repair it, since
                // `reconnect` re-attaches without rebuilding this state. An authenticated poll
                // is as good a source for the new engine as it was for the first.
                if v3.trap.engine_id() != engine_id.as_slice() {
                    match v3.trap.clone().with_engine_id(&engine_id) {
                        Ok(mut localized) => {
                            // The boots and time belong to the OLD engine. A rebooted agent
                            // starts again from boots 1, which is BELOW what we learned from
                            // its predecessor, so the replay check would refuse every trap it
                            // sends -- swapping one permanent rejection for another. The
                            // learning branch in `authenticate_non_authoritative` zeroes them
                            // for exactly this reason; a re-pin is the same event.
                            localized.reset_engine_counters();
                            v3.trap = localized;
                        }
                        Err(e) => debug!(
                            device = %model.name,
                            "cannot localize the trap keys to the discovered engine: {e}"
                        ),
                    }
                }
            }
        }
        let mut by_id: HashMap<String, Sample> =
            samples.into_iter().map(|s| (s.point.clone(), s)).collect();
        Ok(points.iter().filter_map(|r| by_id.remove(&r.id)).collect())
    }

    async fn subscribe(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
        sink: SampleSink,
    ) -> Result<(), ConnectorError> {
        let index = self.index_of(device)?;
        let model = &self.devices[index];
        for r in points {
            let known = model.points.iter().find(|p| p.id == r.id);
            match known {
                None => {
                    return Err(ConnectorError::UnknownPoint {
                        device: device.clone(),
                        point: r.id.clone(),
                    })
                }
                Some(point) if !point.is_pushed() => {
                    return Err(ConnectorError::Other(format!(
                        "point '{}' is an object: it is polled, not pushed",
                        r.id
                    )))
                }
                Some(_) => {}
            }
        }
        let wanted: Vec<Point> = model
            .points
            .iter()
            .filter(|p| points.iter().any(|r| r.id == p.id))
            .cloned()
            .collect();
        let mut routes = listener::lock(&self.routes);
        let route = routes
            .devices
            .get_mut(index)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))?;
        route.points = wanted;
        route.sink = Some(sink);
        Ok(())
    }

    async fn check_subscription(&mut self, device: &DeviceId) -> Result<(), ConnectorError> {
        self.index_of(device)?;
        match &self.listener {
            Some(task) if !task.is_finished() => Ok(()),
            _ => Err(ConnectorError::Transport(
                listener::lock(&self.routes)
                    .listener_error
                    .clone()
                    .unwrap_or_else(|| "the SNMP listener is not running".into()),
            )),
        }
    }

    async fn reconnect(&mut self, device: &DeviceId) -> Result<LinkReport, ConnectorError> {
        let index = self.index_of(device)?;
        // A request session outlives nothing useful: the next one rediscovers the v3 engine and
        // starts from a fresh socket.
        self.sessions.remove(&index);
        self.write_sessions.remove(&index);
        if self.wants_listener()
            && self.listener.as_ref().is_none_or(|task| task.is_finished())
        {
            self.stop_listener().await;
            if let Err(e) = self.start_listener() {
                return Ok(self.report(index, LinkStatus::Disconnected, Some(e)));
            }
        }
        Ok(self.attach(index).await)
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
        let index = self.index_of(device)?;
        let model = self.devices[index].clone();
        let point = model
            .points
            .iter()
            .find(|p| p.id == request.point)
            .ok_or_else(|| ConnectorError::UnknownPoint {
                device: device.clone(),
                point: request.point.clone(),
            })?;
        let oid = point.object_oid().ok_or_else(|| {
            ConnectorError::AccessDenied(format!(
                "{}: a notification point cannot be written",
                request.point
            ))
        })?;
        if !point.access.can_write() {
            return Err(ConnectorError::AccessDenied(request.point.clone()));
        }
        let snmp_type = point.snmp_type().ok_or_else(|| {
            ConnectorError::Decode(format!("point '{}' has no SNMP type to write", request.point))
        })?;
        let encoded = encode_write(snmp_type, point.datatype, request).map_err(ConnectorError::Decode)?;
        let name = oid.to_library();

        let session = self.write_session(index).await?;
        let reply = session
            .set(&[(&name, encoded.value(snmp_type))])
            .await
            .map_err(|e| match e {
                RequestError::Auth(_) | RequestError::Transport(_) | RequestError::Timeout { .. } => {
                    ConnectorError::Transport(e.to_string())
                }
                RequestError::Protocol(_) => ConnectorError::Decode(e.to_string()),
            })?;
        if reply.error_status != 0 {
            return Err(ConnectorError::Other(format!(
                "the agent refused the SET of {oid}: {} (error-status {}, error-index {})",
                error_name(reply.error_status),
                reply.error_status,
                reply.error_index
            )));
        }
        Ok(CommandResult {
            point: request.point.clone(),
            value: request.value.clone(),
            raw: request.raw.clone(),
        })
    }

    async fn disconnect(&mut self) -> Result<(), ConnectorError> {
        self.stop_listener().await;
        self.sessions.clear();
        self.write_sessions.clear();
        self.resolved.clear();
        *listener::lock(&self.routes) = Routes::default();
        Ok(())
    }
}

impl SnmpConnector {
    fn index_of(&self, device: &DeviceId) -> Result<usize, ConnectorError> {
        self.devices
            .iter()
            .position(|d| &d.name == device)
            .ok_or_else(|| ConnectorError::NotConnected(device.clone()))
    }

    /// Link `info` (spec §3.2, §7): the address and version, never a credential.
    fn report(&self, index: usize, status: LinkStatus, reason: Option<String>) -> LinkReport {
        let device = &self.devices[index];
        LinkReport {
            device: device.name.clone(),
            status,
            reason,
            info: Some(serde_json::json!({
                "host": device.host,
                "port": device.port,
                "version": device.version.as_str(),
            })),
        }
    }

    /// Whether the notification socket is needed: some device has a trap or varbind point, and
    /// this is the service rather than a CLI read or write (which would steal the service's port).
    fn wants_listener(&self) -> bool {
        !self.push_disabled && self.devices.iter().any(Device::has_notifications)
    }

    fn start_listener(&mut self) -> Result<(), String> {
        let listen = self
            .conn
            .as_ref()
            .ok_or("the connector is not configured")?
            .listen;
        let socket = listener::bind(listen).map_err(|e| format!("cannot listen on udp {listen}: {e}"))?;
        info!("receiving SNMP notifications on udp {listen}");
        listener::lock(&self.routes).listener_error = None;
        self.listener = Some(tokio::spawn(listener::receive_loop(
            Arc::new(socket),
            self.routes.clone(),
        )));
        Ok(())
    }

    /// Stop the receive task and wait for it, so its socket is closed before another is bound
    /// to the same port.
    async fn stop_listener(&mut self) {
        if let Some(task) = self.listener.take() {
            task.abort();
            let _ = task.await;
        }
    }

    /// Resolve a device's host: the address requests go to, and the one its notifications are
    /// routed by.
    async fn attach(&mut self, index: usize) -> LinkReport {
        let (host, port) = {
            let device = &self.devices[index];
            (device.host.clone(), device.port)
        };
        let resolved = resolve(&host).await;
        let outcome = {
            let mut routes = listener::lock(&self.routes);
            routes.by_addr.retain(|_, owner| *owner != index);
            match resolved {
                Err(e) => Err(e),
                Ok(ip) => match routes.by_addr.get(&ip) {
                    Some(&other) => Err(format!(
                        "address {ip} already belongs to device '{}'",
                        routes.devices[other].name
                    )),
                    None => {
                        routes.by_addr.insert(ip, index);
                        Ok(ip)
                    }
                },
            }
        };
        match outcome {
            Ok(ip) => {
                self.resolved.insert(index, SocketAddr::new(ip, port));
                self.report(index, LinkStatus::Connected, None)
            }
            Err(e) => {
                self.resolved.remove(&index);
                warn!(device = %self.devices[index].name, "{e}");
                self.report(index, LinkStatus::Disconnected, Some(e))
            }
        }
    }

    /// The request session of a device, opened on first use.
    async fn session(&mut self, index: usize) -> Result<&mut Session, ConnectorError> {
        let address = self.address(index)?;
        if !self.sessions.contains_key(&index) {
            let session = {
                let device = &self.devices[index];
                Session::open(device, address, &device.community)
                    .await
                    .map_err(ConnectorError::Transport)?
            };
            self.sessions.insert(index, session);
        }
        Ok(self.sessions.get_mut(&index).expect("just inserted"))
    }

    /// The session a SET goes through: the read session, unless a write community is configured.
    async fn write_session(&mut self, index: usize) -> Result<&mut Session, ConnectorError> {
        let separate = {
            let device = &self.devices[index];
            device.version != SnmpVersion::V3 && device.write_community != device.community
        };
        if !separate {
            return self.session(index).await;
        }
        let address = self.address(index)?;
        if !self.write_sessions.contains_key(&index) {
            let session = {
                let device = &self.devices[index];
                Session::open(device, address, &device.write_community)
                    .await
                    .map_err(ConnectorError::Transport)?
            };
            self.write_sessions.insert(index, session);
        }
        Ok(self.write_sessions.get_mut(&index).expect("just inserted"))
    }

    fn address(&self, index: usize) -> Result<SocketAddr, ConnectorError> {
        self.resolved
            .get(&index)
            .copied()
            .ok_or_else(|| ConnectorError::NotConnected(self.devices[index].name.clone()))
    }
}

/// A device's v3 state for received notifications (§3.2): the same user localized for its own
/// engine (traps) and for ours (informs).
fn v3_receive(device: &Device, engine: Option<&LocalEngine>) -> Option<V3Receive> {
    let v3 = device.v3.as_ref()?;
    let mut trap = v3.security();
    if let Some(engine_id) = &v3.engine_id {
        match trap.with_engine_id(engine_id) {
            Ok(localized) => trap = localized,
            Err(e) => {
                warn!(device = %device.name, "cannot prepare the v3 trap keys: {e}");
                return None;
            }
        }
    }
    let inform = match engine.map(|engine| engine.localize(&v3.security())) {
        Some(Ok(inform)) => inform,
        Some(Err(e)) => {
            warn!(device = %device.name, "cannot prepare the v3 inform keys: {e}");
            return None;
        }
        None => return None,
    };
    Some(V3Receive { user: v3.user.clone(), level: v3.level, trap, inform })
}

async fn resolve_forwarders(names: &[String]) -> std::collections::HashSet<IpAddr> {
    let mut out = std::collections::HashSet::new();
    for name in names {
        match resolve(name).await {
            Ok(ip) => {
                out.insert(ip);
            }
            Err(e) => warn!("trusted forwarder: {e}"),
        }
    }
    out
}

async fn resolve(host: &str) -> Result<IpAddr, String> {
    if let Ok(ip) = host.parse::<IpAddr>() {
        return Ok(listener::normalize(ip));
    }
    let mut addrs = tokio::net::lookup_host((host, 0))
        .await
        .map_err(|e| format!("cannot resolve host '{host}': {e}"))?;
    addrs
        .next()
        .map(|a| listener::normalize(a.ip()))
        .ok_or_else(|| format!("host '{host}' resolves to no address"))
}

// ─── polling ─────────────────────────────────────────────────────────────────────────────────

/// Read every object point of one cycle (spec §5.1), in batches of `max_varbinds`.
async fn read_objects(
    session: &mut Session,
    model: &Device,
    points: &[Point],
    ts: OffsetDateTime,
) -> Vec<Sample> {
    // GETBULK's non-repeaters are answered with the successor of each requested OID, so only a
    // scalar instance (`x.0`) can be fetched that way — by asking for its parent.
    let (bulk, get): (Vec<&Point>, Vec<&Point>) = if model.bulk {
        points.iter().partition(|p| scalar_parent(p).is_some())
    } else {
        (Vec::new(), points.iter().collect())
    };
    let mut out = Vec::with_capacity(points.len());
    let mut dead: Option<String> = None;
    for (bulky, group) in [(true, bulk), (false, get)] {
        for batch in group.chunks(model.max_varbinds) {
            if let Some(reason) = &dead {
                out.extend(batch.iter().map(|p| bad(&model.name, p, ts, format!("skipped: {reason}"))));
                continue;
            }
            let (samples, failure) = read_batch(session, model, bulky, batch, ts).await;
            out.extend(samples);
            if let Some(e) = failure {
                if e.stops_the_cycle() {
                    dead = Some(e.to_string());
                }
            }
        }
    }
    out
}

/// One request: GETBULK over the parents of scalars, or GET (with the SNMPv1 error-index dance).
async fn read_batch(
    session: &mut Session,
    model: &Device,
    bulky: bool,
    batch: &[&Point],
    ts: OffsetDateTime,
) -> (Vec<Sample>, Option<RequestError>) {
    if bulky {
        let parents: Vec<Oid> = batch.iter().filter_map(|p| scalar_parent(p)).collect();
        return match session.get_bulk(&parents).await {
            Ok(reply) => (map_reply(model, batch, reply, ts), None),
            Err(e) => (failed_batch(model, batch, ts, &e), Some(e)),
        };
    }
    if model.version != SnmpVersion::V1 {
        let oids: Vec<Oid> = batch.iter().filter_map(|p| p.object_oid().cloned()).collect();
        return match session.get(&oids).await {
            Ok(reply) => (map_reply(model, batch, reply, ts), None),
            Err(e) => (failed_batch(model, batch, ts, &e), Some(e)),
        };
    }

    // SNMPv1 has no per-varbind exceptions: an unserved OID fails the whole request with
    // noSuchName and an error-index. That point is bad, and the rest are asked again without it.
    let mut pending: Vec<&Point> = batch.to_vec();
    let mut samples = Vec::with_capacity(batch.len());
    while !pending.is_empty() {
        let oids: Vec<Oid> = pending.iter().filter_map(|p| p.object_oid().cloned()).collect();
        match session.get(&oids).await {
            Err(e) => {
                samples.extend(failed_batch(model, &pending, ts, &e));
                return (samples, Some(e));
            }
            Ok(reply) if reply.error_status == 0 => {
                samples.extend(map_reply(model, &pending, reply, ts));
                return (samples, None);
            }
            Ok(reply) => {
                let name = error_name(reply.error_status);
                let index = reply.error_index as usize;
                if index >= 1 && index <= pending.len() {
                    let point = pending.remove(index - 1);
                    samples.push(bad(
                        &model.name,
                        point,
                        ts,
                        format!("the agent answered {name} (error-status {})", reply.error_status),
                    ));
                    continue;
                }
                samples.extend(pending.iter().map(|p| {
                    bad(
                        &model.name,
                        p,
                        ts,
                        format!(
                            "the agent answered {name} (error-status {}, error-index {})",
                            reply.error_status, reply.error_index
                        ),
                    )
                }));
                return (samples, None);
            }
        }
    }
    (samples, None)
}

/// One sample per point of the batch, from the varbinds that came back in the same order.
fn map_reply(model: &Device, batch: &[&Point], reply: poll::Reply, ts: OffsetDateTime) -> Vec<Sample> {
    if reply.error_status != 0 {
        let name = error_name(reply.error_status);
        return batch
            .iter()
            .map(|p| {
                bad(
                    &model.name,
                    p,
                    ts,
                    format!(
                        "the agent answered {name} (error-status {}, error-index {})",
                        reply.error_status, reply.error_index
                    ),
                )
            })
            .collect();
    }
    batch
        .iter()
        .enumerate()
        .map(|(i, point)| {
            let oid = point.object_oid().expect("object points only");
            match reply.varbinds.get(i) {
                None => bad(&model.name, point, ts, "the agent's response has no varbind for it".into()),
                Some((name, _)) if name != oid => bad(
                    &model.name,
                    point,
                    ts,
                    format!("the agent answered with {name} instead of {oid}"),
                ),
                Some((_, value)) if value.kind.is_exception() => {
                    let reason = format!("the agent answered {} for {oid}", value.kind.name());
                    bad(&model.name, point, ts, reason)
                }
                Some((_, value)) => {
                    // Raw mode publishes the octets and nothing else, so it must not run the
                    // typed conversion: a point carrying BOTH `mode = "raw"` and a `datatype`
                    // (which the config permits, and which the contract says makes the
                    // decoding fields inert) otherwise failed to convert and reported a read
                    // that had succeeded as a bad sample.
                    let converted = match (point.mode, point.datatype) {
                        (Mode::Raw, _) | (_, None) => Ok(Value::Bool(false)),
                        (_, Some(datatype)) => {
                            value.convert(datatype).map(|v| point.transform.apply(v))
                        }
                    };
                    sample(
                        &model.name,
                        point,
                        ts,
                        serde_json::json!({ "oid": oid.to_string() }),
                        value.raw.clone(),
                        converted,
                    )
                }
            }
        })
        .collect()
}

fn failed_batch(
    model: &Device,
    batch: &[&Point],
    ts: OffsetDateTime,
    error: &RequestError,
) -> Vec<Sample> {
    batch.iter().map(|p| bad(&model.name, p, ts, error.to_string())).collect()
}

/// The parent of a scalar instance OID (`x` for `x.0`), which GETBULK asks for.
fn scalar_parent(point: &Point) -> Option<Oid> {
    let oid = point.object_oid()?;
    (oid.arcs().last() == Some(&0)).then(|| oid.parent()).flatten()
}

fn bad(device: &str, point: &Point, ts: OffsetDateTime, reason: String) -> Sample {
    let addr = match point.object_oid() {
        Some(oid) => serde_json::json!({ "oid": oid.to_string() }),
        None => serde_json::json!({}),
    };
    sample(device, point, ts, addr, Vec::new(), Err(reason))
}

/// Shape a sample. In raw mode it is good as long as there are octets; in typed mode the
/// conversion decides.
pub(crate) fn sample(
    device: &str,
    point: &Point,
    ts: OffsetDateTime,
    addr: serde_json::Value,
    raw: Vec<u8>,
    value: Result<Value, String>,
) -> Sample {
    let (value, quality, error, datatype) = match (point.mode, value) {
        (Mode::Raw, Err(e)) => (None, Quality::Bad, Some(e), None),
        (Mode::Raw, Ok(_)) => (None, Quality::Good, None, None),
        (Mode::Typed, Ok(v)) => (Some(v), Quality::Good, None, point.datatype),
        (Mode::Typed, Err(e)) => (None, Quality::Bad, Some(e), point.datatype),
    };
    Sample {
        source_value: None,
        ts,
        device: device.to_string(),
        protocol: PROTOCOL,
        point: point.id.clone(),
        mode: point.mode,
        datatype,
        value,
        raw,
        raw_group: 1,
        quality,
        unit: point.unit.clone(),
        addr,
        seq: None,
        error,
    }
}

// ─── writes ──────────────────────────────────────────────────────────────────────────────────

/// A value to write, owned so the library can borrow it for the request.
#[derive(Debug, PartialEq, Eq)]
pub enum Encoded {
    Int(i64),
    Unsigned(u32),
    Big(u64),
    Bytes(Vec<u8>),
    Ip([u8; 4]),
    Oid(Vec<u8>),
}

impl Encoded {
    fn value(&self, snmp_type: SnmpType) -> SnmpValue<'_> {
        match self {
            Encoded::Int(v) => SnmpValue::Integer(*v),
            Encoded::Unsigned(v) => match snmp_type {
                SnmpType::Counter32 => SnmpValue::Counter32(*v),
                SnmpType::TimeTicks => SnmpValue::Timeticks(*v),
                _ => SnmpValue::Unsigned32(*v),
            },
            Encoded::Big(v) => SnmpValue::Counter64(*v),
            Encoded::Bytes(b) => SnmpValue::OctetString(b),
            Encoded::Ip(ip) => SnmpValue::IpAddress(*ip),
            Encoded::Oid(bytes) => {
                SnmpValue::ObjectIdentifier(snmp2::Oid::new(std::borrow::Cow::Borrowed(bytes)))
            }
        }
    }
}

/// Encode a write (spec §5.2): the typed `value` per the point's SNMP type, or `raw` as that
/// type's content octets, with the type's range checked either way.
///
/// The point's datatype is not consulted: what goes on the wire is the SNMP type (explicit, or
/// derived from the datatype at configure), and the value has to fit it.
pub fn encode_write(
    snmp_type: SnmpType,
    _datatype: Option<DataType>,
    request: &CommandRequest,
) -> Result<Encoded, String> {
    if let Some(raw) = &request.raw {
        return encode_raw(snmp_type, raw);
    }
    let value = request
        .value
        .as_ref()
        .ok_or_else(|| "a write needs `value` or `raw`".to_string())?;
    let number = |what: &str| -> Result<i128, String> {
        integral(value).ok_or_else(|| format!("{what} needs a whole number, not {}", shape(value)))
    };
    Ok(match snmp_type {
        SnmpType::Integer => {
            let n = number("an INTEGER")?;
            let n = i32::try_from(n).map_err(|_| format!("INTEGER {n} does not fit 32 bits"))?;
            Encoded::Int(i64::from(n))
        }
        SnmpType::Unsigned32 | SnmpType::Gauge32 | SnmpType::Counter32 | SnmpType::TimeTicks => {
            let n = number(snmp_type.name())?;
            let n = u32::try_from(n)
                .map_err(|_| format!("{} {n} is out of range", snmp_type.name()))?;
            Encoded::Unsigned(n)
        }
        SnmpType::Counter64 => {
            let n = number("a Counter64")?;
            let n = u64::try_from(n).map_err(|_| format!("Counter64 {n} is out of range"))?;
            Encoded::Big(n)
        }
        SnmpType::OctetString => {
            let text = value
                .as_str()
                .ok_or_else(|| format!("an OCTET STRING needs a string, not {}", shape(value)))?;
            if text.len() > MAX_WRITE_OCTETS {
                return Err(format!(
                    "the value is {} octets; at most {MAX_WRITE_OCTETS} fit in one datagram",
                    text.len()
                ));
            }
            Encoded::Bytes(text.as_bytes().to_vec())
        }
        SnmpType::IpAddress => {
            let text = value
                .as_str()
                .ok_or_else(|| format!("an IpAddress needs a string, not {}", shape(value)))?;
            let ip: std::net::Ipv4Addr = text
                .trim()
                .parse()
                .map_err(|_| format!("'{text}' is not an IPv4 address"))?;
            Encoded::Ip(ip.octets())
        }
        SnmpType::Oid => {
            let text = value
                .as_str()
                .ok_or_else(|| format!("an OID needs a string, not {}", shape(value)))?;
            Encoded::Oid(Oid::parse(text)?.encode())
        }
    })
}

fn encode_raw(snmp_type: SnmpType, raw: &str) -> Result<Encoded, String> {
    let content = unhex(raw)?;
    Ok(match snmp_type {
        SnmpType::Integer => {
            if content.is_empty() || content.len() > 4 {
                return Err(format!(
                    "an INTEGER is 1 to 4 content octets, not {}",
                    content.len()
                ));
            }
            let fill = if content[0] & 0x80 != 0 { 0xFF } else { 0x00 };
            let mut bytes = [fill; 8];
            bytes[8 - content.len()..].copy_from_slice(&content);
            Encoded::Int(i64::from_be_bytes(bytes))
        }
        SnmpType::Unsigned32 | SnmpType::Gauge32 | SnmpType::Counter32 | SnmpType::TimeTicks => {
            Encoded::Unsigned(
                u32::try_from(unsigned(&content, 5)?)
                    .map_err(|_| format!("{} is out of range", snmp_type.name()))?,
            )
        }
        SnmpType::Counter64 => Encoded::Big(unsigned(&content, 9)?),
        SnmpType::OctetString => {
            if content.len() > MAX_WRITE_OCTETS {
                return Err(format!(
                    "the value is {} octets; at most {MAX_WRITE_OCTETS} fit in one datagram",
                    content.len()
                ));
            }
            Encoded::Bytes(content)
        }
        SnmpType::IpAddress => Encoded::Ip(
            content
                .as_slice()
                .try_into()
                .map_err(|_| "an IpAddress is four octets".to_string())?,
        ),
        SnmpType::Oid => Encoded::Oid(Oid::from_ber(&content).map_err(|e| e.to_string())?.encode()),
    })
}

fn unsigned(content: &[u8], max_len: usize) -> Result<u64, String> {
    if content.is_empty() || content.len() > max_len {
        return Err(format!(
            "an unsigned value is 1 to {max_len} content octets, not {}",
            content.len()
        ));
    }
    let value = content.iter().fold(0u128, |acc, &b| (acc << 8) | u128::from(b));
    u64::try_from(value).map_err(|_| "the value is out of range".to_string())
}

/// Hex as the contract writes `raw`: pairs of digits, optionally grouped by spaces.
fn unhex(text: &str) -> Result<Vec<u8>, String> {
    let digits: String = text.chars().filter(|c| !c.is_whitespace()).collect();
    let digits = digits.strip_prefix("0x").unwrap_or(&digits);
    if !digits.len().is_multiple_of(2) || !digits.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("`raw` value '{text}' is not an even number of hex digits"));
    }
    Ok((0..digits.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&digits[i..i + 2], 16).expect("checked above"))
        .collect())
}

/// A JSON value as a whole number, whatever shape the command used.
fn integral(value: &serde_json::Value) -> Option<i128> {
    match value {
        serde_json::Value::Bool(b) => Some(i128::from(*b)),
        serde_json::Value::Number(n) => {
            if let Some(v) = n.as_i64() {
                Some(i128::from(v))
            } else if let Some(v) = n.as_u64() {
                Some(i128::from(v))
            } else {
                let f = n.as_f64()?;
                (f.fract() == 0.0 && f.is_finite()).then_some(f as i128)
            }
        }
        // Values beyond 2^53 are published as decimal strings (spec §6), so a write may use one.
        serde_json::Value::String(s) => s.trim().parse::<i128>().ok(),
        _ => None,
    }
}

fn shape(value: &serde_json::Value) -> &'static str {
    match value {
        serde_json::Value::Null => "null",
        serde_json::Value::Bool(_) => "a boolean",
        serde_json::Value::Number(_) => "a fractional number",
        serde_json::Value::String(_) => "a string",
        serde_json::Value::Array(_) => "a list",
        serde_json::Value::Object(_) => "an object",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(value: serde_json::Value) -> CommandRequest {
        CommandRequest { point: "p".into(), value: Some(value), value_repr: None, raw: None }
    }

    fn raw_request(raw: &str) -> CommandRequest {
        CommandRequest { point: "p".into(), value: None, value_repr: None, raw: Some(raw.into()) }
    }

    #[test]
    fn typed_writes_are_range_checked_against_the_snmp_type() {
        let write = |t, v| encode_write(t, None, &request(v));
        assert_eq!(write(SnmpType::Integer, serde_json::json!(55)).unwrap(), Encoded::Int(55));
        assert_eq!(write(SnmpType::Integer, serde_json::json!(-1)).unwrap(), Encoded::Int(-1));
        assert_eq!(write(SnmpType::Integer, serde_json::json!(true)).unwrap(), Encoded::Int(1));
        assert!(write(SnmpType::Integer, serde_json::json!(2_147_483_648u64)).is_err());
        assert_eq!(write(SnmpType::Gauge32, serde_json::json!(250)).unwrap(), Encoded::Unsigned(250));
        assert!(write(SnmpType::Gauge32, serde_json::json!(-1)).is_err(), "unsigned rejects negatives");
        assert!(write(SnmpType::Gauge32, serde_json::json!(4_294_967_296u64)).is_err());
        assert_eq!(
            write(SnmpType::Counter64, serde_json::json!("9007199254740993")).unwrap(),
            Encoded::Big(9_007_199_254_740_993)
        );
        assert_eq!(
            write(SnmpType::OctetString, serde_json::json!("hello")).unwrap(),
            Encoded::Bytes(b"hello".to_vec())
        );
        assert!(write(SnmpType::OctetString, serde_json::json!(5)).is_err());
        assert_eq!(
            write(SnmpType::IpAddress, serde_json::json!("192.168.10.2")).unwrap(),
            Encoded::Ip([192, 168, 10, 2])
        );
        assert!(write(SnmpType::IpAddress, serde_json::json!("::1")).is_err());
        assert_eq!(
            write(SnmpType::Oid, serde_json::json!("1.3.6.1")).unwrap(),
            Encoded::Oid(vec![0x2b, 6, 1])
        );
        assert!(write(SnmpType::Integer, serde_json::json!(1.5)).is_err());
        assert!(encode_write(SnmpType::Integer, None, &CommandRequest {
            point: "p".into(), value: None, value_repr: None, raw: None
        })
        .is_err());
    }

    #[test]
    fn raw_writes_are_the_types_content_octets() {
        let write = |t, raw| encode_write(t, None, &raw_request(raw));
        assert_eq!(write(SnmpType::Integer, "ff").unwrap(), Encoded::Int(-1));
        assert_eq!(write(SnmpType::Integer, "00 80").unwrap(), Encoded::Int(128));
        assert!(write(SnmpType::Integer, "0102030405").is_err(), "more than 32 bits");
        assert_eq!(write(SnmpType::Gauge32, "00ffffffff").unwrap(), Encoded::Unsigned(u32::MAX));
        assert!(write(SnmpType::Gauge32, "0100000000").is_err());
        assert_eq!(write(SnmpType::OctetString, "de ad be ef").unwrap(), Encoded::Bytes(vec![0xde, 0xad, 0xbe, 0xef]));
        assert_eq!(write(SnmpType::IpAddress, "c0a8010a").unwrap(), Encoded::Ip([192, 168, 1, 10]));
        assert!(write(SnmpType::IpAddress, "c0a801").is_err());
        assert_eq!(write(SnmpType::Oid, "2b0601").unwrap(), Encoded::Oid(vec![0x2b, 6, 1]));
        assert!(write(SnmpType::Oid, "2b86").is_err(), "a truncated OID");
        assert!(write(SnmpType::OctetString, "xyz").is_err());
    }

    #[test]
    fn a_value_carries_the_configured_snmp_type_on_the_wire() {
        let gauge = Encoded::Unsigned(250);
        assert!(matches!(gauge.value(SnmpType::Gauge32), SnmpValue::Unsigned32(250)));
        assert!(matches!(gauge.value(SnmpType::Counter32), SnmpValue::Counter32(250)));
        assert!(matches!(gauge.value(SnmpType::TimeTicks), SnmpValue::Timeticks(250)));
        assert!(matches!(Encoded::Int(-42).value(SnmpType::Integer), SnmpValue::Integer(-42)));
        assert!(matches!(Encoded::Big(1).value(SnmpType::Counter64), SnmpValue::Counter64(1)));
    }
}
