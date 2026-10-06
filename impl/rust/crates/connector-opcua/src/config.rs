//! Protocol-specific configuration for the OPC-UA connector. These structs fill the contract's
//! opaque slots: `connection`, `device.protocol_address`, and `point.address`.
//!
//! The serde types are the file's shape; [`device_security`] turns them into the validated
//! [`DeviceSecurity`] a connection is made with (doc/connectors/opcua-connector-spec.md §3).
//! Conversion to the `async-opcua` runtime types (e.g. `NodeId`) is done in [`crate`].
//!
//! Secrets: a password is a [`Secret`], whose `Debug` is redacted and which has no `Display`,
//! and no error built here quotes a value — only field names and file paths.

use serde::de::{self, Deserializer, Visitor};
use serde::Deserialize;
use std::fmt;
use std::path::{Path, PathBuf};

/// Settings a management command may not add or change (spec §3): they name files on the
/// gateway (whose contents would be read and sent to a server) or relax server authentication.
pub const LOCAL_ONLY_SETTINGS: &[&str] = &[
    "pki_dir",
    "certificate",
    "private_key",
    "create_certificate",
    "password_file",
    "user_certificate",
    "user_private_key",
    "trust_any_server_certificate",
    "allow_plaintext_password",
    "allow_deprecated_security",
];

/// Where the PKI directory lives when `[connection] pki_dir` is not set.
pub const DEFAULT_PKI_DIR: &str = "/var/lib/tedge-dot/opcua/pki";

/// Shared `[connection]` defaults for all OPC-UA devices.
#[derive(Debug, Clone, Deserialize)]
pub struct OpcuaConnection {
    #[serde(default = "default_app_name")]
    pub application_name: String,
    #[serde(default = "default_app_uri")]
    pub application_uri: String,
    /// Default security policy (`None`, `Basic256Sha256`, ...). Per-device value wins.
    #[serde(default)]
    pub security_policy: Option<String>,
    /// Default message security mode (`none`, `sign`, `sign_and_encrypt`). Per-device value wins.
    #[serde(default)]
    pub security_mode: Option<String>,
    /// Seconds to wait for a session to activate before declaring the link down.
    #[serde(default = "default_connect_timeout")]
    pub connect_timeout_s: u64,
    /// Seconds to wait for a read/write service call. Bounds a request stuck behind a dead
    /// transport (the client queues requests while it tries to resurrect the session), so a
    /// dropped connection surfaces as bad samples instead of stalling the poll loop.
    #[serde(default = "default_request_timeout")]
    pub request_timeout_s: u64,
    /// The PKI directory (application certificate, trust lists). Relative to the
    /// configuration file; [`DEFAULT_PKI_DIR`] when unset.
    #[serde(default)]
    pub pki_dir: Option<String>,
    /// The application instance certificate (DER or PEM). With `private_key`, overrides the
    /// PKI directory's `own/` entries.
    #[serde(default)]
    pub certificate: Option<String>,
    #[serde(default)]
    pub private_key: Option<String>,
    /// Generate a self-signed application certificate when none exists.
    #[serde(default = "default_true")]
    pub create_certificate: bool,
    /// Accept any server certificate (messages are still signed/encrypted). Insecure.
    #[serde(default)]
    pub trust_any_server_certificate: bool,
    /// Allow the deprecated `Basic128Rsa15` and `Basic256` policies.
    #[serde(default)]
    pub allow_deprecated_security: bool,
    /// Allow a password to be sent unencrypted.
    #[serde(default)]
    pub allow_plaintext_password: bool,
}

impl Default for OpcuaConnection {
    fn default() -> Self {
        OpcuaConnection {
            application_name: default_app_name(),
            application_uri: default_app_uri(),
            security_policy: None,
            security_mode: None,
            connect_timeout_s: default_connect_timeout(),
            request_timeout_s: default_request_timeout(),
            pki_dir: None,
            certificate: None,
            private_key: None,
            create_certificate: true,
            trust_any_server_certificate: false,
            allow_deprecated_security: false,
            allow_plaintext_password: false,
        }
    }
}

impl OpcuaConnection {
    /// Parse `[connection]`; an absent table is all defaults.
    pub fn from_value(value: &serde_json::Value) -> Result<OpcuaConnection, String> {
        if value.is_null() {
            return Ok(OpcuaConnection::default());
        }
        serde_json::from_value(value.clone()).map_err(|e| format!("[connection]: {e}"))
    }

    /// The PKI directory, with a relative path joined to `base_dir`.
    pub fn pki_dir(&self, base_dir: Option<&Path>) -> PathBuf {
        resolve_path(base_dir, self.pki_dir.as_deref().unwrap_or(DEFAULT_PKI_DIR))
    }
}

/// `device.protocol_address` — how to reach one OPC-UA server endpoint.
#[derive(Debug, Clone, Deserialize)]
pub struct OpcuaEndpoint {
    /// e.g. `opc.tcp://plc.example.com:4840/`.
    pub endpoint: String,
    #[serde(default)]
    pub security_policy: Option<String>,
    #[serde(default)]
    pub security_mode: Option<String>,
    /// Username identity. Anonymous when no identity field is set.
    #[serde(default)]
    pub user: Option<String>,
    #[serde(default)]
    pub password: Option<Secret>,
    /// A file whose first line is the password, read at configure.
    #[serde(default)]
    pub password_file: Option<String>,
    /// X.509 user identity: certificate (DER or PEM) and private key (PEM).
    #[serde(default)]
    pub user_certificate: Option<String>,
    #[serde(default)]
    pub user_private_key: Option<String>,
    /// Per-device overrides of the `[connection]` switches of the same name.
    #[serde(default)]
    pub trust_any_server_certificate: Option<bool>,
    #[serde(default)]
    pub allow_deprecated_security: Option<bool>,
    #[serde(default)]
    pub allow_plaintext_password: Option<bool>,
}

/// `point.address` — how to address one OPC-UA node. Either give the standard textual
/// `node_id` (`ns=2;s=Temperature`, `ns=3;i=1001`) or the structured `namespace` + `identifier`.
#[derive(Debug, Clone, Deserialize)]
pub struct NodeAddress {
    #[serde(default)]
    pub node_id: Option<String>,
    #[serde(default)]
    pub namespace: Option<u16>,
    /// String identifier (`s=`) or numeric identifier (`i=`); a JSON string or number.
    #[serde(default)]
    pub identifier: Option<serde_json::Value>,
    /// One field of a structured value, as a dotted path (`Motor.Current`, `Items[1].Value`).
    #[serde(default)]
    pub field: Option<String>,
    /// One element of an array value, selected on the server through `IndexRange`.
    #[serde(default)]
    pub index: Option<u32>,
}

/// A password. Its `Debug` is redacted, it has no `Display`, a wrong type is reported without
/// the value, and the bytes are overwritten when it is dropped.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct Secret(String);

impl Secret {
    pub fn new(value: String) -> Secret {
        Secret(value)
    }

    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl fmt::Debug for Secret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("***")
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        // SAFETY: zero bytes are valid UTF-8, so the string stays well-formed.
        unsafe { self.0.as_bytes_mut() }.fill(0);
    }
}

impl<'de> Deserialize<'de> for Secret {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Secret, D::Error> {
        struct SecretVisitor;
        const WRONG_TYPE: &str = "a password must be a string (or use password_file)";
        // serde's default "invalid type" message quotes the value: every non-string visit is
        // answered with a message that does not.
        macro_rules! reject {
            ($($name:ident: $ty:ty),*) => {
                $(fn $name<E: de::Error>(self, _: $ty) -> Result<Secret, E> {
                    Err(E::custom(WRONG_TYPE))
                })*
            };
        }
        impl<'de> Visitor<'de> for SecretVisitor {
            type Value = Secret;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("a password string")
            }
            fn visit_str<E: de::Error>(self, value: &str) -> Result<Secret, E> {
                Ok(Secret(value.to_string()))
            }
            reject!(visit_bool: bool, visit_i64: i64, visit_u64: u64, visit_f64: f64,
                    visit_bytes: &[u8]);
            fn visit_unit<E: de::Error>(self) -> Result<Secret, E> {
                Err(E::custom(WRONG_TYPE))
            }
            fn visit_seq<A: de::SeqAccess<'de>>(self, _: A) -> Result<Secret, A::Error> {
                Err(de::Error::custom(WRONG_TYPE))
            }
            fn visit_map<A: de::MapAccess<'de>>(self, _: A) -> Result<Secret, A::Error> {
                Err(de::Error::custom(WRONG_TYPE))
            }
        }
        deserializer.deserialize_any(SecretVisitor)
    }
}

/// A security policy the connector knows (spec §3.1). ECC policies are not supported.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Policy {
    None,
    Basic128Rsa15,
    Basic256,
    Basic256Sha256,
    Aes128Sha256RsaOaep,
    Aes256Sha256RsaPss,
}

const POLICY_URI_PREFIX: &str = "http://opcfoundation.org/UA/SecurityPolicy#";

impl Policy {
    pub const ALL: [Policy; 6] = [
        Policy::None,
        Policy::Basic128Rsa15,
        Policy::Basic256,
        Policy::Basic256Sha256,
        Policy::Aes128Sha256RsaOaep,
        Policy::Aes256Sha256RsaPss,
    ];

    /// The configuration name, which is also the fragment of the policy URI.
    pub fn name(self) -> &'static str {
        match self {
            Policy::None => "None",
            Policy::Basic128Rsa15 => "Basic128Rsa15",
            Policy::Basic256 => "Basic256",
            Policy::Basic256Sha256 => "Basic256Sha256",
            Policy::Aes128Sha256RsaOaep => "Aes128_Sha256_RsaOaep",
            Policy::Aes256Sha256RsaPss => "Aes256_Sha256_RsaPss",
        }
    }

    pub fn uri(self) -> String {
        format!("{POLICY_URI_PREFIX}{}", self.name())
    }

    /// A configured name, or the full policy URI.
    pub fn parse(value: &str) -> Option<Policy> {
        let name = value.strip_prefix(POLICY_URI_PREFIX).unwrap_or(value);
        Policy::ALL.into_iter().find(|p| p.name() == name)
    }

    pub fn from_uri(uri: &str) -> Option<Policy> {
        uri.strip_prefix(POLICY_URI_PREFIX).and_then(Policy::parse)
    }

    pub fn is_deprecated(self) -> bool {
        matches!(self, Policy::Basic128Rsa15 | Policy::Basic256)
    }

    pub fn is_secure(self) -> bool {
        self != Policy::None
    }

    /// The RSA key sizes the policy allows, in bits (OPC UA Part 7). `None` for `Policy::None`,
    /// which has no asymmetric key at all. The C build's `POLICIES` table holds the same
    /// bounds, and both must reject the same certificates.
    pub fn key_bits(self) -> Option<(usize, usize)> {
        match self {
            Policy::None => None,
            Policy::Basic128Rsa15 | Policy::Basic256 => Some((1024, 2048)),
            Policy::Basic256Sha256
            | Policy::Aes128Sha256RsaOaep
            | Policy::Aes256Sha256RsaPss => Some((2048, 4096)),
        }
    }
}

/// A message security mode (spec §3.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SecurityMode {
    None,
    Sign,
    SignAndEncrypt,
}

impl SecurityMode {
    pub fn name(self) -> &'static str {
        match self {
            SecurityMode::None => "none",
            SecurityMode::Sign => "sign",
            SecurityMode::SignAndEncrypt => "sign_and_encrypt",
        }
    }

    /// Any case (earlier releases lowercased the value, and the packaged default config says
    /// `"None"`); `signandencrypt` is a spelling they accepted too.
    pub fn parse(value: &str) -> Option<SecurityMode> {
        match value.to_ascii_lowercase().as_str() {
            "none" => Some(SecurityMode::None),
            "sign" => Some(SecurityMode::Sign),
            "sign_and_encrypt" | "signandencrypt" => Some(SecurityMode::SignAndEncrypt),
            _ => None,
        }
    }
}

/// Who a session is activated as.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Identity {
    Anonymous,
    UserName {
        user: String,
        password: Secret,
    },
    X509 {
        certificate: PathBuf,
        private_key: PathBuf,
    },
}

impl Identity {
    /// The identity type named in "identity unsupported:" reasons.
    pub fn kind(&self) -> &'static str {
        match self {
            Identity::Anonymous => "anonymous",
            Identity::UserName { .. } => "username",
            Identity::X509 { .. } => "certificate",
        }
    }
}

/// The validated security settings of one device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceSecurity {
    pub policy: Policy,
    pub mode: SecurityMode,
    pub identity: Identity,
    pub trust_any_server_certificate: bool,
    pub allow_plaintext_password: bool,
}

impl DeviceSecurity {
    pub fn is_secure(&self) -> bool {
        self.policy.is_secure()
    }
}

/// Validate one device's security settings against the `[connection]` defaults (spec §3.1).
///
/// Precedence: a device's `security_policy` wins over the connection's. A device's
/// `security_mode` wins over the connection's too — but a device that sets its own policy and
/// no mode does not inherit the connection's mode, it gets its policy's default (`none` for
/// `None`, `sign_and_encrypt` otherwise), so `security_policy = "None"` alone is always valid.
///
/// Errors name the offending field and never a value.
pub fn device_security(
    conn: &OpcuaConnection,
    ep: &OpcuaEndpoint,
    base_dir: Option<&Path>,
) -> Result<DeviceSecurity, String> {
    let policy_text = ep
        .security_policy
        .as_deref()
        .or(conn.security_policy.as_deref());
    let policy = match policy_text {
        None => Policy::None,
        Some(text) => Policy::parse(text).ok_or_else(|| {
            format!(
                "security_policy '{text}' is not one of {}",
                Policy::ALL.map(Policy::name).join(", ")
            )
        })?,
    };
    let allow_deprecated = ep
        .allow_deprecated_security
        .unwrap_or(conn.allow_deprecated_security);
    if policy.is_deprecated() && !allow_deprecated {
        return Err(format!(
            "security_policy '{}' is deprecated (SHA-1); set allow_deprecated_security = true to use it",
            policy.name()
        ));
    }

    let mode_text = match (&ep.security_mode, &ep.security_policy) {
        (Some(mode), _) => Some(mode.as_str()),
        (None, Some(_)) => None,
        (None, None) => conn.security_mode.as_deref(),
    };
    let mode = match mode_text {
        None if policy.is_secure() => SecurityMode::SignAndEncrypt,
        None => SecurityMode::None,
        Some(text) => SecurityMode::parse(text).ok_or_else(|| {
            format!("security_mode '{text}' is not one of none, sign, sign_and_encrypt")
        })?,
    };
    match (policy.is_secure(), mode) {
        (false, SecurityMode::None) | (true, SecurityMode::Sign | SecurityMode::SignAndEncrypt) => {}
        (false, _) => {
            return Err(format!(
                "security_mode '{}' needs a security_policy other than None",
                mode.name()
            ))
        }
        (true, SecurityMode::None) => {
            return Err(format!(
                "security_mode 'none' cannot be used with security_policy '{}' (use sign or sign_and_encrypt)",
                policy.name()
            ))
        }
    }

    Ok(DeviceSecurity {
        policy,
        mode,
        identity: identity(ep, base_dir)?,
        trust_any_server_certificate: ep
            .trust_any_server_certificate
            .unwrap_or(conn.trust_any_server_certificate),
        allow_plaintext_password: ep
            .allow_plaintext_password
            .unwrap_or(conn.allow_plaintext_password),
    })
}

fn identity(ep: &OpcuaEndpoint, base_dir: Option<&Path>) -> Result<Identity, String> {
    let username_fields = ep.user.is_some() || ep.password.is_some() || ep.password_file.is_some();
    let cert_fields = ep.user_certificate.is_some() || ep.user_private_key.is_some();
    if username_fields && cert_fields {
        return Err(
            "user/password/password_file and user_certificate/user_private_key are both set; use one identity"
                .into(),
        );
    }
    if cert_fields {
        return match (&ep.user_certificate, &ep.user_private_key) {
            (Some(cert), Some(key)) => {
                let private_key = resolve_path(base_dir, key);
                check_private_key_mode(&private_key, "user_private_key")?;
                Ok(Identity::X509 {
                    certificate: resolve_path(base_dir, cert),
                    private_key,
                })
            }
            (Some(_), None) => Err("user_certificate needs user_private_key".into()),
            _ => Err("user_private_key needs user_certificate".into()),
        };
    }
    let Some(user) = &ep.user else {
        if ep.password.is_some() || ep.password_file.is_some() {
            return Err("password/password_file needs user".into());
        }
        return Ok(Identity::Anonymous);
    };
    let password = match (&ep.password, &ep.password_file) {
        (Some(_), Some(_)) => {
            return Err("password and password_file are both set; use one".into());
        }
        (Some(p), None) => p.clone(),
        (None, Some(file)) => read_password_file(&resolve_path(base_dir, file))?,
        (None, None) => Secret::default(),
    };
    Ok(Identity::UserName {
        user: user.clone(),
        password,
    })
}

/// The first line of a password file, without its line ending. The error names the path only.
pub fn read_password_file(path: &Path) -> Result<Secret, String> {
    let text = std::fs::read_to_string(path).map_err(|e| {
        format!(
            "password_file '{}' cannot be read: {}",
            path.display(),
            io_kind(&e)
        )
    })?;
    let secret = Secret::new(text.lines().next().unwrap_or_default().to_string());
    // `text` holds the secret as well.
    let mut text = text;
    // SAFETY: zero bytes are valid UTF-8.
    unsafe { text.as_bytes_mut() }.fill(0);
    Ok(secret)
}

/// An I/O error without anything it might have quoted from the file.
fn io_kind(e: &std::io::Error) -> String {
    match e.kind() {
        std::io::ErrorKind::InvalidData => "not valid UTF-8".to_string(),
        _ => e.to_string(),
    }
}

/// Refuse a private key file that other users may read (group or other permission bits).
/// A missing file is not an error here; loading it reports that.
pub fn check_private_key_mode(path: &Path, field: &str) -> Result<(), String> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(meta) = std::fs::metadata(path) {
            let mode = meta.permissions().mode() & 0o777;
            if mode & 0o077 != 0 {
                return Err(format!(
                    "{field} '{}' is accessible by other users (mode {mode:04o}); restrict it to the owner (chmod 600)",
                    path.display()
                ));
            }
        }
    }
    #[cfg(not(unix))]
    let _ = (path, field);
    Ok(())
}

/// `path` unchanged when absolute, joined to `base_dir` when relative.
pub fn resolve_path(base_dir: Option<&Path>, path: &str) -> PathBuf {
    let p = Path::new(path);
    match base_dir {
        Some(base) if p.is_relative() => base.join(p),
        _ => p.to_path_buf(),
    }
}

fn default_app_name() -> String {
    "tedge-dot".to_string()
}
fn default_app_uri() -> String {
    "urn:tedge-dot".to_string()
}
fn default_connect_timeout() -> u64 {
    15
}
fn default_request_timeout() -> u64 {
    5
}
fn default_true() -> bool {
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn endpoint(v: serde_json::Value) -> OpcuaEndpoint {
        let mut v = v;
        v["endpoint"] = json!("opc.tcp://localhost:4840/");
        serde_json::from_value(v).unwrap()
    }

    fn connection(v: serde_json::Value) -> OpcuaConnection {
        OpcuaConnection::from_value(&v).unwrap()
    }

    fn security(conn: serde_json::Value, ep: serde_json::Value) -> Result<DeviceSecurity, String> {
        device_security(&connection(conn), &endpoint(ep), None)
    }

    #[test]
    fn parse_endpoint() {
        let v = json!({
            "endpoint": "opc.tcp://localhost:4840/",
            "security_policy": "None",
            "security_mode": "none"
        });
        let e: OpcuaEndpoint = serde_json::from_value(v).unwrap();
        assert_eq!(e.endpoint, "opc.tcp://localhost:4840/");
        assert_eq!(e.security_policy.as_deref(), Some("None"));
        assert!(e.user.is_none());
    }

    #[test]
    fn parse_node_address_textual() {
        let v = json!({ "node_id": "ns=2;s=Temperature" });
        let a: NodeAddress = serde_json::from_value(v).unwrap();
        assert_eq!(a.node_id.as_deref(), Some("ns=2;s=Temperature"));
    }

    #[test]
    fn parse_node_address_structured_string() {
        let v = json!({ "namespace": 2, "identifier": "Temperature" });
        let a: NodeAddress = serde_json::from_value(v).unwrap();
        assert_eq!(a.namespace, Some(2));
        assert_eq!(a.identifier.unwrap().as_str(), Some("Temperature"));
    }

    #[test]
    fn parse_node_address_structured_numeric() {
        let v = json!({ "namespace": 3, "identifier": 1001 });
        let a: NodeAddress = serde_json::from_value(v).unwrap();
        assert_eq!(a.namespace, Some(3));
        assert_eq!(a.identifier.unwrap().as_u64(), Some(1001));
    }

    #[test]
    fn parse_connection_security_fields() {
        let c = connection(json!({
            "pki_dir": "pki",
            "certificate": "own.der",
            "private_key": "own.pem",
            "create_certificate": false,
            "trust_any_server_certificate": true,
            "allow_deprecated_security": true,
            "allow_plaintext_password": true,
        }));
        assert_eq!(c.pki_dir.as_deref(), Some("pki"));
        assert_eq!(c.certificate.as_deref(), Some("own.der"));
        assert_eq!(c.private_key.as_deref(), Some("own.pem"));
        assert!(!c.create_certificate);
        assert!(c.trust_any_server_certificate);
        assert!(c.allow_deprecated_security);
        assert!(c.allow_plaintext_password);
    }

    #[test]
    fn connection_defaults() {
        let c = OpcuaConnection::from_value(&serde_json::Value::Null).unwrap();
        assert!(c.create_certificate);
        assert!(!c.trust_any_server_certificate);
        assert_eq!(c.pki_dir(None), PathBuf::from(DEFAULT_PKI_DIR));
    }

    #[test]
    fn invalid_connection_is_an_error() {
        let e = OpcuaConnection::from_value(&json!({ "create_certificate": "yes" })).unwrap_err();
        assert!(e.contains("[connection]"), "{e}");
    }

    #[test]
    fn parse_endpoint_security_fields() {
        let e = endpoint(json!({
            "user": "operator",
            "password_file": "secret",
            "user_certificate": "u.der",
            "user_private_key": "u.pem",
            "trust_any_server_certificate": true,
            "allow_deprecated_security": false,
            "allow_plaintext_password": true,
        }));
        assert_eq!(e.password_file.as_deref(), Some("secret"));
        assert_eq!(e.user_certificate.as_deref(), Some("u.der"));
        assert_eq!(e.user_private_key.as_deref(), Some("u.pem"));
        assert_eq!(e.trust_any_server_certificate, Some(true));
        assert_eq!(e.allow_deprecated_security, Some(false));
        assert_eq!(e.allow_plaintext_password, Some(true));
    }

    #[test]
    fn secret_debug_is_redacted() {
        let e = endpoint(json!({ "user": "u", "password": "s3cret" }));
        assert!(!format!("{e:?}").contains("s3cret"));
        assert_eq!(e.password.as_ref().unwrap().expose(), "s3cret");
    }

    #[test]
    fn non_string_password_is_not_quoted() {
        let err = serde_json::from_value::<OpcuaEndpoint>(json!({
            "endpoint": "opc.tcp://x:4840/", "user": "u", "password": 987654
        }))
        .unwrap_err()
        .to_string();
        assert!(!err.contains("987654"), "{err}");
    }

    #[test]
    fn defaults_are_unsecured() {
        let s = security(json!({}), json!({})).unwrap();
        assert_eq!((s.policy, s.mode), (Policy::None, SecurityMode::None));
        assert_eq!(s.identity, Identity::Anonymous);
        assert!(!s.trust_any_server_certificate);
    }

    #[test]
    fn device_policy_overrides_connection() {
        let conn = json!({ "security_policy": "Basic256Sha256" });
        let s = security(
            conn.clone(),
            json!({ "security_policy": "None", "security_mode": "none" }),
        )
        .unwrap();
        assert_eq!((s.policy, s.mode), (Policy::None, SecurityMode::None));
        let s = security(conn, json!({})).unwrap();
        assert_eq!(
            (s.policy, s.mode),
            (Policy::Basic256Sha256, SecurityMode::SignAndEncrypt)
        );
    }

    #[test]
    fn device_policy_does_not_inherit_connection_mode() {
        let conn = json!({ "security_policy": "Basic256Sha256", "security_mode": "sign" });
        let s = security(conn.clone(), json!({ "security_policy": "None" })).unwrap();
        assert_eq!(s.mode, SecurityMode::None);
        let s = security(conn, json!({})).unwrap();
        assert_eq!(s.mode, SecurityMode::Sign);
    }

    #[test]
    fn policy_names_and_uris() {
        for p in Policy::ALL {
            assert_eq!(Policy::parse(p.name()), Some(p));
            assert_eq!(Policy::parse(&p.uri()), Some(p));
            assert_eq!(Policy::from_uri(&p.uri()), Some(p));
        }
        assert_eq!(Policy::parse("ECC_nistP256"), None);
        assert_eq!(Policy::parse("basic256sha256"), None);
    }

    /// These bounds are duplicated in the C build's `POLICIES` table
    /// (impl/c/connectors/opcua/connector_opcua.c); the two must refuse the same certificates,
    /// or the implementations disagree about which servers are usable. The 1024-bit case is
    /// not hypothetical: the UA-.NETStandard reference server generates exactly that.
    #[test]
    fn policy_key_bits_match_the_c_table() {
        assert_eq!(Policy::None.key_bits(), None);
        assert_eq!(Policy::Basic128Rsa15.key_bits(), Some((1024, 2048)));
        assert_eq!(Policy::Basic256.key_bits(), Some((1024, 2048)));
        assert_eq!(Policy::Basic256Sha256.key_bits(), Some((2048, 4096)));
        assert_eq!(Policy::Aes128Sha256RsaOaep.key_bits(), Some((2048, 4096)));
        assert_eq!(Policy::Aes256Sha256RsaPss.key_bits(), Some((2048, 4096)));

        // A 1024-bit key is acceptable to the deprecated policies and to nothing else.
        for p in Policy::ALL {
            let Some((min, max)) = p.key_bits() else { continue };
            assert_eq!(
                1024 >= min && 1024 <= max,
                p.is_deprecated(),
                "{} accepts 1024 bits?",
                p.name()
            );
            assert!(2048 >= min && 2048 <= max, "{} rejects 2048 bits", p.name());
        }
    }

    #[test]
    fn unknown_policy_names_the_field() {
        let e = security(json!({}), json!({ "security_policy": "Basic999" })).unwrap_err();
        assert!(e.starts_with("security_policy 'Basic999'"), "{e}");
    }

    #[test]
    fn modes_are_case_insensitive() {
        let s = security(
            json!({ "security_policy": "None", "security_mode": "None" }),
            json!({}),
        )
        .unwrap();
        assert_eq!(s.mode, SecurityMode::None);
        let s = security(
            json!({}),
            json!({ "security_policy": "Basic256Sha256", "security_mode": "Sign_And_Encrypt" }),
        )
        .unwrap();
        assert_eq!(s.mode, SecurityMode::SignAndEncrypt);
    }

    #[test]
    fn unknown_mode_names_the_field() {
        let e = security(
            json!({}),
            json!({ "security_policy": "Basic256Sha256", "security_mode": "encrypt" }),
        )
        .unwrap_err();
        assert!(e.starts_with("security_mode 'encrypt'"), "{e}");
    }

    #[test]
    fn inconsistent_pairs_are_rejected() {
        let e = security(
            json!({}),
            json!({ "security_policy": "Basic256Sha256", "security_mode": "none" }),
        )
        .unwrap_err();
        assert!(e.contains("security_mode"), "{e}");
        let e = security(
            json!({}),
            json!({ "security_policy": "None", "security_mode": "sign" }),
        )
        .unwrap_err();
        assert!(e.contains("security_mode"), "{e}");
        let e = security(json!({}), json!({ "security_mode": "sign_and_encrypt" })).unwrap_err();
        assert!(e.contains("security_mode"), "{e}");
    }

    #[test]
    fn every_secure_policy_accepts_both_secure_modes() {
        for p in Policy::ALL.into_iter().filter(|p| p.is_secure()) {
            for m in ["sign", "sign_and_encrypt", "signandencrypt"] {
                let s = security(
                    json!({ "allow_deprecated_security": true }),
                    json!({ "security_policy": p.name(), "security_mode": m }),
                )
                .unwrap();
                assert_eq!(s.policy, p);
            }
        }
    }

    #[test]
    fn deprecated_policy_needs_opt_in() {
        for p in ["Basic256", "Basic128Rsa15"] {
            let e = security(json!({}), json!({ "security_policy": p })).unwrap_err();
            assert!(
                e.contains("deprecated") && e.contains("allow_deprecated_security"),
                "{e}"
            );
            assert!(security(
                json!({ "allow_deprecated_security": true }),
                json!({ "security_policy": p })
            )
            .is_ok());
            assert!(security(
                json!({}),
                json!({ "security_policy": p, "allow_deprecated_security": true })
            )
            .is_ok());
        }
        // A device may also withdraw the connection's opt-in.
        assert!(security(
            json!({ "allow_deprecated_security": true }),
            json!({ "security_policy": "Basic256", "allow_deprecated_security": false })
        )
        .is_err());
    }

    #[test]
    fn switches_fall_back_to_connection() {
        let s = security(
            json!({ "trust_any_server_certificate": true, "allow_plaintext_password": true }),
            json!({}),
        )
        .unwrap();
        assert!(s.trust_any_server_certificate && s.allow_plaintext_password);
        let s = security(
            json!({ "trust_any_server_certificate": true }),
            json!({ "trust_any_server_certificate": false }),
        )
        .unwrap();
        assert!(!s.trust_any_server_certificate);
    }

    #[test]
    fn username_identity() {
        let s = security(json!({}), json!({ "user": "operator", "password": "pw" })).unwrap();
        assert_eq!(
            s.identity,
            Identity::UserName {
                user: "operator".into(),
                password: Secret::new("pw".into())
            }
        );
        // A user without a password is allowed (empty password).
        let s = security(json!({}), json!({ "user": "operator" })).unwrap();
        assert_eq!(s.identity.kind(), "username");
    }

    #[test]
    fn password_file_first_line() {
        let dir = tempdir();
        std::fs::write(dir.join("pw"), "line-one\r\nline-two\n").unwrap();
        let ep = endpoint(json!({ "user": "operator", "password_file": "pw" }));
        let s = device_security(&OpcuaConnection::default(), &ep, Some(&dir)).unwrap();
        match s.identity {
            Identity::UserName { password, .. } => assert_eq!(password.expose(), "line-one"),
            other => panic!("expected a username identity, got {}", other.kind()),
        }
    }

    #[test]
    fn unreadable_password_file_names_the_path() {
        let e = security(
            json!({}),
            json!({ "user": "u", "password_file": "/nonexistent/pw" }),
        )
        .unwrap_err();
        assert!(e.contains("password_file '/nonexistent/pw'"), "{e}");
    }

    #[test]
    fn ambiguous_identities_are_rejected() {
        let cases = [
            (
                json!({ "user": "u", "password": "a", "password_file": "b" }),
                "password_file",
            ),
            (
                json!({ "user": "u", "user_certificate": "c", "user_private_key": "k" }),
                "user_certificate",
            ),
            (json!({ "password": "a" }), "needs user"),
            (json!({ "password_file": "a" }), "needs user"),
            (json!({ "user_certificate": "c" }), "user_private_key"),
            (json!({ "user_private_key": "k" }), "user_certificate"),
        ];
        for (ep, needle) in cases {
            let e = security(json!({}), ep.clone()).unwrap_err();
            assert!(e.contains(needle), "{ep}: {e}");
        }
    }

    #[test]
    fn x509_identity_paths_resolve_against_base_dir() {
        let dir = tempdir();
        let ep = endpoint(json!({ "user_certificate": "u.der", "user_private_key": "/abs/u.pem" }));
        let s = device_security(&OpcuaConnection::default(), &ep, Some(&dir)).unwrap();
        assert_eq!(
            s.identity,
            Identity::X509 {
                certificate: dir.join("u.der"),
                private_key: PathBuf::from("/abs/u.pem")
            }
        );
    }

    #[cfg(unix)]
    #[test]
    fn readable_private_key_is_rejected() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempdir();
        let key = dir.join("u.pem");
        std::fs::write(&key, "key").unwrap();
        std::fs::set_permissions(&key, std::fs::Permissions::from_mode(0o640)).unwrap();
        let ep = endpoint(json!({ "user_certificate": "u.der", "user_private_key": "u.pem" }));
        let e = device_security(&OpcuaConnection::default(), &ep, Some(&dir)).unwrap_err();
        assert!(e.contains("user_private_key") && e.contains("0640"), "{e}");
        std::fs::set_permissions(&key, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(device_security(&OpcuaConnection::default(), &ep, Some(&dir)).is_ok());
    }

    #[test]
    fn relative_pki_dir_resolves_against_base_dir() {
        let c = connection(json!({ "pki_dir": "pki" }));
        assert_eq!(
            c.pki_dir(Some(Path::new("/etc/tedge/plugins/ot"))),
            PathBuf::from("/etc/tedge/plugins/ot/pki")
        );
        let c = connection(json!({ "pki_dir": "/srv/pki" }));
        assert_eq!(
            c.pki_dir(Some(Path::new("/etc"))),
            PathBuf::from("/srv/pki")
        );
    }

    fn tempdir() -> PathBuf {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static N: AtomicUsize = AtomicUsize::new(0);
        let dir = std::env::temp_dir().join(format!(
            "opcua-config-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }
}
