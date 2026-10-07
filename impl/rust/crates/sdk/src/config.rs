//! Contract-level configuration model (protocol-neutral). The protocol-specific objects
//! (`connection`, `device.protocol_address`, `point.address`) are kept as raw JSON values and
//! parsed by the connector module in `configure`.

use crate::model::{DataType, Mode, Transform};
use serde::Deserialize;
use std::fmt;
use std::time::Duration;

#[derive(Clone, PartialEq, Deserialize)]
pub struct ConnectorConfig {
    pub connector: ConnectorSection,
    #[serde(default)]
    pub mqtt: MqttSection,
    /// Protocol-specific shared connection defaults (opaque to the contract).
    #[serde(default)]
    pub connection: serde_json::Value,
    #[serde(rename = "device", default)]
    pub devices: Vec<DeviceConfig>,
    /// The absolute directory of the configuration file, set by [`crate::library::resolve`].
    /// Connector modules resolve relative paths in their protocol-specific settings (key
    /// files, PKI directories) against it, never against the process working directory.
    /// `None` for a configuration parsed without a file.
    #[serde(skip)]
    pub base_dir: Option<std::path::PathBuf>,
}

impl ConnectorConfig {
    /// `path` as written in the configuration: an absolute path unchanged, a relative one
    /// joined to [`ConnectorConfig::base_dir`] (or left relative when there is none).
    pub fn resolve_path(&self, path: &str) -> std::path::PathBuf {
        let p = std::path::Path::new(path);
        match &self.base_dir {
            Some(base) if p.is_relative() => base.join(p),
            _ => p.to_path_buf(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct ConnectorSection {
    pub protocol: String,
    /// The configured service name; read it through [`ConnectorSection::service_name`], which
    /// applies the default.
    #[serde(rename = "service_name", default)]
    pub(crate) configured_service_name: Option<String>,
    #[serde(default = "default_poll_interval")]
    pub poll_interval: String,
    /// Default sampling interval of pushed points (contract §3.1); see
    /// [`ConnectorConfig::sampling_interval`].
    #[serde(default)]
    pub sampling_interval: Option<String>,
    #[serde(default = "default_log_level")]
    pub log_level: String,
    /// Upper bound on a single protocol-module call (read batch, write, connect, subscribe).
    /// A module that never returns — what a half-open TCP socket produces: no answer, no
    /// error, no RST — would otherwise block the connector's loop forever, stopping samples,
    /// health and link status with nothing logged. The runtime turns the bound into an
    /// ordinary transport error, so the usual degraded-link and reconnect handling applies.
    #[serde(default = "default_operation_timeout")]
    pub operation_timeout: String,
    /// How long the connector's loop may make no progress before it is considered wedged and
    /// restarted (the supervisor cancels and re-runs it; the MQTT last will marks the service
    /// down so the cloud sees the outage). `"0"` disables the watchdog.
    #[serde(default = "default_stall_timeout")]
    pub stall_timeout: String,
    /// Directories searched for the point libraries devices name in `points_from`
    /// ([`crate::library`]). Unset means the built-in path: the site directory
    /// `/etc/tedge/plugins/ot/points.d` first, then the packaged
    /// `/usr/share/tedge-dot/points.d`. Relative entries resolve against the configuration
    /// file's own directory.
    #[serde(default)]
    pub point_library_path: Option<Vec<String>>,
    /// Default reporting policy for every point (§5.3), as written.
    #[serde(default)]
    pub report: Option<serde_json::Value>,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct MqttSection {
    #[serde(default = "default_mqtt_host")]
    pub host: String,
    #[serde(default = "default_mqtt_port")]
    pub port: u16,
}

impl Default for MqttSection {
    fn default() -> Self {
        MqttSection {
            host: default_mqtt_host(),
            port: default_mqtt_port(),
        }
    }
}

#[derive(Clone, PartialEq, Deserialize)]
pub struct DeviceConfig {
    pub name: String,
    /// Protocol-specific device address (opaque to the contract).
    pub protocol_address: serde_json::Value,
    /// The *device type* this instance is one of (§3.1): what its point list describes, not
    /// where it is. Declared here, or inherited from the first point library the device
    /// references (§3.4) — a library is the point list of one device type, so it is the
    /// natural place to name it.
    ///
    /// It qualifies the names of the device's parameter sets (§5.2), which are tenant-wide
    /// identifiers in the cloud: two device types on the same protocol have different points
    /// and so must not share a set name. It is also echoed in samples and link status, so the
    /// registration flow can use it as the thin-edge entity type.
    #[serde(rename = "type", default)]
    pub device_type: Option<String>,
    #[serde(default)]
    pub poll_interval: Option<String>,
    #[serde(default)]
    pub sampling_interval: Option<String>,
    #[serde(default)]
    pub default_mode: Option<Mode>,
    /// Point libraries this device inherits its points from, in order (§3.4). Names are
    /// resolved against the library search path; entries containing `/` or ending in `.toml`
    /// are paths, relative ones against the configuration file's directory. Resolution
    /// happens in [`crate::library`] when the configuration is loaded, so by the time a
    /// connector sees this config `points` already holds the fully-resolved list and this
    /// field is only a record of where it came from.
    #[serde(default)]
    pub points_from: Vec<String>,
    #[serde(rename = "point", default)]
    pub points: Vec<PointConfig>,
    /// Default reporting policy for this device's points (§5.3), as written.
    #[serde(default)]
    pub report: Option<serde_json::Value>,
}

// The opaque protocol tables may hold credentials (an SNMP USM password, an OPC UA user
// password), and a derived `Debug` is one `{:?}` or failed `assert_eq!` away from a log. These
// impls print them with every credential-like value masked; see [`redacted`].
impl fmt::Debug for ConnectorConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ConnectorConfig")
            .field("connector", &self.connector)
            .field("mqtt", &self.mqtt)
            .field("connection", &redacted(&self.connection))
            .field("devices", &self.devices)
            .field("base_dir", &self.base_dir)
            .finish()
    }
}

impl fmt::Debug for DeviceConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DeviceConfig")
            .field("name", &self.name)
            .field("protocol_address", &redacted(&self.protocol_address))
            .field("device_type", &self.device_type)
            .field("poll_interval", &self.poll_interval)
            .field("sampling_interval", &self.sampling_interval)
            .field("default_mode", &self.default_mode)
            .field("points_from", &self.points_from)
            .field("points", &self.points)
            .field("report", &self.report)
            .finish()
    }
}

/// A copy of an opaque protocol table with the value of every key that names a credential
/// (containing `password`, `secret`, `passphrase` or `token`, in any case) replaced by `"***"`,
/// at any depth.
pub fn redacted(value: &serde_json::Value) -> serde_json::Value {
    use serde_json::Value;
    match value {
        Value::Object(map) => Value::Object(
            map.iter()
                .map(|(k, v)| {
                    let key = k.to_ascii_lowercase();
                    let secret = ["password", "secret", "passphrase", "token"]
                        .iter()
                        .any(|word| key.contains(word));
                    let v = if secret { Value::String("***".into()) } else { redacted(v) };
                    (k.clone(), v)
                })
                .collect(),
        ),
        Value::Array(items) => Value::Array(items.iter().map(redacted).collect()),
        other => other.clone(),
    }
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct PointConfig {
    pub id: String,
    #[serde(default)]
    pub mode: Option<Mode>,
    #[serde(default)]
    pub datatype: Option<DataType>,
    #[serde(default)]
    pub endianness: Option<String>,
    #[serde(default)]
    pub word_order: Option<String>,
    #[serde(default)]
    pub poll_interval: Option<String>,
    /// How often the source samples this point when it is delivered by push (§3.2); a polled
    /// point ignores it. See [`ConnectorConfig::sampling_interval`].
    #[serde(default)]
    pub sampling_interval: Option<String>,
    /// Protocol-specific point address (opaque to the contract).
    pub address: serde_json::Value,
    #[serde(default)]
    pub access: Option<String>,
    #[serde(default)]
    pub unit: Option<String>,
    /// Short human-readable label for this signal, for where a name is displayed instead of the
    /// `id` (which is a topic segment and a fragment key, so it stays a plain identifier).
    /// Feeds a parameter's DTM title and the `point_labels` of the capability descriptor (§7).
    #[serde(default)]
    pub name: Option<String>,
    /// Longer human-readable explanation of what this signal is. Feeds a parameter's DTM
    /// description and the `point_labels` of the capability descriptor (§7). Neither this nor
    /// `name` is echoed per sample: they are static, so they are published once, retained.
    #[serde(default)]
    pub description: Option<String>,
    /// Optional per-point linear transform applied by the connector after decode.
    #[serde(default)]
    pub transform: Option<Transform>,
    /// Free-form signal metadata, echoed verbatim as `meta` in every sample envelope for this
    /// point. Flows read it for per-signal behaviour (e.g. `on_change`, `min_interval`,
    /// `deadband`); the connector and runtime never interpret it.
    #[serde(default)]
    pub meta: Option<serde_json::Value>,
    /// Set to `false` to keep this point on the polling schedule even when the connector
    /// supports push delivery (`subscribe`). Defaults to push when available.
    #[serde(default)]
    pub subscribe: Option<bool>,
    /// This point's reporting policy (§5.3), as written (merged with a library's).
    #[serde(default)]
    pub report: Option<serde_json::Value>,
    /// This point's value map (§4.3), as written. A library's is replaced, not merged; an empty
    /// table means no map. Read it through [`PointConfig::value_map`].
    #[serde(default)]
    pub map: Option<serde_json::Value>,
}

impl PointConfig {
    /// The point's parsed value map, if it has one. The loader validated it, so a map that does
    /// not parse (only possible for a config built without the loader) counts as none.
    pub fn value_map(&self) -> Option<crate::map::ValueMap> {
        self.map
            .as_ref()
            .and_then(|m| crate::map::ValueMap::parse(m).ok().flatten())
    }

    /// Resolve the effective output mode, given the device default.
    pub fn resolved_mode(&self, device_default: Option<Mode>) -> Mode {
        self.mode.or(device_default).unwrap_or(Mode::Typed)
    }
}

impl ConnectorConfig {
    /// The merged `report` table of one point: `[connector]`, then its device, then the point
    /// (already merged with any point library by the loader).
    pub fn report_table(&self, device: &DeviceConfig, point: &PointConfig) -> serde_json::Map<String, serde_json::Value> {
        let mut table = serde_json::Map::new();
        crate::report::merge_into(&mut table, self.connector.report.as_ref());
        crate::report::merge_into(&mut table, device.report.as_ref());
        crate::report::merge_into(&mut table, point.report.as_ref());
        table
    }

    /// A point's effective poll interval (§3.1): the point's, else its device's, else
    /// `[connector]`'s, else 2 s.
    pub fn poll_interval(&self, device: &DeviceConfig, point: &PointConfig) -> Duration {
        point
            .poll_interval
            .as_deref()
            .and_then(parse_duration)
            .or_else(|| device.poll_interval.as_deref().and_then(parse_duration))
            .or_else(|| parse_duration(&self.connector.poll_interval))
            .unwrap_or(Duration::from_secs(2))
    }

    /// A pushed point's effective sampling interval (§3.1): the first `sampling_interval` of the
    /// point, its device and `[connector]`, else its effective poll interval. An explicit
    /// `sampling_interval` at any level wins over every `poll_interval`, so a connector-wide
    /// setting is not silently cancelled by an older point-level `poll_interval`. Zero is kept:
    /// it asks the source for its fastest rate. The C loader (impl/c/sdk/src/config.c
    /// `resolve_sampling_intervals`) resolves the same value.
    pub fn sampling_interval(&self, device: &DeviceConfig, point: &PointConfig) -> Duration {
        [
            point.sampling_interval.as_deref(),
            device.sampling_interval.as_deref(),
            self.connector.sampling_interval.as_deref(),
        ]
        .into_iter()
        .flatten()
        .find_map(parse_duration)
        .unwrap_or_else(|| self.poll_interval(device, point))
    }
}

impl ConnectorSection {
    /// The connector's service name: `service_name` as configured, else `tedge-dot-<protocol>`.
    ///
    /// The default carries the protocol because connectors of different protocols run from one
    /// configuration directory and must not share a service (health, capability descriptor and
    /// the management command topic, contract §6.3, all hang off it).
    pub fn service_name(&self) -> String {
        self.configured_service_name
            .clone()
            .unwrap_or_else(|| format!("tedge-dot-{}", self.protocol))
    }
}
fn default_poll_interval() -> String {
    "2s".to_string()
}
fn default_log_level() -> String {
    "info".to_string()
}
fn default_operation_timeout() -> String {
    "30s".to_string()
}
fn default_stall_timeout() -> String {
    "120s".to_string()
}
fn default_mqtt_host() -> String {
    "127.0.0.1".to_string()
}
fn default_mqtt_port() -> u16 {
    1883
}

/// Parse a thin-edge duration string (`"500ms"`, `"2s"`, `"1.5m"`, `"2h"`): a decimal number
/// (digits, optionally `.` and more digits) followed by an optional unit, where no unit means
/// seconds and `ms` takes whole milliseconds only. Whitespace — as C's `isspace()` defines it —
/// may surround the number and the unit. Anything else yields `None`: signs, exponents,
/// `inf`/`NaN`, and values too large for a [`Duration`].
///
/// The C SDK's `tdot_duration_parse` accepts exactly the same strings, because both loaders
/// refuse a `poll_interval` this rejects and must refuse the same files. Config values arrive
/// from hand-edited files and remote `set-config` commands, so this must never panic.
pub fn parse_duration(s: &str) -> Option<Duration> {
    let s = crate::library::trim_c(s);
    let (number, unit) = match s.find(|c: char| !(c.is_ascii_digit() || c == '.')) {
        Some(at) => (&s[..at], crate::library::trim_c(&s[at..])),
        None => (s, ""),
    };
    let digits = |part: &str| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit());
    let well_formed = match number.split_once('.') {
        Some((whole, fraction)) => digits(whole) && digits(fraction),
        None => digits(number),
    };
    if !well_formed {
        return None;
    }
    let scale = match unit {
        "ms" => return number.parse::<u64>().ok().map(Duration::from_millis),
        "" | "s" => 1.0,
        "m" => 60.0,
        "h" => 3600.0,
        _ => return None,
    };
    Duration::try_from_secs_f64(number.parse::<f64>().ok()? * scale).ok()
}

impl ConnectorConfig {
    /// The stall watchdog's limit for this connector: `[connector] stall_timeout` (120s when it
    /// does not parse), zero when disabled with `"0"`, and never less than twice
    /// `operation_timeout` (30s when it does not parse), so one slow but legitimate call is not
    /// read as a hang. The C loader derives `stall_timeout_s` the same way.
    pub fn stall_limit(&self) -> Duration {
        let configured =
            parse_duration(&self.connector.stall_timeout).unwrap_or(Duration::from_secs(120));
        if configured.is_zero() {
            return Duration::ZERO;
        }
        let operation =
            parse_duration(&self.connector.operation_timeout).unwrap_or(Duration::from_secs(30));
        configured.max(operation.saturating_mul(2))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn debug_redacts_credentials_in_opaque_tables() {
        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "x"
[connection]
Auth_Password = "conn-secret-1"
nested = { list = [{ priv_passphrase = "conn-secret-2" }], host = "keep-me" }
[[device]]
name = "d"
protocol_address = { user = "u", password = "dev-secret-3", client_secret = 42 }
"#,
        )
        .unwrap();
        let text = format!("{cfg:?}");
        for leaked in ["conn-secret-1", "conn-secret-2", "dev-secret-3", "42"] {
            assert!(!text.contains(leaked), "{leaked} in {text}");
        }
        assert!(text.contains("keep-me") && text.contains("\"u\""), "{text}");
        // Only the Debug view is masked.
        assert_eq!(cfg.devices[0].protocol_address["password"], "dev-secret-3");
    }

    #[test]
    fn timeout_defaults_are_sane_and_overridable() {
        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "modbus"
"#,
        )
        .unwrap();
        assert_eq!(parse_duration(&cfg.connector.operation_timeout), Some(Duration::from_secs(30)));
        assert_eq!(parse_duration(&cfg.connector.stall_timeout), Some(Duration::from_secs(120)));

        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "modbus"
operation_timeout = "3s"
stall_timeout = "0"
"#,
        )
        .unwrap();
        assert_eq!(parse_duration(&cfg.connector.operation_timeout), Some(Duration::from_secs(3)));
        // "0" disables the watchdog
        assert_eq!(parse_duration(&cfg.connector.stall_timeout), Some(Duration::ZERO));
    }

    /// The service name addresses the connector's management commands (contract §6.3), and the
    /// flows default to `tedge-dot-<protocol>` when a command names none: the connector default
    /// must be that same name, whatever the protocol.
    #[test]
    fn service_name_defaults_to_the_protocol_service() {
        let cfg: ConnectorConfig = toml::from_str("[connector]\nprotocol = \"opcua\"\n").unwrap();
        assert_eq!(cfg.connector.service_name(), "tedge-dot-opcua");

        let cfg: ConnectorConfig =
            toml::from_str("[connector]\nprotocol = \"opcua\"\nservice_name = \"plant-a\"\n")
                .unwrap();
        assert_eq!(cfg.connector.service_name(), "plant-a");
    }

    #[test]
    fn durations() {
        assert_eq!(parse_duration("500ms"), Some(Duration::from_millis(500)));
        assert_eq!(parse_duration("2s"), Some(Duration::from_secs(2)));
        assert_eq!(parse_duration("5m"), Some(Duration::from_secs(300)));
        assert_eq!(parse_duration("2h"), Some(Duration::from_secs(7200)));
        assert_eq!(parse_duration("3"), Some(Duration::from_secs(3)));
        assert_eq!(parse_duration("1.5m"), Some(Duration::from_secs(90)));
        assert_eq!(parse_duration(" 2 s\t"), Some(Duration::from_secs(2)));
        assert_eq!(parse_duration("0"), Some(Duration::ZERO));
    }

    /// The grammar both SDKs share. Mirrors `check_duration_grammar` in impl/c/tests/config.c:
    /// every string here must get the same verdict from `tdot_duration_parse`.
    #[test]
    fn duration_grammar_matches_the_c_sdk() {
        for bad in [
            "1.5ms", "1e3s", "+2s", ".5s", "5.s", "0x10", "2 fortnights", "2s x", "s", "ms",
            "2 3s", "\u{a0}2s", "100000000000000000000s", "18446744073709551616ms",
        ] {
            assert_eq!(parse_duration(bad), None, "{bad:?}");
        }
        assert_eq!(parse_duration("18446744073709551615ms"), Some(Duration::from_millis(u64::MAX)));
    }

    /// Found by the `config_toml` fuzz target: negative/NaN/overflowing durations used to
    /// panic in `Duration::from_secs_f64`.
    #[test]
    fn invalid_durations_are_none_not_panics() {
        assert_eq!(parse_duration("-66"), None);
        assert_eq!(parse_duration("-5s"), None);
        assert_eq!(parse_duration("NaN"), None);
        assert_eq!(parse_duration("inf"), None);
        assert_eq!(parse_duration("1e300h"), None);
        assert_eq!(parse_duration(""), None);
        assert_eq!(parse_duration("abc"), None);
    }
}
