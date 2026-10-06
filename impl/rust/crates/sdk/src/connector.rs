//! The `Connector` trait every protocol module implements, plus the supporting types the SDK
//! passes in and out of it.

use crate::config::ConnectorConfig;
use crate::decode::{Endianness, WordOrder};
use crate::model::{DataType, DeviceId, InvertError, Mode, Sample, Transform, Value};
use async_trait::async_trait;
use thiserror::Error;

/// Access permitted on a point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    Read,
    Write,
    ReadWrite,
}

impl Access {
    pub fn parse(s: Option<&str>) -> Self {
        match s.map(|s| s.to_ascii_lowercase()).as_deref() {
            Some("write") => Access::Write,
            Some("read_write") | Some("readwrite") => Access::ReadWrite,
            _ => Access::Read,
        }
    }

    pub fn can_write(self) -> bool {
        matches!(self, Access::Write | Access::ReadWrite)
    }
}

/// A resolved reference to a point the runtime asks the connector to read. The protocol-specific
/// address is looked up by the connector using `device` + `id`.
#[derive(Clone, Debug)]
pub struct PointRef {
    pub id: String,
    pub mode: Mode,
    pub datatype: Option<DataType>,
    pub endianness: Endianness,
    pub word_order: WordOrder,
    pub access: Access,
    pub unit: Option<String>,
    /// Per-point linear transform applied to the decoded numeric value.
    pub transform: Transform,
    /// Effective poll interval resolved by the runtime. Subscribe-capable connectors should
    /// use it as the per-point sampling-interval hint (e.g. OPC-UA monitored item sampling).
    pub interval: Option<std::time::Duration>,
}

/// The connector's declared capabilities; drives the capability descriptor and conformance.
#[derive(Clone, Debug)]
pub struct Capabilities {
    pub protocol: &'static str,
    pub version: &'static str,
    pub modes: Vec<Mode>,
    pub datatypes: Vec<DataType>,
    pub point_kinds: Vec<String>,
    pub command_verbs: Vec<String>,
    pub features: Vec<String>,
    pub subscribe: bool,
}

impl Capabilities {
    /// Serialize to the capability descriptor JSON (contract §7).
    pub fn to_json(&self) -> serde_json::Value {
        let modes: Vec<&str> = self
            .modes
            .iter()
            .map(|m| match m {
                Mode::Raw => "raw",
                Mode::Typed => "typed",
            })
            .collect();
        let datatypes: Vec<serde_json::Value> = self
            .datatypes
            .iter()
            .map(|d| serde_json::to_value(d).unwrap())
            .collect();
        serde_json::json!({
            "protocol": self.protocol,
            "version": self.version,
            "modes": modes,
            "datatypes": datatypes,
            "point_kinds": self.point_kinds,
            "command_verbs": self.command_verbs,
            "features": self.features,
            "subscribe": self.subscribe,
        })
    }
}

/// Per-device connection status.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LinkStatus {
    Connected,
    Disconnected,
    Degraded,
}

impl LinkStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            LinkStatus::Connected => "connected",
            LinkStatus::Disconnected => "disconnected",
            LinkStatus::Degraded => "degraded",
        }
    }
}

#[derive(Clone, Debug)]
pub struct LinkReport {
    pub device: DeviceId,
    pub status: LinkStatus,
    pub reason: Option<String>,
    /// Optional device descriptor (transport/address details) published on the link status topic.
    /// Flows (e.g. ot-registration) can forward this into a digital-twin fragment. `None` omits it.
    pub info: Option<serde_json::Value>,
}

impl LinkReport {
    /// A link report without a device descriptor.
    pub fn new(device: DeviceId, status: LinkStatus, reason: Option<String>) -> Self {
        LinkReport {
            device,
            status,
            reason,
            info: None,
        }
    }
}

/// A parsed command request (contract §6).
#[derive(Clone, Debug)]
pub struct CommandRequest {
    pub point: String,
    pub value: Option<serde_json::Value>,
    pub value_repr: Option<String>,
    pub raw: Option<String>,
}

impl CommandRequest {
    /// The request a connector's `write` receives for `point` (contract §4.2, §6.2).
    ///
    /// A write request carries engineering units — the units of the sample `value` — so for a
    /// typed point a numeric `value` is mapped back through the inverse of the point's
    /// transform, and rounded to the nearest integer (ties away from zero) when the datatype
    /// is an integer, since connectors truncate or reject a fractional integer. Integer results
    /// are sent as JSON integers. Booleans, strings, `raw` (hex) requests and raw-mode points
    /// pass through unchanged. Every write path (the `write` and `write-batch` verbs and the
    /// CLI) calls this once before `execute`, so connectors never deal with the transform on
    /// write. Fails when the transform has no inverse; the write must then fail too.
    pub fn to_raw_units(&self, point: &PointRef) -> Result<CommandRequest, String> {
        let n = match &self.value {
            Some(serde_json::Value::Number(n))
                if self.raw.is_none()
                    && point.mode == Mode::Typed
                    && !point.transform.is_identity() =>
            {
                n.as_f64().unwrap_or(f64::NAN)
            }
            _ => return Ok(self.clone()),
        };
        let raw = match point.transform.invert(Value::Number(n)) {
            Ok(Value::Number(raw)) => raw,
            Ok(_) => unreachable!("invert keeps a number a number"),
            Err(InvertError::NotInvertible { multiplier_zero }) => {
                return Err(format!(
                    "point {} transform is not invertible ({})",
                    point.id,
                    if multiplier_zero {
                        "multiplier 0"
                    } else {
                        "decimal_shift out of range"
                    }
                ))
            }
            Err(InvertError::NotFinite) => {
                return Err(format!(
                    "point {}: value has no finite raw value under its transform",
                    point.id
                ))
            }
        };
        let value = if point.datatype.is_some_and(DataType::is_integer) {
            integer_json(raw.round())
        } else {
            serde_json::json!(raw)
        };
        Ok(CommandRequest {
            value: Some(value),
            ..self.clone()
        })
    }
}

/// A whole `f64` as a JSON integer (connectors read integer datatypes with `as_i64`/`as_u64`),
/// falling back to a JSON float outside the 64-bit range.
fn integer_json(n: f64) -> serde_json::Value {
    // 2^64 and -2^63 are exact in f64, so the casts below are lossless.
    if (0.0..18_446_744_073_709_551_616.0).contains(&n) {
        serde_json::Value::from(n as u64)
    } else if (-9_223_372_036_854_775_808.0..0.0).contains(&n) {
        serde_json::Value::from(n as i64)
    } else {
        serde_json::json!(n)
    }
}

/// The outcome of a successful command (the runtime wraps it in the result envelope).
#[derive(Clone, Debug, Default)]
pub struct CommandResult {
    pub point: String,
    pub value: Option<serde_json::Value>,
    pub raw: Option<String>,
}

/// Sink for event-driven (`subscribe`) connectors to push samples into.
pub type SampleSink = tokio::sync::mpsc::Sender<Sample>;

#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("invalid configuration: {0}")]
    Invalid(String),
}

#[derive(Debug, Error)]
pub enum ConnectorError {
    #[error("not connected to device {0}")]
    NotConnected(DeviceId),
    #[error("unknown point {point} on device {device}")]
    UnknownPoint { device: DeviceId, point: String },
    #[error("access denied: point {0} is not writable")]
    AccessDenied(String),
    #[error("transport error: {0}")]
    Transport(String),
    #[error("decode error: {0}")]
    Decode(String),
    #[error("unsupported operation: {0}")]
    Unsupported(String),
    #[error("{0}")]
    Other(String),
}

/// A protocol module. One instance manages the connection(s) for one configured connector.
///
/// Only `Send` is required (not `Sync`): the SDK runtime drives a connector from a single task,
/// so a connector may hold non-`Sync` transport handles (e.g. a `tokio-modbus` `Context`).
// `#[async_trait]` marks each method `#[must_use]`, and the boxed future it returns is
// `#[must_use]` already, which clippy reports as `double_must_use` on the generated code.
#[allow(clippy::double_must_use)]
#[async_trait]
pub trait Connector: Send {
    /// Validate & parse the protocol-specific parts of the configuration into a typed model.
    fn configure(&mut self, config: &ConnectorConfig) -> Result<(), ConfigError>;

    /// Declare what this connector supports. Must be cheap and pure.
    fn capabilities(&self) -> Capabilities;

    /// Keys of `[connection]` and `device.protocol_address` (matched at any depth) that a
    /// management command may not add or change, because they name local files or relax
    /// security. Such a command is refused; see [`crate::library::reject_local_only_settings`].
    fn local_only_settings(&self) -> &'static [&'static str] {
        &[]
    }

    /// OPTIONAL: tell the module, before [`Connector::connect`], that nothing will subscribe:
    /// a one-shot `read` or `write` from the CLI. The module must then not open anything that
    /// only push delivery needs — above all a listening socket, which the `run` service on the
    /// same host already holds (SNMP's notification port). The default does nothing.
    fn disable_push(&mut self) {}

    /// Establish protocol connections to all configured devices.
    async fn connect(&mut self) -> Result<Vec<LinkReport>, ConnectorError>;

    /// Read a batch of points for one device. Returns one [`Sample`] per requested point
    /// (including `bad`/`stale` samples).
    async fn read_points(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
    ) -> Result<Vec<Sample>, ConnectorError>;

    /// OPTIONAL: whether the module delivers `point` of `device` by push, for a module that
    /// advertises `subscribe`. Asked before [`Connector::subscribe`]: only the points it answers
    /// `true` for are passed to `subscribe` and taken off the polling schedule; the others stay
    /// polled through [`Connector::read_points`]. A module whose devices mix pushed and polled
    /// points (SNMP: notification points are pushed, object points polled) overrides this.
    ///
    /// Called after [`Connector::configure`]; must be cheap and pure. The default pushes every
    /// point, which is what every subscribe-capable module did before this hook existed.
    fn pushes_point(&self, device: &DeviceId, point: &PointRef) -> bool {
        let _ = (device, point);
        true
    }

    /// OPTIONAL: for event-driven protocols. Default = polling only.
    async fn subscribe(
        &mut self,
        device: &DeviceId,
        points: &[PointRef],
        sink: SampleSink,
    ) -> Result<(), ConnectorError> {
        let _ = (device, points, sink);
        Err(ConnectorError::Unsupported("subscribe".into()))
    }

    /// OPTIONAL: report whether push delivery armed by [`Connector::subscribe`] for `device` is
    /// still live.
    ///
    /// A subscribed point is off the polling schedule, so when the session behind a
    /// subscription dies (the server went away for longer than the protocol stack retries, it
    /// stopped answering, or it dropped the subscription) no failed read ever reveals it: the
    /// device just goes quiet behind a `connected` link. The runtime therefore calls this on
    /// every tick for each device with pushed points. `Ok` means push delivery is live; any
    /// error is handled like a failed poll batch — the link degrades, the device is reconnected
    /// with backoff, and its subscription re-armed. Must be cheap: report state the module
    /// already tracks rather than making a protocol round trip.
    ///
    /// The default signals `Unsupported`, which leaves liveness to polled reads alone.
    async fn check_subscription(&mut self, device: &DeviceId) -> Result<(), ConnectorError> {
        let _ = device;
        Err(ConnectorError::Unsupported("check_subscription".into()))
    }

    /// OPTIONAL: re-establish the connection to a single device whose transport dropped.
    ///
    /// The runtime calls this with exponential backoff while a device's reads keep failing.
    /// Return the device's new link report (mirroring [`Connector::connect`] semantics:
    /// `Ok` with a `Disconnected` report when the attempt failed cleanly). The default
    /// signals `Unsupported`, which makes the runtime fall back to a full
    /// [`Connector::connect`] — correct, but it re-establishes every device at once.
    async fn reconnect(&mut self, device: &DeviceId) -> Result<LinkReport, ConnectorError> {
        let _ = device;
        Err(ConnectorError::Unsupported("reconnect".into()))
    }

    /// OPTIONAL: execute a command verb (default supports nothing).
    async fn execute(
        &mut self,
        device: &DeviceId,
        verb: &str,
        request: &CommandRequest,
    ) -> Result<CommandResult, ConnectorError> {
        let _ = (device, request);
        Err(ConnectorError::Unsupported(verb.to_string()))
    }

    /// Close connections cleanly. Called on shutdown and before reload.
    async fn disconnect(&mut self) -> Result<(), ConnectorError>;
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn point(datatype: DataType, mode: Mode, transform: Transform) -> PointRef {
        PointRef {
            id: "setpoint".into(),
            mode,
            datatype: Some(datatype),
            endianness: Endianness::Big,
            word_order: WordOrder::Big,
            access: Access::ReadWrite,
            unit: None,
            transform,
            interval: None,
        }
    }

    fn write(value: serde_json::Value) -> CommandRequest {
        CommandRequest {
            point: "setpoint".into(),
            value: Some(value),
            value_repr: None,
            raw: None,
        }
    }

    const TENTHS: Transform = Transform {
        multiplier: 0.1,
        divisor: 1.0,
        decimal_shift: 0,
        offset: 0.0,
    };

    #[test]
    fn integer_point_gets_the_rounded_raw_value() {
        // 21.5 / 0.1 is 214.99999999999997 in f64: truncating would write 214
        let p = point(DataType::Uint16, Mode::Typed, TENTHS);
        let raw = write(json!(21.5)).to_raw_units(&p).unwrap();
        assert_eq!(raw.value, Some(json!(215)));
        assert!(
            raw.value.unwrap().is_u64(),
            "integer datatypes get a JSON integer"
        );

        let p = point(DataType::Int32, Mode::Typed, TENTHS);
        assert_eq!(
            write(json!(-21.5)).to_raw_units(&p).unwrap().value,
            Some(json!(-215))
        );
    }

    #[test]
    fn integer_rounding_ties_away_from_zero() {
        let p = point(
            DataType::Int16,
            Mode::Typed,
            Transform {
                offset: 0.5,
                ..Transform::default()
            },
        );
        assert_eq!(
            write(json!(3.0)).to_raw_units(&p).unwrap().value,
            Some(json!(3))
        );
        assert_eq!(
            write(json!(-2.0)).to_raw_units(&p).unwrap().value,
            Some(json!(-3))
        );
    }

    #[test]
    fn float_point_keeps_the_unrounded_value() {
        let p = point(
            DataType::Float32,
            Mode::Typed,
            Transform {
                multiplier: 2.0,
                offset: 1.0,
                ..Transform::default()
            },
        );
        assert_eq!(
            write(json!(4.0)).to_raw_units(&p).unwrap().value,
            Some(json!(1.5))
        );
    }

    #[test]
    fn pass_through_cases() {
        let typed = point(DataType::Uint16, Mode::Typed, TENTHS);
        for v in [json!(true), json!("21.5")] {
            assert_eq!(
                write(v.clone()).to_raw_units(&typed).unwrap().value,
                Some(v)
            );
        }
        // raw-mode point
        let raw_mode = point(DataType::Uint16, Mode::Raw, TENTHS);
        assert_eq!(
            write(json!(21.5)).to_raw_units(&raw_mode).unwrap().value,
            Some(json!(21.5))
        );
        // raw (hex) request
        let hex = CommandRequest {
            raw: Some("00d7".into()),
            ..write(json!(21.5))
        };
        let out = hex.to_raw_units(&typed).unwrap();
        assert_eq!(
            (out.value, out.raw),
            (Some(json!(21.5)), Some("00d7".into()))
        );
        // identity transform: untouched, even past 2^53
        let identity = point(DataType::Uint64, Mode::Typed, Transform::default());
        let big = json!(u64::MAX);
        assert_eq!(
            write(big.clone()).to_raw_units(&identity).unwrap().value,
            Some(big)
        );
    }

    #[test]
    fn non_invertible_transform_fails_the_write() {
        let p = point(
            DataType::Uint16,
            Mode::Typed,
            Transform {
                multiplier: 0.0,
                ..Transform::default()
            },
        );
        assert_eq!(
            write(json!(1)).to_raw_units(&p).unwrap_err(),
            "point setpoint transform is not invertible (multiplier 0)"
        );
    }
}
