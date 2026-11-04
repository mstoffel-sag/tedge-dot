//! The HTTP layer (spec §3.1, §7): one client per `(base_url, auth, tls)`, authentication,
//! TLS, timeouts, the CSRF header, request chunking, and PI's error bodies classified the way
//! the link status needs them. Nothing here knows about points or samples.

use crate::config::{Auth, Settings, Tls};
use reqwest::header::{HeaderMap, HeaderValue, AUTHORIZATION};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::CryptoProvider;
use rustls::pki_types::{pem::PemObject, CertificateDer, PrivateKeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, DigitallySignedStruct, RootCertStore, SignatureScheme};
use serde_json::Value as Json;
use std::fmt;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};
use tracing::warn;

pub type Channel = WebSocketStream<MaybeTlsStream<tokio::net::TcpStream>>;

/// What went wrong with a request, as the link status and the command results need it.
#[derive(Clone, Debug, PartialEq)]
pub enum HttpError {
    /// 401 or 403: the credentials were refused.
    Unauthorized(u16),
    /// 414, or a 400 without a PI `Errors` body (HTTP.sys's answer to a long request).
    TooLong(u16),
    /// Any other unsuccessful status, with PI's `Errors` text.
    Status { code: u16, errors: String },
    /// The request or the connection setup timed out.
    Timeout(String),
    /// DNS, TCP, TLS or I/O, with the cause chain.
    Transport(String),
}

impl HttpError {
    /// Whether the failure says the device is unreachable (the link degrades), rather than that
    /// one request was refused.
    pub fn is_link_failure(&self) -> bool {
        match self {
            HttpError::Timeout(_) | HttpError::Transport(_) | HttpError::Unauthorized(_) => true,
            HttpError::Status { code, .. } => *code >= 500,
            HttpError::TooLong(_) => false,
        }
    }
}

impl fmt::Display for HttpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            HttpError::Unauthorized(_) => write!(f, "unauthorized"),
            HttpError::TooLong(code) => write!(f, "HTTP {code}: request too long"),
            HttpError::Status { code, errors } if errors.is_empty() => write!(f, "HTTP {code}"),
            HttpError::Status { code, errors } => write!(f, "HTTP {code}: {errors}"),
            HttpError::Timeout(what) => write!(f, "timeout: {what}"),
            HttpError::Transport(what) => write!(f, "{what}"),
        }
    }
}

/// An error and its sources, joined: reqwest and tungstenite wrap the rustls cause, which is
/// what names a TLS failure ("invalid peer certificate: ...").
fn chain(e: &(dyn std::error::Error + 'static)) -> String {
    let mut parts = vec![e.to_string()];
    let mut source = e.source();
    while let Some(s) = source {
        let text = s.to_string();
        if !parts.iter().any(|p| p.contains(&text)) {
            parts.push(text);
        }
        source = s.source();
    }
    parts.join(": ")
}

fn from_reqwest(e: reqwest::Error) -> HttpError {
    if e.is_timeout() {
        HttpError::Timeout(chain(&e))
    } else {
        HttpError::Transport(chain(&e))
    }
}

/// Percent-encode a query component (RFC 3986 unreserved characters stay).
pub fn encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => out.push(b as char),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

/// A query string from pairs, encoded.
pub fn query(pairs: &[(&str, &str)]) -> String {
    pairs
        .iter()
        .map(|(k, v)| format!("{}={}", encode(k), encode(v)))
        .collect::<Vec<_>>()
        .join("&")
}

/// Accepts any server certificate: `tls.insecure_skip_verify`. Signatures are still checked, so
/// the handshake is a real one; only the certificate's trust and name are not.
#[derive(Debug)]
struct NoVerify(Arc<CryptoProvider>);

impl ServerCertVerifier for NoVerify {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(message, cert, dss, &self.0.signature_verification_algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(message, cert, dss, &self.0.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

/// The rustls configuration HTTPS and the Channels WebSocket share.
pub fn tls_config(tls: &Tls, auth: &Auth) -> Result<ClientConfig, String> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let builder = ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .map_err(|e| format!("TLS: {e}"))?;
    let builder = if tls.insecure_skip_verify {
        builder
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(NoVerify(provider)))
    } else {
        let mut roots = RootCertStore::empty();
        let native = rustls_native_certs::load_native_certs();
        let (added, _) = roots.add_parsable_certificates(native.certs);
        if let Some(ca) = &tls.ca_file {
            let certs: Vec<CertificateDer<'static>> = CertificateDer::pem_file_iter(ca)
                .map_err(|e| format!("tls.ca_file {}: {e}", ca.display()))?
                .collect::<Result<_, _>>()
                .map_err(|e| format!("tls.ca_file {}: {e}", ca.display()))?;
            if certs.is_empty() {
                return Err(format!("tls.ca_file {}: no certificate in the file", ca.display()));
            }
            for cert in certs {
                roots
                    .add(cert)
                    .map_err(|e| format!("tls.ca_file {}: {e}", ca.display()))?;
            }
        } else if added == 0 {
            warn!("no trusted CA certificates found in the system store; set tls.ca_file");
        }
        builder.with_root_certificates(roots)
    };
    match auth {
        Auth::Certificate { cert_file, key_file } => {
            let chain: Vec<CertificateDer<'static>> = CertificateDer::pem_file_iter(cert_file)
                .map_err(|e| format!("auth.cert_file {}: {e}", cert_file.display()))?
                .collect::<Result<_, _>>()
                .map_err(|e| format!("auth.cert_file {}: {e}", cert_file.display()))?;
            let key = PrivateKeyDer::from_pem_file(key_file)
                .map_err(|e| format!("auth.key_file {}: {e}", key_file.display()))?;
            builder
                .with_client_auth_cert(chain, key)
                .map_err(|e| format!("auth.cert_file/key_file: {e}"))
        }
        _ => Ok(builder.with_no_client_auth()),
    }
}

fn read_secret(path: &std::path::Path, what: &str) -> Result<String, String> {
    std::fs::read_to_string(path)
        .map(|s| s.trim_end_matches(['\n', '\r']).to_string())
        .map_err(|e| format!("{what} {}: {e}", path.display()))
}

/// How a request authenticates (a client certificate is part of the TLS configuration).
enum Credentials {
    None,
    Basic(String),
    Bearer { file: std::path::PathBuf, token: Mutex<String> },
}

impl Credentials {
    fn header(&self) -> Option<String> {
        match self {
            Credentials::None => None,
            Credentials::Basic(h) => Some(h.clone()),
            Credentials::Bearer { token, .. } => Some(format!("Bearer {}", token.lock().unwrap())),
        }
    }

    /// After a 401: re-read the token file. True when it held a different token, which is then
    /// worth one retry.
    fn refresh(&self) -> bool {
        let Credentials::Bearer { file, token } = self else {
            return false;
        };
        match read_secret(file, "auth.token_file") {
            Ok(new) => {
                let mut current = token.lock().unwrap();
                if *current != new {
                    *current = new;
                    return true;
                }
                false
            }
            Err(e) => {
                warn!("{e}");
                false
            }
        }
    }
}

/// A PI Web API client: one per `(base_url, auth, tls)`, shared by the devices that name it.
pub struct Client {
    http: reqwest::Client,
    base: String,
    credentials: Credentials,
    tls: Arc<ClientConfig>,
    ws_timeout: std::time::Duration,
}

impl Client {
    pub fn new(base_url: &str, settings: &Settings) -> Result<Client, String> {
        // The config refuses this already (config::device); never read a secret for plain HTTP.
        if !base_url.starts_with("https://") && !matches!(settings.auth, Auth::None) {
            return Err(format!("{base_url}: credentials are only sent over https://"));
        }
        let tls = Arc::new(tls_config(&settings.tls, &settings.auth)?);
        let credentials = match &settings.auth {
            Auth::None | Auth::Certificate { .. } => Credentials::None,
            Auth::Basic { username, password_file } => {
                use base64::Engine;
                let password = read_secret(password_file, "auth.password_file")?;
                let token = base64::engine::general_purpose::STANDARD.encode(format!("{username}:{password}"));
                Credentials::Basic(format!("Basic {token}"))
            }
            Auth::Bearer { token_file } => Credentials::Bearer {
                file: token_file.clone(),
                token: Mutex::new(read_secret(token_file, "auth.token_file")?),
            },
        };
        let http = reqwest::Client::builder()
            .tls_backend_preconfigured((*tls).clone())
            .timeout(settings.request_timeout)
            .connect_timeout(settings.connect_timeout)
            .build()
            .map_err(|e| chain(&e))?;
        Ok(Client {
            http,
            base: base_url.trim_end_matches('/').to_string(),
            credentials,
            tls,
            ws_timeout: settings.connect_timeout + settings.request_timeout,
        })
    }

    /// The URL of `path` (relative to the PI Web API root) with an encoded query.
    pub fn url(&self, path: &str, query: &str) -> String {
        if query.is_empty() {
            format!("{}/{}", self.base, path)
        } else {
            format!("{}/{}?{}", self.base, path, query)
        }
    }

    async fn send(&self, build: impl Fn(Option<String>) -> reqwest::RequestBuilder) -> Result<reqwest::Response, HttpError> {
        let mut retried = false;
        loop {
            let response = build(self.credentials.header()).send().await.map_err(from_reqwest)?;
            let status = response.status().as_u16();
            if status == 401 && !retried && self.credentials.refresh() {
                retried = true;
                continue;
            }
            return Ok(response);
        }
    }

    /// The body of a successful answer, or the classified failure. `refused_is_auth`: a 403 means
    /// the credentials were refused (a read), rather than this one request (a write).
    async fn classify(response: reqwest::Response, refused_is_auth: bool) -> Result<Json, HttpError> {
        let status = response.status().as_u16();
        let body = response.bytes().await.map_err(from_reqwest)?;
        let json: Option<Json> = serde_json::from_slice(&body).ok();
        let errors = json.as_ref().and_then(crate::value::errors_of);
        match status {
            200..=299 => {
                let json = json.unwrap_or(Json::Null);
                // A 200 can still carry a failure of the request as a whole.
                if let Some(ex) = json.get("WebException") {
                    let code = ex.get("StatusCode").and_then(Json::as_u64).unwrap_or(500) as u16;
                    return Err(HttpError::Status { code, errors: crate::value::errors_of(ex).unwrap_or_default() });
                }
                Ok(json)
            }
            401 => Err(HttpError::Unauthorized(status)),
            403 if refused_is_auth => Err(HttpError::Unauthorized(status)),
            414 => Err(HttpError::TooLong(414)),
            400 if errors.is_none() => Err(HttpError::TooLong(400)),
            _ => Err(HttpError::Status { code: status, errors: errors.unwrap_or_default() }),
        }
    }

    /// `GET path?query`, the JSON body of a successful answer.
    pub async fn get(&self, path: &str, query: &str) -> Result<Json, HttpError> {
        let url = self.url(path, query);
        let response = self
            .send(|auth| {
                let mut r = self.http.get(&url);
                if let Some(a) = auth {
                    r = r.header(AUTHORIZATION, a);
                }
                r
            })
            .await?;
        Self::classify(response, true).await
    }

    /// `POST path?query` with a JSON body and the CSRF header; the status of a successful answer.
    pub async fn post(&self, path: &str, query: &str, body: &Json) -> Result<u16, HttpError> {
        let url = self.url(path, query);
        let response = self
            .send(|auth| {
                let mut r = self
                    .http
                    .post(&url)
                    .header("X-Requested-With", "XMLHttpRequest")
                    .json(body);
                if let Some(a) = auth {
                    r = r.header(AUTHORIZATION, a);
                }
                r
            })
            .await?;
        let status = response.status().as_u16();
        Self::classify(response, false).await.map(|_| status)
    }

    /// Open a Channels WebSocket (`streamsets/channel`). An answer other than 101 is
    /// [`HttpError::Status`] (or `Unauthorized`): the caller decides whether to fall back.
    pub async fn channel(&self, query: &str) -> Result<Channel, HttpError> {
        let url = self.url("streamsets/channel", query);
        let ws_url = if let Some(rest) = url.strip_prefix("https://") {
            format!("wss://{rest}")
        } else if let Some(rest) = url.strip_prefix("http://") {
            format!("ws://{rest}")
        } else {
            url
        };
        let mut retried = false;
        loop {
            let mut request = ws_url
                .as_str()
                .into_client_request()
                .map_err(|e| HttpError::Transport(chain(&e)))?;
            let mut headers = HeaderMap::new();
            if let Some(a) = self.credentials.header() {
                headers.insert(AUTHORIZATION, HeaderValue::from_str(&a).map_err(|e| HttpError::Transport(e.to_string()))?);
            }
            request.headers_mut().extend(headers);
            let connector = if ws_url.starts_with("wss://") {
                tokio_tungstenite::Connector::Rustls(self.tls.clone())
            } else {
                tokio_tungstenite::Connector::Plain
            };
            let open = tokio_tungstenite::connect_async_tls_with_config(request, None, false, Some(connector));
            match tokio::time::timeout(self.ws_timeout, open).await {
                Err(_) => return Err(HttpError::Timeout("opening the channel".into())),
                Ok(Ok((ws, _))) => return Ok(ws),
                Ok(Err(tokio_tungstenite::tungstenite::Error::Http(response))) => {
                    let code = response.status().as_u16();
                    if code == 401 && !retried && self.credentials.refresh() {
                        retried = true;
                        continue;
                    }
                    // Only 401: a 403 to the upgrade, after REST requests with the same
                    // credentials succeeded, is a refusal of the WebSocket (spec §5.2).
                    if code == 401 {
                        return Err(HttpError::Unauthorized(code));
                    }
                    let errors = response
                        .body()
                        .as_ref()
                        .and_then(|b| serde_json::from_slice::<Json>(b).ok())
                        .and_then(|j| crate::value::errors_of(&j))
                        .unwrap_or_default();
                    return Err(HttpError::Status { code, errors });
                }
                Ok(Err(e)) => return Err(HttpError::Transport(chain(&e))),
            }
        }
    }
}

/// Split `web_ids` into requests of at most `chunk` ids whose URL stays within `max_url_bytes`
/// (the fixed part of the URL is `fixed` bytes).
pub fn chunks(web_ids: &[String], chunk: usize, max_url_bytes: usize, fixed: usize) -> Vec<Vec<String>> {
    let mut out: Vec<Vec<String>> = Vec::new();
    let mut current: Vec<String> = Vec::new();
    let mut len = fixed;
    for id in web_ids {
        let add = "&webId=".len() + id.len();
        if !current.is_empty() && (current.len() >= chunk.max(1) || len + add > max_url_bytes) {
            out.push(std::mem::take(&mut current));
            len = fixed;
        }
        len += add;
        current.push(id.clone());
    }
    if !current.is_empty() {
        out.push(current);
    }
    out
}

/// A device's current chunk size: halved on a "too long" answer, logged once (spec §3.1).
#[derive(Debug)]
pub struct ChunkSize {
    size: AtomicUsize,
    warned: std::sync::atomic::AtomicBool,
}

impl ChunkSize {
    pub fn new(size: usize) -> Self {
        ChunkSize { size: AtomicUsize::new(size.max(1)), warned: Default::default() }
    }

    pub fn get(&self) -> usize {
        self.size.load(Ordering::Relaxed)
    }

    /// Halve after a "too long" answer for a chunk of `tried` ids. False when it cannot shrink.
    pub fn halve(&self, device: &str, tried: usize, code: u16) -> bool {
        if tried <= 1 {
            return false;
        }
        let next = (tried / 2).max(1);
        self.size.fetch_min(next, Ordering::Relaxed);
        if !self.warned.swap(true, Ordering::Relaxed) {
            warn!(%device, "the server answered {code} to a request with {tried} WebIds; using at most {next} per request");
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn query_encoding_keeps_paths_intact() {
        assert_eq!(encode("\\\\PISRV01\\SINUSOID"), "%5C%5CPISRV01%5CSINUSOID");
        assert_eq!(encode("a|b c"), "a%7Cb%20c");
        assert_eq!(query(&[("path", "\\\\S\\T"), ("selectedFields", "WebId;Name")]), "path=%5C%5CS%5CT&selectedFields=WebId%3BName");
    }

    #[test]
    fn chunks_respect_count_and_url_length() {
        let ids: Vec<String> = (0..120).map(|i| format!("F1DP{i:040}")).collect();
        let by_count = chunks(&ids, 50, 1_000_000, 100);
        assert_eq!(by_count.iter().map(Vec::len).collect::<Vec<_>>(), vec![50, 50, 20]);
        let by_url = chunks(&ids, 50, 100 + 10 * (7 + 44), 100);
        assert!(by_url.iter().all(|c| c.len() <= 10));
        assert_eq!(by_url.iter().map(Vec::len).sum::<usize>(), 120);
    }

    #[test]
    fn chunk_size_halves_down_to_one() {
        let c = ChunkSize::new(50);
        assert!(c.halve("d", 50, 414));
        assert_eq!(c.get(), 25);
        assert!(c.halve("d", 3, 414));
        assert_eq!(c.get(), 1);
        assert!(!c.halve("d", 1, 414));
    }

    #[test]
    fn link_failures() {
        assert!(HttpError::Timeout("x".into()).is_link_failure());
        assert!(HttpError::Status { code: 503, errors: String::new() }.is_link_failure());
        assert!(!HttpError::Status { code: 404, errors: String::new() }.is_link_failure());
        assert_eq!(HttpError::Unauthorized(401).to_string(), "unauthorized");
    }
}
