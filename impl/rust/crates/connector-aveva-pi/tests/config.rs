//! Configuration loading (spec §3, openspec aveva-pi-web-api-connection): every load error
//! names the device or point and the key.

use connector_aveva_pi::config::{Address, Auth, Delivery};
use tedge_dot_sdk::ConnectorConfig;

fn load(text: &str) -> Result<connector_aveva_pi::config::Device, String> {
    let config: ConnectorConfig = toml::from_str(text).map_err(|e| e.to_string())?;
    let mut c = connector_aveva_pi::factory();
    c.configure(&config).map_err(|e| e.to_string())?;
    let conn = connector_aveva_pi::config::connection(&config.connection)?;
    let d = &config.devices[0];
    connector_aveva_pi::config::device(&d.name, &d.protocol_address, &conn, &d.points, d.default_mode)
}

fn device(address: &str, point_address: &str, extra: &str) -> String {
    format!(
        r#"
[connector]
protocol = "aveva-pi"

[connection]
{extra}

[[device]]
name = "pi"
protocol_address = {address}

  [[device.point]]
  id = "p"
  datatype = "float64"
  address = {point_address}
"#
    )
}

const ADDR: &str = r#"{ base_url = "https://pi.example.com/piwebapi/", data_server = "PISRV01", af_database = '\\AF\Plant' }"#;

#[test]
fn addresses_resolve_to_full_paths() {
    let d = load(&device(ADDR, r#"{ tag = "SINUSOID" }"#, "")).unwrap();
    assert_eq!(d.base_url, "https://pi.example.com/piwebapi");
    assert_eq!(d.points[0].address, Address::Point("\\\\PISRV01\\SINUSOID".into()));
    let d = load(&device(ADDR, r#"{ path = 'Line1\Pump01|Pressure' }"#, "")).unwrap();
    assert_eq!(d.points[0].address, Address::Attribute("\\\\AF\\Plant\\Line1\\Pump01|Pressure".into()));
    let d = load(&device(ADDR, r#"{ path = '\\AF\Plant\Pump01|Pressure' }"#, "")).unwrap();
    assert_eq!(d.points[0].address.kind(), "attribute");
    let d = load(&device(ADDR, r#"{ web_id = "F1DPabc" }"#, "")).unwrap();
    assert_eq!(d.points[0].address, Address::WebId("F1DPabc".into()));
    assert_eq!(d.delivery, Delivery::Channel);
}

#[test]
fn tag_without_data_server() {
    let e = load(&device(r#"{ base_url = "https://pi/piwebapi" }"#, r#"{ tag = "SINUSOID" }"#, "")).unwrap_err();
    assert!(e.contains("point 'p'") && e.contains("data_server"), "{e}");
}

#[test]
fn two_address_keys() {
    let e = load(&device(ADDR, r#"{ tag = "A", web_id = "F1DP" }"#, "")).unwrap_err();
    assert!(e.contains("point 'p'") && e.contains("exactly one"), "{e}");
}

#[test]
fn relative_attribute_needs_af_database() {
    let e = load(&device(r#"{ base_url = "https://pi/piwebapi" }"#, r#"{ path = 'Pump|P' }"#, "")).unwrap_err();
    assert!(e.contains("af_database"), "{e}");
}

#[test]
fn inline_secrets_are_refused() {
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"auth = { method = "basic", username = "u", password = "secret" }"#)).unwrap_err();
    assert!(e.contains("auth.password") && e.contains("password_file"), "{e}");
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"auth = { method = "bearer", token = "t" }"#)).unwrap_err();
    assert!(e.contains("auth.token") && e.contains("token_file"), "{e}");
}

#[test]
fn auth_methods() {
    let d = load(&device(ADDR, r#"{ tag = "A" }"#, r#"auth = { method = "bearer", token_file = "/run/t" }"#)).unwrap();
    assert_eq!(d.settings.auth, Auth::Bearer { token_file: "/run/t".into() });
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"auth = { method = "certificate", cert_file = "/c" }"#)).unwrap_err();
    assert!(e.contains("key_file"), "{e}");
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"auth = { method = "kerberos" }"#)).unwrap_err();
    assert!(e.contains("kerberos"), "{e}");
}

/// Credentials are never sent over plain HTTP; a server without authentication may use it.
#[test]
fn credentials_need_https() {
    const HTTP: &str = r#"{ base_url = "http://pi/piwebapi", data_server = "S" }"#;
    for (method, auth) in [
        ("basic", r#"auth = { method = "basic", username = "u", password_file = "/p" }"#),
        ("bearer", r#"auth = { method = "bearer", token_file = "/t" }"#),
        ("certificate", r#"auth = { method = "certificate", cert_file = "/c", key_file = "/k" }"#),
    ] {
        let e = load(&device(HTTP, r#"{ tag = "A" }"#, auth)).unwrap_err();
        assert!(e.contains(&format!("auth method '{method}' needs https://")), "{e}");
    }
    load(&device(HTTP, r#"{ tag = "A" }"#, "")).unwrap();
}

#[test]
fn device_overrides_the_connection() {
    let address = r#"{ base_url = "http://pi/piwebapi", data_server = "S", delivery = "recorded", request_timeout = "2s", backfill = false }"#;
    let d = load(&device(address, r#"{ tag = "A" }"#, r#"request_timeout = "20s""#)).unwrap();
    assert_eq!(d.settings.request_timeout, std::time::Duration::from_secs(2));
    assert_eq!(d.settings.backfill, None);
    assert_eq!(d.delivery, Delivery::Recorded);
}

#[test]
fn unknown_keys_and_bad_values() {
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"colour = "red""#)).unwrap_err();
    assert!(e.contains("colour"), "{e}");
    let e = load(&device(r#"{ base_url = "https://pi", delivery = "carrier-pigeon" }"#, r#"{ web_id = "W" }"#, "")).unwrap_err();
    assert!(e.contains("delivery"), "{e}");
    let e = load(&device(r#"{ base_url = "ftp://pi" }"#, r#"{ web_id = "W" }"#, "")).unwrap_err();
    assert!(e.contains("base_url"), "{e}");
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"backfill = { page_size = 200000 }"#)).unwrap_err();
    assert!(e.contains("page_size"), "{e}");
    let e = load(&device(ADDR, r#"{ tag = "A" }"#, r#"request_timeout = "soon""#)).unwrap_err();
    assert!(e.contains("request_timeout"), "{e}");
}

#[test]
fn local_only_settings() {
    let c = connector_aveva_pi::factory();
    for key in ["password_file", "token_file", "cert_file", "key_file", "ca_file", "insecure_skip_verify", "state_dir"] {
        assert!(c.local_only_settings().contains(&key), "{key}");
    }
}

#[test]
fn capability_descriptor() {
    let caps = connector_aveva_pi::factory().capabilities();
    assert_eq!(caps.protocol, "aveva-pi");
    assert!(caps.subscribe);
    assert_eq!(caps.command_verbs, vec!["write".to_string()]);
}

/// What connectors/aveva-pi/tools/plugin-to-tedge-dot.py generates from the community plugin's
/// files (its pytest compares the script's output with this file), the connector loads.
#[test]
fn migration_script_output_loads() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../../connectors/aveva-pi/tools/testdata/plugin-expected.toml");
    let text = std::fs::read_to_string(path).expect("golden migration config");
    let d = load(&text).unwrap();
    assert_eq!(d.name, "pi-plant1");
    assert_eq!(d.delivery, Delivery::Channel);
    assert_eq!(
        d.settings.auth,
        Auth::Basic { username: "CORP\\pi-reader".into(), password_file: "/etc/tedge/plugins/ot/secrets/aveva-pi.password".into() }
    );
    assert_eq!(d.settings.tls.ca_file.as_deref(), Some(std::path::Path::new("/etc/tedge/plugins/ot/certs/pi-ca.pem")));
    let ids: Vec<_> = d.points.iter().map(|p| p.id.as_str()).collect();
    assert_eq!(ids, ["SINUSOID", "REACTOR01_TEMP", "TICKS", "COUNT16", "PUMP01_STATE", "BATCH_ID", "LAST_CHANGE", "NOPE"]);
    assert_eq!(d.points[1].address, Address::Point("\\\\PISRV01\\REACTOR01.TEMP".into()));
    assert!(d.points.iter().all(|p| p.datatype.is_some()));
    // A digital tag is its state code, a number, as the plugin's series was: a `string` point
    // would publish no measurement.
    assert_eq!(d.points[4].datatype, Some(tedge_dot_sdk::DataType::Int32));
}
