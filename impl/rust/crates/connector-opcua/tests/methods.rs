//! OPC UA method calls (openspec change `opcua-method-calls`) against an in-process `async-opcua`
//! server: the `call-method` verb converts the arguments, calls the method on its object and
//! returns the outputs; a bad method status is the command's reason; and both ids go through the
//! device's namespace table.

use std::sync::Arc;

use connector_opcua::OpcuaConnector;
use opcua::nodes::{MethodBuilder, ObjectBuilder};
use opcua::server::diagnostics::NamespaceMetadata;
use opcua::server::node_manager::memory::{simple_node_manager, SimpleNodeManager};
use opcua::server::{ServerBuilder, ServerHandle};
use opcua::types::{DataTypeId, NodeId, ObjectId, StatusCode, Variant};
use tedge_dot_sdk::{CommandRequest, Connector, ConnectorConfig, ConnectorError, LinkStatus};
use tokio::net::TcpListener;

const OPCUA_URI: &str = "http://opcfoundation.org/UA/";
const NAMESPACE_URI: &str = "urn:tedge-dot-opcua-test:nodes";

/// A server with a `Machine` object in `NAMESPACE_URI` and three methods on it:
/// `Multiply(Int32, Int32) -> Int32`, `Ping() -> String` and `Fail()`, which answers
/// `BadInvalidState`. Returns the index the server gave the namespace.
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
    let machine = NodeId::new(ns, "Machine");
    let id = |name: &str| NodeId::new(ns, format!("Machine.{name}"));
    {
        let mut space = nm.address_space().write();
        ObjectBuilder::new(&machine, "Machine", "Machine")
            .organized_by(ObjectId::ObjectsFolder)
            .insert(&mut *space);
        MethodBuilder::new(&id("Multiply"), "Multiply", "Multiply")
            .component_of(machine.clone())
            .executable(true)
            .user_executable(true)
            .input_args(
                &mut *space,
                &id("Multiply.InputArguments"),
                &[("A", DataTypeId::Int32).into(), ("B", DataTypeId::Int32).into()],
            )
            .output_args(&mut *space, &id("Multiply.OutputArguments"), &[("Product", DataTypeId::Int32).into()])
            .insert(&mut *space);
        MethodBuilder::new(&id("Ping"), "Ping", "Ping")
            .component_of(machine.clone())
            .executable(true)
            .user_executable(true)
            .output_args(&mut *space, &id("Ping.OutputArguments"), &[("Reply", DataTypeId::String).into()])
            .insert(&mut *space);
        MethodBuilder::new(&id("Fail"), "Fail", "Fail")
            .component_of(machine.clone())
            .executable(true)
            .user_executable(true)
            .insert(&mut *space);
    }
    nm.inner().add_method_callback(id("Multiply"), |args| match args {
        [Variant::Int32(a), Variant::Int32(b)] => Ok(vec![Variant::Int32(a * b)]),
        _ => Err(StatusCode::BadTypeMismatch),
    });
    nm.inner().add_method_callback(id("Ping"), |_| Ok(vec![Variant::from("pong")]));
    nm.inner().add_method_callback(id("Fail"), |_| Err(StatusCode::BadInvalidState));
    tokio::spawn(server.run_with(listener));
    (handle, nm, ns, port)
}

/// One device whose method points address the server's namespace as `local`, through a table
/// that lists it there.
fn config(port: u16, local: u16) -> ConnectorConfig {
    let mut table: Vec<String> = (0..=local).map(|i| format!("\"urn:filler:{i}\"")).collect();
    table[0] = format!("\"{OPCUA_URI}\"");
    table[local as usize] = format!("\"{NAMESPACE_URI}\"");
    let method = |id: &str, name: &str, inputs: &str| {
        format!(
            r#"
              [[device.point]]
              id = "{id}"
              access = "call_method"
              address = {{ object_id = "ns={local};s=Machine", method_id = "ns={local};s=Machine.{name}", inputs = [{inputs}] }}
            "#
        )
    };
    let text = format!(
        r#"
        [connector]
        protocol = "opcua"

        [[device]]
        name = "plc-1"
        protocol_address = {{ endpoint = "opc.tcp://127.0.0.1:{port}", namespaces = [{}] }}
        {}{}{}
        "#,
        table.join(", "),
        method("multiply", "Multiply", r#""int32", "int32""#),
        method("ping", "Ping", ""),
        method("fail", "Fail", ""),
    );
    toml::from_str(&text).unwrap()
}

fn call(point: &str, arguments: serde_json::Value) -> CommandRequest {
    CommandRequest { point: point.into(), value: Some(arguments), value_repr: None, raw: None }
}

async fn connected(cfg: &ConnectorConfig) -> OpcuaConnector {
    let mut connector = OpcuaConnector::default();
    connector.configure(cfg).unwrap();
    let reports = connector.connect().await.unwrap();
    assert_eq!(reports[0].status, LinkStatus::Connected, "{:?}", reports[0].reason);
    connector
}

fn reason(result: Result<tedge_dot_sdk::CommandResult, ConnectorError>) -> String {
    match result {
        Err(ConnectorError::Other(reason)) => reason,
        other => panic!("expected a failed call, got {other:?}"),
    }
}

#[tokio::test]
async fn methods_are_called_with_their_arguments_and_return_their_outputs() {
    let (_handle, _nm, ns, port) = start_server().await;
    let mut connector = connected(&config(port, ns)).await;
    let device = "plc-1".to_string();

    let result = connector
        .execute(&device, "call-method", &call("multiply", serde_json::json!([6, 7])))
        .await
        .unwrap();
    assert_eq!(result.point, "multiply");
    assert_eq!(result.value, Some(serde_json::json!([42])));

    let result = connector.execute(&device, "call-method", &call("ping", serde_json::json!([]))).await;
    assert_eq!(result.unwrap().value, Some(serde_json::json!(["pong"])));

    let result = connector.execute(&device, "call-method", &call("fail", serde_json::json!([]))).await;
    assert_eq!(reason(result), "call rejected: BadInvalidState");

    let result =
        connector.execute(&device, "call-method", &call("multiply", serde_json::json!([6]))).await;
    assert_eq!(reason(result), "expected 2 arguments, got 1");

    // A method point is not a value: never written, and a read explains why.
    let write = CommandRequest { point: "multiply".into(), value: Some(serde_json::json!(1)), value_repr: None, raw: None };
    assert!(matches!(
        connector.execute(&device, "write", &write).await,
        Err(ConnectorError::AccessDenied(_))
    ));
}

/// Spec "Namespace translation of method ids": the ids were written when the namespace sat one
/// index further back, and the call still reaches the method.
#[tokio::test]
async fn method_ids_follow_a_moved_namespace() {
    let (_handle, _nm, ns, port) = start_server().await;
    let mut connector = connected(&config(port, ns + 1)).await;
    let result = connector
        .execute(&"plc-1".to_string(), "call-method", &call("multiply", serde_json::json!([3, 5])))
        .await
        .unwrap();
    assert_eq!(result.value, Some(serde_json::json!([15])));
}

#[tokio::test]
async fn a_method_on_a_device_without_a_session_fails_without_a_call() {
    let (_handle, _nm, ns, port) = start_server().await;
    let mut connector = OpcuaConnector::default();
    connector.configure(&config(port, ns)).unwrap();
    let result = connector
        .execute(&"plc-1".to_string(), "call-method", &call("ping", serde_json::json!([])))
        .await;
    assert_eq!(reason(result), "device plc-1 is not connected");
}
