//! Structured values against a real server (openspec change `opcua-custom-datatypes`): an
//! in-process `async-opcua` server publishes custom structure types with their
//! `DataTypeDefinition`, and the connector resolves them per session and decodes single fields,
//! array elements, raw bodies and the additional built-in types. The binary decoding itself is
//! pinned by the shared vectors (`src/structure.rs`); this covers the session side: reading the
//! definitions, nested and aliased field types, shared reads, subscriptions and the errors.

use std::sync::Arc;
use std::time::Duration;

use connector_opcua::OpcuaConnector;
use opcua::nodes::{DataTypeBuilder, ObjectBuilder, ReferenceDirection, VariableBuilder};
use opcua::server::diagnostics::NamespaceMetadata;
use opcua::server::node_manager::memory::{simple_node_manager, SimpleNodeManager};
use opcua::server::{ServerBuilder, ServerHandle};
use opcua::types::{
    ByteString, ByteStringBody, DataTypeDefinition, DataTypeId, DataValue, DateTime,
    ExtensionObject, LocalizedText, NodeId, ObjectId, ObjectTypeId, ReferenceTypeId,
    StructureDefinition, StructureField, StructureType, Variant,
};
use tedge_dot_sdk::{
    Access, Connector, ConnectorConfig, DataType, Endianness, Mode, PointRef, Quality, Sample,
    Value, WordOrder,
};
use tokio::net::TcpListener;
use tokio::sync::mpsc;

const NAMESPACE_URI: &str = "urn:tedge-dot-opcua-test:structures";

/// The test structures' node ids within the test namespace.
const MOTOR: u32 = 3001;
const MOTOR_ENCODING: u32 = 3002;
const PUMP: u32 = 3003;
const PUMP_ENCODING: u32 = 3004;
const NO_DEFINITION: u32 = 3005;

fn field(name: &str, data_type: NodeId, value_rank: i32) -> StructureField {
    StructureField {
        name: name.into(),
        data_type,
        value_rank,
        ..Default::default()
    }
}

/// `PumpStatus { Running: Boolean, Speed: Double, Motor: Motor { Current: Float, Temp: Float },
/// Label: String, Stamp: UtcTime, Samples: Double[] }`, binary encoded.
fn pump_body(speed: f64, current: f32, label: &str) -> Vec<u8> {
    let mut b = vec![1u8];
    b.extend(speed.to_le_bytes());
    b.extend(current.to_le_bytes());
    b.extend(41.5f32.to_le_bytes());
    b.extend((label.len() as i32).to_le_bytes());
    b.extend(label.as_bytes());
    // 2026-10-06T08:15:30.25Z as 100 ns ticks since 1601
    b.extend(134_357_481_302_500_000i64.to_le_bytes());
    b.extend(3i32.to_le_bytes());
    for s in [1.0f64, 2.0, 4.5] {
        b.extend(s.to_le_bytes());
    }
    b
}

fn pump_value(ns: u16, speed: f64, current: f32, label: &str) -> Variant {
    let body = ByteStringBody::new(ByteString::from(pump_body(speed, current, label)), NodeId::new(ns, PUMP_ENCODING));
    Variant::ExtensionObject(ExtensionObject::new(body))
}

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
        let motor = StructureDefinition {
            default_encoding_id: NodeId::new(ns, MOTOR_ENCODING),
            base_data_type: DataTypeId::Structure.into(),
            structure_type: StructureType::Structure,
            fields: Some(vec![
                field("Current", DataTypeId::Float.into(), -1),
                field("Temp", DataTypeId::Float.into(), -1),
            ]),
        };
        DataTypeBuilder::new(&NodeId::new(ns, MOTOR), "Motor", "Motor")
            .subtype_of(DataTypeId::Structure)
            .data_type_definition(DataTypeDefinition::Structure(motor))
            .insert(&mut *space);
        let pump = StructureDefinition {
            default_encoding_id: NodeId::new(ns, PUMP_ENCODING),
            base_data_type: DataTypeId::Structure.into(),
            structure_type: StructureType::Structure,
            fields: Some(vec![
                field("Running", DataTypeId::Boolean.into(), -1),
                field("Speed", DataTypeId::Double.into(), -1),
                field("Motor", NodeId::new(ns, MOTOR), -1),
                field("Label", DataTypeId::String.into(), -1),
                // UtcTime: a simple subtype of DateTime, resolved through the alias table.
                field("Stamp", DataTypeId::UtcTime.into(), -1),
                field("Samples", DataTypeId::Double.into(), 1),
            ]),
        };
        DataTypeBuilder::new(&NodeId::new(ns, PUMP), "PumpStatus", "PumpStatus")
            .subtype_of(DataTypeId::Structure)
            .data_type_definition(DataTypeDefinition::Structure(pump))
            .insert(&mut *space);
        // The encoding node and its HasEncoding reference, which a server publishes for every
        // structure type: how a value's encoding id leads back to its DataType.
        ObjectBuilder::new(&NodeId::new(ns, PUMP_ENCODING), "Default Binary", "Default Binary")
            .has_type_definition(ObjectTypeId::DataTypeEncodingType)
            .reference(NodeId::new(ns, PUMP), ReferenceTypeId::HasEncoding, ReferenceDirection::Inverse)
            .insert(&mut *space);
        DataTypeBuilder::new(&NodeId::new(ns, NO_DEFINITION), "Opaque", "Opaque")
            .subtype_of(DataTypeId::Structure)
            .insert(&mut *space);

        VariableBuilder::new(&NodeId::new(ns, "Pump"), "Pump", "Pump")
            .data_type(NodeId::new(ns, PUMP))
            .value(pump_value(ns, 1450.0, 3.25, "P1"))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        // Declared with the abstract Structure, as many servers do: the concrete type is known
        // only from the value's encoding.
        VariableBuilder::new(&NodeId::new(ns, "AnyPump"), "AnyPump", "AnyPump")
            .data_type(DataTypeId::Structure)
            .value(pump_value(ns, 1450.0, 3.25, "P1"))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        let opaque = ByteStringBody::new(ByteString::from(vec![1u8, 2, 3]), NodeId::new(ns, 3006));
        VariableBuilder::new(&NodeId::new(ns, "Opaque"), "Opaque", "Opaque")
            .data_type(NodeId::new(ns, NO_DEFINITION))
            .value(Variant::ExtensionObject(ExtensionObject::new(opaque)))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        VariableBuilder::new(&NodeId::new(ns, "Temperatures"), "Temperatures", "Temperatures")
            .data_type(DataTypeId::Double)
            .value_rank(1)
            .value(vec![20.0f64, 21.0, 22.5, 23.0])
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        VariableBuilder::new(&NodeId::new(ns, "Serviced"), "Serviced", "Serviced")
            .data_type(DataTypeId::DateTime)
            .value(DateTime::from(134_357_481_302_500_000i64))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        VariableBuilder::new(&NodeId::new(ns, "Blob"), "Blob", "Blob")
            .data_type(DataTypeId::ByteString)
            .value(ByteString::from(vec![0xdeu8, 0xad, 0xbe, 0xef]))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        VariableBuilder::new(&NodeId::new(ns, "State"), "State", "State")
            .data_type(DataTypeId::LocalizedText)
            .value(LocalizedText::new("de-DE", "Betrieb"))
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
    }
    tokio::spawn(server.run_with(listener));
    (handle, nm, ns, port)
}

fn config(port: u16, ns: u16) -> ConnectorConfig {
    let toml = format!(
        r#"
        [connector]
        protocol = "opcua"

        [[device]]
        name = "plc-1"
        protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:{port}" }}
        default_mode = "typed"

          [[device.point]]
          id = "speed"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Pump", field = "Speed" }}

          [[device.point]]
          id = "current"
          datatype = "float32"
          address = {{ node_id = "ns={ns};s=Pump", field = "Motor.Current" }}

          [[device.point]]
          id = "label"
          datatype = "string"
          address = {{ node_id = "ns={ns};s=Pump", field = "Label" }}

          [[device.point]]
          id = "stamp"
          datatype = "string"
          address = {{ node_id = "ns={ns};s=Pump", field = "Stamp" }}

          [[device.point]]
          id = "sample_2"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Pump", field = "Samples[2]" }}

          [[device.point]]
          id = "speed_wrong_type"
          datatype = "int32"
          address = {{ node_id = "ns={ns};s=Pump", field = "Speed" }}

          [[device.point]]
          id = "pump_raw"
          mode = "raw"
          address = {{ node_id = "ns={ns};s=Pump" }}

          [[device.point]]
          id = "pump_typed_whole"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Pump" }}

          [[device.point]]
          id = "opaque_field"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Opaque", field = "X" }}

          [[device.point]]
          id = "opaque_raw"
          mode = "raw"
          address = {{ node_id = "ns={ns};s=Opaque" }}

          [[device.point]]
          id = "temperature_2"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Temperatures", index = 2 }}

          [[device.point]]
          id = "temperature_9"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Temperatures", index = 9 }}

          [[device.point]]
          id = "temperatures_whole"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=Temperatures" }}

          [[device.point]]
          id = "serviced"
          datatype = "string"
          address = {{ node_id = "ns={ns};s=Serviced" }}

          [[device.point]]
          id = "serviced_ms"
          datatype = "int64"
          address = {{ node_id = "ns={ns};s=Serviced" }}

          [[device.point]]
          id = "state"
          datatype = "string"
          address = {{ node_id = "ns={ns};s=State" }}

          [[device.point]]
          id = "any_speed"
          datatype = "float64"
          address = {{ node_id = "ns={ns};s=AnyPump", field = "Speed" }}

          [[device.point]]
          id = "blob"
          datatype = "bytes"
          address = {{ node_id = "ns={ns};s=Blob" }}
        "#
    );
    toml::from_str(&toml).unwrap()
}

fn pref(id: &str) -> PointRef {
    PointRef {
        id: id.to_string(),
        mode: Mode::Typed,
        datatype: None,
        endianness: Endianness::Big,
        word_order: WordOrder::Big,
        access: Access::Read,
        unit: None,
        transform: Default::default(),
        interval: Some(Duration::from_millis(100)),
    }
}

async fn connected(port: u16, ns: u16) -> OpcuaConnector {
    let mut connector = OpcuaConnector::default();
    connector.configure(&config(port, ns)).unwrap();
    let reports = connector.connect().await.unwrap();
    assert_eq!(reports[0].status.as_str(), "connected", "{:?}", reports[0].reason);
    connector
}

fn by_point(samples: Vec<Sample>) -> std::collections::HashMap<String, Sample> {
    samples.into_iter().map(|s| (s.point.clone(), s)).collect()
}

fn good(s: &Sample) -> &Value {
    assert_eq!(s.quality, Quality::Good, "{}: {:?}", s.point, s.error);
    s.value.as_ref().unwrap()
}

#[tokio::test]
async fn polled_fields_elements_and_builtins() {
    let (_handle, _nm, ns, port) = start_server().await;
    let mut connector = connected(port, ns).await;
    let device = "plc-1".to_string();
    let ids = [
        "speed", "current", "label", "stamp", "sample_2", "speed_wrong_type", "pump_raw",
        "pump_typed_whole", "opaque_field", "opaque_raw", "temperature_2", "temperature_9",
        "temperatures_whole", "serviced", "serviced_ms", "state", "blob", "any_speed",
    ];
    let refs: Vec<PointRef> = ids.iter().map(|id| pref(id)).collect();
    let s = by_point(connector.read_points(&device, &refs).await.unwrap());
    assert_eq!(s.len(), ids.len());

    // Fields of one structure, including a nested one, an alias type and an array element.
    assert_eq!(good(&s["speed"]), &Value::Number(1450.0));
    assert_eq!(s["speed"].addr["field"], "Speed");
    assert_eq!(good(&s["current"]), &Value::Number(3.25));
    assert_eq!(good(&s["label"]), &Value::Text("P1".into()));
    assert_eq!(good(&s["stamp"]), &Value::Text("2026-10-06T08:15:30.25Z".into()));
    assert_eq!(good(&s["sample_2"]), &Value::Number(4.5));

    // The declared type is checked against the definition, per point.
    assert_eq!(s["speed_wrong_type"].quality, Quality::Bad);
    assert_eq!(
        s["speed_wrong_type"].error.as_deref(),
        Some("field \"Speed\" is Double, point declares int32 (accepted: float64)")
    );

    // Raw mode: the body, with the type identified by namespace URI.
    let raw = &s["pump_raw"];
    assert_eq!(raw.quality, Quality::Good, "{:?}", raw.error);
    assert_eq!(raw.raw, pump_body(1450.0, 3.25, "P1"));
    assert_eq!(raw.addr["encoding_id"], format!("nsu={NAMESPACE_URI};i={PUMP_ENCODING}"));
    assert_eq!(raw.addr["data_type"], format!("nsu={NAMESPACE_URI};i={PUMP}"));
    assert_eq!(s["pump_typed_whole"].quality, Quality::Bad);
    assert_eq!(
        s["pump_typed_whole"].error.as_deref(),
        Some("value is a structure; select one of its fields with address.field")
    );

    // A type without a definition: field points report it, raw still works.
    let opaque = &s["opaque_field"];
    assert_eq!(opaque.quality, Quality::Bad);
    assert!(
        opaque.error.as_deref().unwrap().starts_with("data type definition unavailable"),
        "{:?}",
        opaque.error
    );
    assert_eq!(s["opaque_raw"].quality, Quality::Good, "{:?}", s["opaque_raw"].error);
    assert_eq!(s["opaque_raw"].raw, vec![1, 2, 3]);

    // Array elements, selected on the server.
    assert_eq!(good(&s["temperature_2"]), &Value::Number(22.5));
    assert_eq!(s["temperature_2"].addr["index"], 2);
    assert_eq!(s["temperature_9"].quality, Quality::Bad);
    assert_eq!(s["temperatures_whole"].quality, Quality::Bad);
    assert_eq!(
        s["temperatures_whole"].error.as_deref(),
        Some("value is an array; select one element with address.index")
    );

    // Built-in types rendered into SDK datatypes.
    assert_eq!(good(&s["serviced"]), &Value::Text("2026-10-06T08:15:30.25Z".into()));
    assert_eq!(good(&s["serviced_ms"]), &Value::Number(1_791_274_530_250.0));
    assert_eq!(s["serviced_ms"].datatype, Some(DataType::Int64));
    assert_eq!(good(&s["state"]), &Value::Text("Betrieb".into()));
    assert_eq!(good(&s["blob"]), &Value::Text("deadbeef".into()));
    assert_eq!(s["blob"].datatype, Some(DataType::Bytes));
    assert_eq!(s["blob"].raw, vec![0xde, 0xad, 0xbe, 0xef]);

    // A variable declared with the abstract Structure: resolved through its value's encoding.
    assert_eq!(good(&s["any_speed"]), &Value::Number(1450.0));
}

/// Receive samples until every expectation has matched one, in any order: the server batches
/// the initial values and changes of several monitored items in one notification.
async fn expect_all(rx: &mut mpsc::Receiver<Sample>, mut expected: Vec<(&str, Value)>) {
    let wanted = format!("{expected:?}");
    tokio::time::timeout(Duration::from_secs(10), async {
        while !expected.is_empty() {
            let s = rx.recv().await.expect("sample channel closed");
            expected.retain(|(point, value)| !(s.point == *point && s.value.as_ref() == Some(value)));
        }
    })
    .await
    .unwrap_or_else(|_| panic!("timed out waiting for {wanted}, still missing {expected:?}"))
}

#[tokio::test]
async fn subscribed_fields_share_one_monitored_item() {
    let (handle, nm, ns, port) = start_server().await;
    let mut connector = connected(port, ns).await;
    let device = "plc-1".to_string();
    let (tx, mut rx) = mpsc::channel::<Sample>(64);
    connector
        // Array elements are left out: the async-opcua 0.18 server applies a monitored item's
        // IndexRange twice (doc/upstream/async-opcua-index-range-applied-twice.md). The e2e
        // suite covers pushed elements against the asyncua simulator.
        .subscribe(&device, &[pref("speed"), pref("current")], tx)
        .await
        .unwrap();

    expect_all(
        &mut rx,
        vec![
            ("speed", Value::Number(1450.0)),
            ("current", Value::Number(3.25)),
        ],
    )
    .await;

    // One server-side change of the structure updates both of its field points.
    tokio::time::sleep(Duration::from_millis(150)).await;
    nm.set_value(
        handle.subscriptions(),
        &NodeId::new(ns, "Pump"),
        None,
        DataValue::new_now(pump_value(ns, 900.0, 5.5, "P1")),
    )
    .unwrap();
    expect_all(&mut rx, vec![("speed", Value::Number(900.0)), ("current", Value::Number(5.5))]).await;
}

#[tokio::test]
async fn field_points_are_validated_at_configure() {
    let mut connector = OpcuaConnector::default();
    let base = r#"
        [connector]
        protocol = "opcua"
        [[device]]
        name = "plc-1"
        protocol_address = { endpoint = "opc.tcp://127.0.0.1:4840" }
          [[device.point]]
          id = "p"
          datatype = "float64"
    "#;
    for (extra, want) in [
        (r#"address = { node_id = "ns=2;s=X", field = "A..B" }"#, "has an empty segment"),
        (r#"address = { node_id = "ns=2;s=X", field = "A[x]" }"#, "has an invalid index"),
        (r#"address = { node_id = "ns=2;s=X", field = "A" }
            mode = "raw""#, "a raw point reads the whole structure body"),
        (r#"address = { node_id = "ns=2;s=X", field = "A" }
            access = "read_write""#, "are read-only"),
        (r#"address = { node_id = "ns=2;s=X", index = 1 }
            access = "write""#, "are read-only"),
    ] {
        let config: ConnectorConfig = toml::from_str(&format!("{base}\n{extra}")).unwrap();
        let err = connector.configure(&config).unwrap_err().to_string();
        assert!(err.contains(want), "{extra}: {err}");
    }
}

/// Two points subscribed to the same static node share one monitored item: each gets the
/// initial value once, and nothing more while the node does not change.
#[tokio::test]
async fn points_sharing_a_static_node_are_notified_once() {
    let (_handle, _nm, ns, port) = start_server().await;
    let mut connector = connected(port, ns).await;
    let (tx, mut rx) = mpsc::channel::<Sample>(64);
    connector.subscribe(&"plc-1".to_string(), &[pref("speed"), pref("label")], tx).await.unwrap();
    let mut seen: Vec<String> = Vec::new();
    let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
    while let Ok(Some(s)) = tokio::time::timeout_at(deadline, rx.recv()).await {
        seen.push(s.point);
    }
    seen.sort();
    assert_eq!(seen, vec!["label".to_string(), "speed".to_string()], "{seen:?}");
    // The push health check counts monitored items, not points: two points on one node are
    // one healthy item, not a lost one.
    connector.check_subscription(&"plc-1".to_string()).await.unwrap();
}
