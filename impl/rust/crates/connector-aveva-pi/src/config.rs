//! The protocol-specific configuration (spec §3): `[connection]`, `device.protocol_address` and
//! `point.address`, parsed and validated into a typed model.

use serde_json::{Map, Value as Json};
use std::path::PathBuf;
use std::time::Duration;
use tedge_dot_sdk::{parse_duration, Access, DataType, Mode, PointConfig};

/// Keys a management command may not add, change or remove (spec §3.1).
pub const LOCAL_ONLY: &[&str] = &[
    "password_file",
    "token_file",
    "cert_file",
    "key_file",
    "ca_file",
    "insecure_skip_verify",
    "state_dir",
];

/// Datatypes the decoder supports (spec §4).
pub const DATATYPES: &[DataType] = &[
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
];

const CONNECTION_KEYS: &[&str] = &[
    "auth",
    "tls",
    "request_timeout",
    "connect_timeout",
    "resolve_retry",
    "max_web_ids_per_request",
    "max_url_bytes",
    "max_concurrent_requests",
    "update_option",
    "buffer_option",
    "state_dir",
    "backfill",
    "channel",
];

/// `[connection]` keys a device may override in `protocol_address`.
const DEVICE_OVERRIDES: &[&str] = &[
    "auth",
    "tls",
    "request_timeout",
    "connect_timeout",
    "resolve_retry",
    "max_web_ids_per_request",
    "max_url_bytes",
    "max_concurrent_requests",
    "state_dir",
    "backfill",
    "channel",
];

const UPDATE_OPTIONS: &[&str] = &["Replace", "Insert", "NoReplace", "ReplaceOnly", "InsertNoCompression"];
const BUFFER_OPTIONS: &[&str] = &["DoNotBuffer", "BufferIfPossible", "Buffer"];

#[derive(Clone, Debug, PartialEq)]
pub enum Auth {
    None,
    Basic { username: String, password_file: PathBuf },
    Bearer { token_file: PathBuf },
    Certificate { cert_file: PathBuf, key_file: PathBuf },
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tls {
    pub ca_file: Option<PathBuf>,
    pub insecure_skip_verify: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Backfill {
    pub max_age: Duration,
    pub max_events: usize,
    pub page_size: usize,
    pub live_buffer: usize,
    pub cursor_flush: Duration,
}

impl Default for Backfill {
    fn default() -> Self {
        Backfill {
            max_age: Duration::from_secs(3600),
            max_events: 10_000,
            page_size: 1000,
            live_buffer: 10_000,
            cursor_flush: Duration::from_secs(10),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Channel {
    pub heartbeat_rate: u32,
    pub polling_interval: Duration,
    pub include_initial_values: bool,
}

impl Default for Channel {
    fn default() -> Self {
        Channel {
            heartbeat_rate: 5,
            polling_interval: Duration::from_secs(1),
            include_initial_values: true,
        }
    }
}

impl Channel {
    /// How long the server stays silent before it sends a heartbeat; `None` without heartbeats.
    pub fn heartbeat_period(&self) -> Option<Duration> {
        (self.heartbeat_rate > 0).then(|| self.polling_interval * self.heartbeat_rate)
    }
}

/// The settings a device's requests run with: `[connection]` merged with the device's overrides.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub auth: Auth,
    pub tls: Tls,
    pub request_timeout: Duration,
    pub connect_timeout: Duration,
    pub resolve_retry: Duration,
    pub max_web_ids_per_request: usize,
    pub max_url_bytes: usize,
    pub max_concurrent_requests: usize,
    pub update_option: String,
    pub buffer_option: String,
    pub state_dir: PathBuf,
    /// `None`: `backfill = false`.
    pub backfill: Option<Backfill>,
    pub channel: Channel,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            auth: Auth::None,
            tls: Tls::default(),
            request_timeout: Duration::from_secs(10),
            connect_timeout: Duration::from_secs(5),
            resolve_retry: Duration::from_secs(300),
            max_web_ids_per_request: 50,
            max_url_bytes: 8192,
            max_concurrent_requests: 4,
            update_option: "Replace".into(),
            buffer_option: "BufferIfPossible".into(),
            state_dir: PathBuf::from("/var/lib/tedge-dot/aveva-pi"),
            backfill: Some(Backfill::default()),
            channel: Channel::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Delivery {
    Channel,
    Recorded,
    Snapshot,
}

impl Delivery {
    pub fn as_str(self) -> &'static str {
        match self {
            Delivery::Channel => "channel",
            Delivery::Recorded => "recorded",
            Delivery::Snapshot => "snapshot",
        }
    }
}

/// What a point addresses (spec §3.3).
#[derive(Clone, Debug, PartialEq)]
pub enum Address {
    /// A PI point, by its full path `\\server\tag`.
    Point(String),
    /// An AF attribute, by its full path `\\af\db\element|attribute`.
    Attribute(String),
    /// Any stream, by WebId: never looked up.
    WebId(String),
}

impl Address {
    pub fn kind(&self) -> &'static str {
        match self {
            Address::Point(_) => "point",
            Address::Attribute(_) => "attribute",
            Address::WebId(_) => "stream",
        }
    }

    /// The path or WebId, as named in `not found:` errors and `addr.path`.
    pub fn text(&self) -> &str {
        match self {
            Address::Point(p) | Address::Attribute(p) | Address::WebId(p) => p,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Point {
    pub id: String,
    pub address: Address,
    pub mode: Mode,
    pub datatype: Option<DataType>,
    pub access: Access,
    pub unit: Option<String>,
}

#[derive(Clone, Debug)]
pub struct Device {
    pub name: String,
    /// The PI Web API root, without a trailing `/`.
    pub base_url: String,
    pub data_server: Option<String>,
    pub af_database: Option<String>,
    pub delivery: Delivery,
    pub settings: Settings,
    pub points: Vec<Point>,
}

fn obj<'a>(v: &'a Json, what: &str) -> Result<Option<&'a Map<String, Json>>, String> {
    match v {
        Json::Null => Ok(None),
        Json::Object(m) => Ok(Some(m)),
        _ => Err(format!("{what} must be a table")),
    }
}

fn check_keys(m: &Map<String, Json>, allowed: &[&str], what: &str) -> Result<(), String> {
    for k in m.keys() {
        if !allowed.contains(&k.as_str()) {
            return Err(format!("{what}: unknown key '{k}'"));
        }
    }
    Ok(())
}

fn string(m: &Map<String, Json>, key: &str, what: &str) -> Result<Option<String>, String> {
    match m.get(key) {
        None => Ok(None),
        Some(Json::String(s)) if !s.is_empty() => Ok(Some(s.clone())),
        Some(_) => Err(format!("{what}.{key} must be a non-empty string")),
    }
}

fn duration(m: &Map<String, Json>, key: &str, what: &str) -> Result<Option<Duration>, String> {
    match m.get(key) {
        None => Ok(None),
        Some(Json::String(s)) => parse_duration(s)
            .map(Some)
            .ok_or_else(|| format!("{what}.{key}: '{s}' is not a duration such as \"10s\"")),
        Some(_) => Err(format!("{what}.{key} must be a duration string such as \"10s\"")),
    }
}

fn positive(m: &Map<String, Json>, key: &str, what: &str) -> Result<Option<usize>, String> {
    match m.get(key) {
        None => Ok(None),
        Some(v) => match v.as_u64() {
            Some(n) if n > 0 => Ok(Some(n as usize)),
            _ => Err(format!("{what}.{key} must be a positive integer")),
        },
    }
}

fn boolean(m: &Map<String, Json>, key: &str, what: &str) -> Result<Option<bool>, String> {
    match m.get(key) {
        None => Ok(None),
        Some(Json::Bool(b)) => Ok(Some(*b)),
        Some(_) => Err(format!("{what}.{key} must be true or false")),
    }
}

fn parse_auth(v: &Json, what: &str) -> Result<Auth, String> {
    let m = obj(v, what)?.ok_or_else(|| format!("{what} must be a table"))?;
    for (inline, file) in [("password", "password_file"), ("token", "token_file")] {
        if m.contains_key(inline) {
            return Err(format!(
                "{what}.{inline} is not allowed: put the secret in a file and name it with {what}.{file}"
            ));
        }
    }
    let method = string(m, "method", what)?.unwrap_or_else(|| "none".into());
    let path = |key: &str| -> Result<PathBuf, String> {
        string(m, key, what)?
            .map(PathBuf::from)
            .ok_or_else(|| format!("{what}.{key} is required for method '{method}'"))
    };
    let auth = match method.as_str() {
        "none" => {
            check_keys(m, &["method"], what)?;
            Auth::None
        }
        "basic" => {
            check_keys(m, &["method", "username", "password_file"], what)?;
            Auth::Basic {
                username: string(m, "username", what)?
                    .ok_or_else(|| format!("{what}.username is required for method 'basic'"))?,
                password_file: path("password_file")?,
            }
        }
        "bearer" => {
            check_keys(m, &["method", "token_file"], what)?;
            Auth::Bearer { token_file: path("token_file")? }
        }
        "certificate" => {
            check_keys(m, &["method", "cert_file", "key_file"], what)?;
            Auth::Certificate { cert_file: path("cert_file")?, key_file: path("key_file")? }
        }
        other => {
            return Err(format!(
                "{what}.method '{other}' is not one of none, basic, bearer, certificate"
            ))
        }
    };
    Ok(auth)
}

fn parse_tls(v: &Json, what: &str) -> Result<Tls, String> {
    let m = obj(v, what)?.ok_or_else(|| format!("{what} must be a table"))?;
    check_keys(m, &["ca_file", "insecure_skip_verify"], what)?;
    Ok(Tls {
        ca_file: string(m, "ca_file", what)?.map(PathBuf::from),
        insecure_skip_verify: boolean(m, "insecure_skip_verify", what)?.unwrap_or(false),
    })
}

fn parse_backfill(v: &Json, what: &str) -> Result<Option<Backfill>, String> {
    if v == &Json::Bool(false) {
        return Ok(None);
    }
    if v == &Json::Bool(true) {
        return Ok(Some(Backfill::default()));
    }
    let m = obj(v, what)?.ok_or_else(|| format!("{what} must be a table or false"))?;
    check_keys(m, &["max_age", "max_events", "page_size", "live_buffer", "cursor_flush"], what)?;
    let d = Backfill::default();
    let page_size = positive(m, "page_size", what)?.unwrap_or(d.page_size);
    if page_size > 150_000 {
        return Err(format!("{what}.page_size must be at most 150000 (the server's default cap)"));
    }
    Ok(Some(Backfill {
        max_age: duration(m, "max_age", what)?.unwrap_or(d.max_age),
        max_events: positive(m, "max_events", what)?.unwrap_or(d.max_events),
        page_size,
        live_buffer: positive(m, "live_buffer", what)?.unwrap_or(d.live_buffer),
        cursor_flush: duration(m, "cursor_flush", what)?.unwrap_or(d.cursor_flush),
    }))
}

fn parse_channel(v: &Json, what: &str) -> Result<Channel, String> {
    let m = obj(v, what)?.ok_or_else(|| format!("{what} must be a table"))?;
    check_keys(m, &["heartbeat_rate", "polling_interval", "include_initial_values"], what)?;
    let d = Channel::default();
    let heartbeat_rate = match m.get("heartbeat_rate") {
        None => d.heartbeat_rate,
        Some(v) => v
            .as_u64()
            .and_then(|n| u32::try_from(n).ok())
            .ok_or_else(|| format!("{what}.heartbeat_rate must be a non-negative integer"))?,
    };
    Ok(Channel {
        heartbeat_rate,
        polling_interval: duration(m, "polling_interval", what)?.unwrap_or(d.polling_interval),
        include_initial_values: boolean(m, "include_initial_values", what)?
            .unwrap_or(d.include_initial_values),
    })
}

/// Apply the keys of `m` (a `[connection]` table or a device's overrides) to `s`.
fn apply(s: &mut Settings, m: &Map<String, Json>, what: &str) -> Result<(), String> {
    if let Some(v) = m.get("auth") {
        s.auth = parse_auth(v, &format!("{what}.auth"))?;
    }
    if let Some(v) = m.get("tls") {
        s.tls = parse_tls(v, &format!("{what}.tls"))?;
    }
    if let Some(d) = duration(m, "request_timeout", what)? {
        s.request_timeout = d;
    }
    if let Some(d) = duration(m, "connect_timeout", what)? {
        s.connect_timeout = d;
    }
    if let Some(d) = duration(m, "resolve_retry", what)? {
        s.resolve_retry = d;
    }
    if let Some(n) = positive(m, "max_web_ids_per_request", what)? {
        s.max_web_ids_per_request = n;
    }
    if let Some(n) = positive(m, "max_url_bytes", what)? {
        s.max_url_bytes = n;
    }
    if let Some(n) = positive(m, "max_concurrent_requests", what)? {
        s.max_concurrent_requests = n;
    }
    if let Some(o) = string(m, "update_option", what)? {
        if !UPDATE_OPTIONS.contains(&o.as_str()) {
            return Err(format!("{what}.update_option '{o}' is not one of {}", UPDATE_OPTIONS.join(", ")));
        }
        s.update_option = o;
    }
    if let Some(o) = string(m, "buffer_option", what)? {
        if !BUFFER_OPTIONS.contains(&o.as_str()) {
            return Err(format!("{what}.buffer_option '{o}' is not one of {}", BUFFER_OPTIONS.join(", ")));
        }
        s.buffer_option = o;
    }
    if let Some(d) = string(m, "state_dir", what)? {
        s.state_dir = PathBuf::from(d);
    }
    if let Some(v) = m.get("backfill") {
        s.backfill = parse_backfill(v, &format!("{what}.backfill"))?;
    }
    if let Some(v) = m.get("channel") {
        s.channel = parse_channel(v, &format!("{what}.channel"))?;
    }
    Ok(())
}

/// `[connection]`.
pub fn connection(v: &Json) -> Result<Settings, String> {
    let mut s = Settings::default();
    if let Some(m) = obj(v, "connection")? {
        check_keys(m, CONNECTION_KEYS, "connection")?;
        apply(&mut s, m, "connection")?;
    }
    Ok(s)
}

/// A device: its `protocol_address` over the connection settings, and its parsed points.
pub fn device(name: &str, address: &Json, conn: &Settings, points: &[PointConfig], default_mode: Option<Mode>) -> Result<Device, String> {
    let m = obj(address, "protocol_address")?
        .ok_or_else(|| "protocol_address.base_url is required".to_string())?;
    let mut allowed = vec!["base_url", "data_server", "af_database", "delivery"];
    allowed.extend_from_slice(DEVICE_OVERRIDES);
    check_keys(m, &allowed, "protocol_address")?;
    let base_url = string(m, "base_url", "protocol_address")?
        .ok_or_else(|| "protocol_address.base_url is required".to_string())?;
    if !(base_url.starts_with("https://") || base_url.starts_with("http://")) {
        return Err(format!("protocol_address.base_url '{base_url}' must start with https:// or http://"));
    }
    let delivery = match string(m, "delivery", "protocol_address")?.as_deref() {
        None | Some("channel") => Delivery::Channel,
        Some("recorded") => Delivery::Recorded,
        Some("snapshot") => Delivery::Snapshot,
        Some(other) => {
            return Err(format!(
                "protocol_address.delivery '{other}' is not one of channel, recorded, snapshot"
            ))
        }
    };
    let mut settings = conn.clone();
    apply(&mut settings, m, "protocol_address")?;
    // A password or token goes in every request's header, and a client certificate needs TLS:
    // plain HTTP is for servers without authentication only.
    let method = match settings.auth {
        Auth::None => None,
        Auth::Basic { .. } => Some("basic"),
        Auth::Bearer { .. } => Some("bearer"),
        Auth::Certificate { .. } => Some("certificate"),
    };
    if let (Some(method), true) = (method, base_url.starts_with("http://")) {
        return Err(format!(
            "protocol_address.base_url '{base_url}': auth method '{method}' needs https://, \
             credentials are never sent over plain HTTP"
        ));
    }
    let data_server = string(m, "data_server", "protocol_address")?;
    let af_database = string(m, "af_database", "protocol_address")?;
    let mut parsed = Vec::with_capacity(points.len());
    for p in points {
        parsed.push(
            point(p, default_mode, data_server.as_deref(), af_database.as_deref())
                .map_err(|e| format!("point '{}': {e}", p.id))?,
        );
    }
    Ok(Device {
        name: name.to_string(),
        base_url: base_url.trim_end_matches('/').to_string(),
        data_server,
        af_database,
        delivery,
        settings,
        points: parsed,
    })
}

/// A point's `address` (spec §3.3).
pub fn point(p: &PointConfig, default_mode: Option<Mode>, data_server: Option<&str>, af_database: Option<&str>) -> Result<Point, String> {
    let m = obj(&p.address, "address")?.ok_or_else(|| {
        "address must have exactly one of tag, path or web_id".to_string()
    })?;
    check_keys(m, &["tag", "path", "web_id"], "address")?;
    if m.len() != 1 {
        return Err("address must have exactly one of tag, path or web_id".into());
    }
    let address = if let Some(tag) = string(m, "tag", "address")? {
        let server = data_server.ok_or_else(|| {
            "address.tag needs the device's protocol_address.data_server".to_string()
        })?;
        Address::Point(format!("\\\\{}\\{}", server.trim_start_matches('\\'), tag))
    } else if let Some(path) = string(m, "path", "address")? {
        if path.contains('|') {
            if path.starts_with("\\\\") {
                Address::Attribute(path)
            } else {
                let db = af_database.ok_or_else(|| {
                    "a relative attribute path needs the device's protocol_address.af_database".to_string()
                })?;
                Address::Attribute(format!("{}\\{}", db.trim_end_matches('\\'), path))
            }
        } else if path.starts_with("\\\\") {
            Address::Point(path)
        } else {
            return Err(format!("address.path '{path}' must start with \\\\<server>"));
        }
    } else if let Some(w) = string(m, "web_id", "address")? {
        Address::WebId(w)
    } else {
        return Err("address must have exactly one of tag, path or web_id".into());
    };
    let mode = p.mode.or(default_mode).unwrap_or(Mode::Typed);
    if mode == Mode::Typed {
        match p.datatype {
            None => return Err("datatype is required in typed mode".into()),
            Some(dt) if !DATATYPES.contains(&dt) => {
                return Err(format!("datatype '{}' is not supported (use raw mode)", crate::value::datatype_name(dt)))
            }
            _ => {}
        }
    }
    Ok(Point {
        id: p.id.clone(),
        address,
        mode,
        datatype: p.datatype,
        access: Access::parse(p.access.as_deref()),
        unit: p.unit.clone(),
    })
}
