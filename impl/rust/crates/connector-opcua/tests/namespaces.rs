//! The local namespace table (openspec change `opcua-namespace-table`) against an in-process
//! `async-opcua` server: points written against one namespace order read, write and subscribe the
//! right nodes on a server that numbers its namespaces differently, a namespace missing on the
//! server fails only its own points, and a point library's table applies to the library's points.

mod common;

use std::sync::Arc;
use std::time::Duration;

use connector_opcua::OpcuaConnector;
use opcua::nodes::VariableBuilder;
use opcua::server::diagnostics::NamespaceMetadata;
use opcua::server::node_manager::memory::{simple_node_manager, SimpleNodeManager};
use opcua::server::{ServerBuilder, ServerHandle};
use opcua::types::{DataTypeId, NodeId, ObjectId};
use tedge_dot_sdk::{
    library, Access, CommandRequest, Connector, ConnectorConfig, DataType, Endianness, LinkStatus,
    Mode, PointRef, Quality, Sample, Value, WordOrder,
};
use tokio::net::TcpListener;
use tokio::sync::mpsc;

const OPCUA_URI: &str = "http://opcfoundation.org/UA/";
const NAMESPACE_URI: &str = "urn:tedge-dot-opcua-test:nodes";

/// A server with `Temperature` (Double, 21.5) and `Counter` (UInt16, 0) in `NAMESPACE_URI`.
/// Returns the index the server gave that namespace.
async fn start_server() -> (ServerHandle, Arc<SimpleNodeManager>, u16, u16) {
    let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
    let port = listener.local_addr().unwrap().port();
    let (server, handle) = ServerBuilder::new_anonymous("tedge-dot-test")
        .application_uri("urn:tedge-dot-opcua-test")
        .product_uri("urn:tedge-dot-opcua-test")
        .host("127.0.0.1")
        .port(port)
        .discovery_urls(vec![format!("opc.tcp://127.0.0.1:{port}")])
        .with_node_manager(simple_node_manager(
            NamespaceMetadata { namespace_uri: NAMESPACE_URI.to_owned(), ..Default::default() },
            "simple",
        ))
        .build()
        .unwrap();
    let nm = handle.node_managers().get_of_type::<SimpleNodeManager>().unwrap();
    let ns = handle.get_namespace_index(NAMESPACE_URI).unwrap();
    {
        let mut space = nm.address_space().write();
        VariableBuilder::new(&NodeId::new(ns, "Temperature"), "Temperature", "Temperature")
            .data_type(DataTypeId::Double)
            .value(21.5f64)
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        VariableBuilder::new(&NodeId::new(ns, "Counter"), "Counter", "Counter")
            .data_type(DataTypeId::UInt16)
            .value(0u16)
            .writable()
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
    }
    tokio::spawn(server.run_with(listener));
    (handle, nm, ns, port)
}

/// A namespace table, as TOML, that lists `NAMESPACE_URI` at `at` (other entries are fillers).
fn table(at: u16, extra: &[(u16, &str)]) -> String {
    let len = extra.iter().map(|(i, _)| *i).chain([at]).max().unwrap() + 1;
    let entries: Vec<String> = (0..len)
        .map(|i| match i {
            0 => OPCUA_URI.to_string(),
            i if i == at => NAMESPACE_URI.to_string(),
            i => extra
                .iter()
                .find(|(j, _)| *j == i)
                .map_or_else(|| format!("urn:filler:{i}"), |(_, uri)| uri.to_string()),
        })
        .map(|uri| format!("\"{uri}\""))
        .collect();
    format!("[{}]", entries.join(", "))
}

fn config(port: u16, namespaces: &str, points: &str) -> ConnectorConfig {
    let text = format!(
        r#"
        [connector]
        protocol = "opcua"

        [[device]]
        name = "plc-1"
        protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:{port}", namespaces = {namespaces} }}
        default_mode = "typed"
        {points}
        "#
    );
    toml::from_str(&text).unwrap()
}

fn point(id: &str, node_id: &str, datatype: &str) -> String {
    format!(
        r#"
          [[device.point]]
          id = "{id}"
          datatype = "{datatype}"
          access = "read_write"
          address = {{ node_id = "{node_id}" }}
        "#
    )
}

fn pref(id: &str, datatype: DataType) -> PointRef {
    PointRef {
        id: id.to_string(),
        mode: Mode::Typed,
        datatype: Some(datatype),
        endianness: Endianness::Big,
        word_order: WordOrder::Big,
        access: Access::Read,
        unit: None,
        transform: Default::default(),
        interval: Some(Duration::from_millis(100)),
    }
}

async fn read(connector: &mut OpcuaConnector, id: &str, datatype: DataType) -> Sample {
    let mut samples = connector.read_points(&"plc-1".to_string(), &[pref(id, datatype)]).await.unwrap();
    samples.remove(0)
}

#[tokio::test]
async fn ids_written_against_another_namespace_order_read_the_right_node() {
    let (handle, _nm, ns, port) = start_server().await;
    // The ids were written when the namespace sat one index further back.
    let local = ns + 1;
    let cfg = config(port, &table(local, &[]), &point("temperature", &format!("ns={local};s=Temperature"), "float64"));
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    let reports = connector.connect().await.unwrap();
    assert_eq!(reports[0].status, LinkStatus::Connected);

    let sample = read(&mut connector, "temperature", DataType::Float64).await;
    assert_eq!(sample.quality, Quality::Good, "{:?}", sample.error);
    assert_eq!(sample.value, Some(Value::Number(21.5)));
    // The sample shows the address as configured, not the translated index.
    assert_eq!(sample.addr["node_id"], serde_json::json!(format!("ns={local};s=Temperature")));

    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[tokio::test]
async fn the_same_order_reads_the_configured_ids() {
    let (handle, _nm, ns, port) = start_server().await;
    let cfg = config(port, &table(ns, &[]), &point("temperature", &format!("ns={ns};s=Temperature"), "float64"));
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    connector.connect().await.unwrap();
    let sample = read(&mut connector, "temperature", DataType::Float64).await;
    assert_eq!(sample.value, Some(Value::Number(21.5)));
    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[tokio::test]
async fn a_namespace_missing_on_the_server_fails_only_its_points() {
    let (handle, _nm, ns, port) = start_server().await;
    let missing = ns + 1;
    let points = point("temperature", &format!("ns={ns};s=Temperature"), "float64")
        + &point("ghost", &format!("ns={missing};s=Temperature"), "float64");
    let cfg = config(port, &table(ns, &[(missing, "urn:not-on-the-server")]), &points);
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    let reports = connector.connect().await.unwrap();
    assert_eq!(reports[0].status, LinkStatus::Connected, "a missing namespace must not fail the device");

    let samples = connector
        .read_points(
            &"plc-1".to_string(),
            &[pref("temperature", DataType::Float64), pref("ghost", DataType::Float64)],
        )
        .await
        .unwrap();
    let by_id = |id: &str| samples.iter().find(|s| s.point == id).unwrap();
    assert_eq!(by_id("temperature").quality, Quality::Good);
    assert_eq!(by_id("ghost").quality, Quality::Bad);
    assert_eq!(by_id("ghost").error.as_deref(), Some("namespace not found: urn:not-on-the-server"));

    // A write to the untranslated point is refused with the same reason.
    let request = CommandRequest {
        point: "ghost".into(),
        value: Some(serde_json::json!(1.0)),
        value_repr: None,
        raw: None,
    };
    let err = connector.execute(&"plc-1".to_string(), "write", &request).await.unwrap_err();
    assert!(err.to_string().contains("namespace not found: urn:not-on-the-server"), "{err}");

    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[tokio::test]
async fn a_write_goes_to_the_translated_node() {
    let (handle, nm, ns, port) = start_server().await;
    let local = ns + 2;
    let cfg = config(port, &table(local, &[]), &point("counter", &format!("ns={local};s=Counter"), "uint16"));
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    connector.connect().await.unwrap();

    let request = CommandRequest {
        point: "counter".into(),
        value: Some(serde_json::json!(7)),
        value_repr: None,
        raw: None,
    };
    connector.execute(&"plc-1".to_string(), "write", &request).await.unwrap();
    let written = {
        let space = nm.address_space().read();
        let node = space.find(&NodeId::new(ns, "Counter")).unwrap();
        node.as_node().get_attribute(
            opcua::types::TimestampsToReturn::Neither,
            opcua::types::AttributeId::Value,
            &opcua::types::NumericRange::None,
            &opcua::types::DataEncoding::Binary,
        )
    };
    assert_eq!(written.and_then(|dv| dv.value), Some(opcua::types::Variant::UInt16(7)));

    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[tokio::test]
async fn subscribed_points_use_the_translation_and_untranslated_ones_report_why() {
    let (handle, nm, ns, port) = start_server().await;
    let local = ns + 1;
    let missing = ns + 3;
    let points = point("temperature", &format!("ns={local};s=Temperature"), "float64")
        + &point("ghost", &format!("ns={missing};s=Temperature"), "float64");
    let cfg = config(port, &table(local, &[(missing, "urn:not-on-the-server")]), &points);
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    connector.connect().await.unwrap();

    let (tx, mut rx) = mpsc::channel::<Sample>(64);
    connector
        .subscribe(
            &"plc-1".to_string(),
            &[pref("temperature", DataType::Float64), pref("ghost", DataType::Float64)],
            tx,
        )
        .await
        .expect("an untranslated point must not fail the subscription");

    nm.set_value(
        handle.subscriptions(),
        &NodeId::new(ns, "Temperature"),
        None,
        opcua::types::DataValue::new_now(42.0f64),
    )
    .unwrap();

    let mut ghost = None;
    let mut pushed = None;
    tokio::time::timeout(Duration::from_secs(10), async {
        while ghost.is_none() || pushed.is_none() {
            let sample = rx.recv().await.expect("sample channel closed");
            match sample.point.as_str() {
                "ghost" => ghost = Some(sample),
                "temperature" if sample.value == Some(Value::Number(42.0)) => pushed = Some(sample),
                _ => {}
            }
        }
    })
    .await
    .expect("timed out waiting for the pushed and the bad sample");
    let ghost = ghost.unwrap();
    assert_eq!(ghost.quality, Quality::Bad);
    assert_eq!(ghost.device, "plc-1");
    assert_eq!(ghost.error.as_deref(), Some("namespace not found: urn:not-on-the-server"));
    assert_eq!(pushed.unwrap().addr["node_id"], serde_json::json!(format!("ns={local};s=Temperature")));
    connector.check_subscription(&"plc-1".to_string()).await.unwrap();

    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[tokio::test]
async fn a_library_table_applies_to_the_library_points() {
    let (handle, _nm, ns, port) = start_server().await;
    let dir = common::tempdir("ns-library");
    std::fs::create_dir_all(dir.join("opcua")).unwrap();
    // The library was written against a server with the namespace at ns + 2, the device's own
    // points against one with it at ns + 1.
    let lib_at = ns + 2;
    let dev_at = ns + 1;
    std::fs::write(
        dir.join("opcua/acme.toml"),
        format!(
            r#"
[library]
protocol = "opcua"
namespaces = {}

[[point]]
id = "lib_temperature"
datatype = "float64"
address = {{ node_id = "ns={lib_at};s=Temperature" }}

[[point]]
id = "overridden"
datatype = "float64"
address = {{ node_id = "ns={lib_at};s=Counter" }}
"#,
            table(lib_at, &[])
        ),
    )
    .unwrap();
    let text = format!(
        r#"
[connector]
protocol = "opcua"
point_library_path = ["{dir}"]

[[device]]
name = "plc-1"
protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:{port}", namespaces = {dev_table} }}
default_mode = "typed"
points_from = ["acme"]

  [[device.point]]
  id = "dev_temperature"
  datatype = "float64"
  address = {{ node_id = "ns={dev_at};s=Temperature" }}

  # The device's own address: translated through the device's table, not the library's.
  [[device.point]]
  id = "overridden"
  address = {{ node_id = "ns={dev_at};s=Temperature" }}
"#,
        dir = dir.display(),
        dev_table = table(dev_at, &[]),
    );
    let cfg = library::resolve(&text, &dir).unwrap();
    let mut connector = OpcuaConnector::default();
    connector.configure(&cfg).unwrap();
    connector.connect().await.unwrap();
    for id in ["lib_temperature", "dev_temperature", "overridden"] {
        let sample = read(&mut connector, id, DataType::Float64).await;
        assert_eq!(sample.value, Some(Value::Number(21.5)), "{id}: {:?}", sample.error);
    }
    connector.disconnect().await.unwrap();
    handle.cancel();
}

#[test]
fn table_errors_are_configuration_errors() {
    let configure = |namespaces: &str, node_id: &str| {
        let cfg = config(4840, namespaces, &point("p", node_id, "float64"));
        OpcuaConnector::default().configure(&cfg).map(|_| ()).unwrap_err().to_string()
    };
    let err = configure("[\"urn:acme:plc\"]", "ns=1;i=1");
    assert!(err.contains("namespaces entry 0 must be 'http://opcfoundation.org/UA/', found 'urn:acme:plc'"), "{err}");
    let err = configure("\"urn:acme:plc\"", "ns=1;i=1");
    assert!(err.contains("namespaces must be an array of namespace URI strings"), "{err}");
    let err = configure(&table(1, &[]), "ns=5;i=1");
    assert!(err.contains("point 'p': namespace index 5 is not in the namespace table (2 entries)"), "{err}");
    // Namespace 0 is never translated, so it needs no entry beyond 0.
    let cfg = config(4840, &format!("[\"{OPCUA_URI}\"]"), &point("p", "i=2258", "string"));
    OpcuaConnector::default().configure(&cfg).unwrap();
}
