//! A standalone OPC UA server for throughput measurements (`ot-conformance serve opcua`).
//!
//! It serves `points` `Double` variables `ns=2;s=Bulk0` … `ns=2;s=Bulk<N-1>`, anonymous with
//! security `None`, each starting at its own index. With a change rate, a task changes that
//! many variables per second, round robin and spread evenly, adding 1 to each, so pushed
//! (monitored) points see real data changes. Unlike the conformance simulator there is no
//! proxy, no ground truth and no outage switch: the server advertises the endpoint it listens
//! on, and nothing sits between it and the connector.

use opcua::nodes::VariableBuilder;
use opcua::server::diagnostics::NamespaceMetadata;
use opcua::server::node_manager::memory::{simple_node_manager, SimpleNodeManager};
use opcua::server::{ServerBuilder, ServerHandle};
use opcua::types::{DataTypeId, DataValue, DateTime, NodeId, ObjectId, Variant};
use std::time::Duration;

const NAMESPACE_URI: &str = "urn:tedge-dot-bench:opcua";
const NAMESPACE_INDEX: u16 = 2;
/// How often the change task wakes up; the changes owed since the last wake-up are applied
/// together.
const CHANGE_TICK: Duration = Duration::from_millis(10);

pub struct BulkServer {
    pub endpoint: String,
    handle: ServerHandle,
    pki: std::path::PathBuf,
}

impl Drop for BulkServer {
    fn drop(&mut self) {
        self.handle.cancel();
        let _ = std::fs::remove_dir_all(&self.pki);
    }
}

impl BulkServer {
    /// Start the server on `host:port` (port 0 picks a free one) and return once it accepts
    /// connections.
    pub async fn start(points: usize, change_rate: f64, host: &str, port: u16) -> Result<BulkServer, String> {
        let listener = tokio::net::TcpListener::bind((host, port))
            .await
            .map_err(|e| format!("cannot listen on {host}:{port}: {e}"))?;
        let port = listener.local_addr().map_err(|e| format!("local_addr: {e}"))?.port();
        let endpoint = format!("opc.tcp://{host}:{port}");

        // Only security None is offered, but the server still wants an application
        // certificate: generate one in a per-process directory instead of reading ./pki.
        let pki = std::env::temp_dir().join(format!("tedge-dot-bench-opcua-{}", std::process::id()));
        let mut builder = ServerBuilder::new_anonymous("tedge-dot-bench")
            .pki_dir(&pki)
            .create_sample_keypair(true)
            .application_uri("urn:tedge-dot-bench")
            .product_uri("urn:tedge-dot-bench")
            .host(host)
            .port(port)
            .discovery_urls(vec![endpoint.clone()])
            .with_node_manager(simple_node_manager(
                NamespaceMetadata {
                    namespace_uri: NAMESPACE_URI.to_owned(),
                    ..Default::default()
                },
                "bench",
            ));
        // A throughput run reads and monitors tens of thousands of nodes in one session.
        let limits = builder.limits_mut();
        limits.operational.max_nodes_per_read = limits.operational.max_nodes_per_read.max(points);
        limits.subscriptions.max_monitored_items_per_sub =
            limits.subscriptions.max_monitored_items_per_sub.max(points);
        let (server, handle) = builder.build().map_err(|e| format!("server build failed: {e}"))?;

        let nm = handle
            .node_managers()
            .get_of_type::<SimpleNodeManager>()
            .ok_or("no SimpleNodeManager")?;
        let ns = handle
            .get_namespace_index(NAMESPACE_URI)
            .ok_or("namespace not registered")?;
        if ns != NAMESPACE_INDEX {
            return Err(format!("namespace landed on index {ns}, expected {NAMESPACE_INDEX}"));
        }
        let nodes: Vec<NodeId> = (0..points).map(|i| NodeId::new(ns, format!("Bulk{i}"))).collect();
        {
            let mut space = nm.address_space().write();
            for (i, node) in nodes.iter().enumerate() {
                let name = format!("Bulk{i}");
                VariableBuilder::new(node, &name, &name)
                    .data_type(DataTypeId::Double)
                    .value(Variant::Double(i as f64))
                    .organized_by(ObjectId::ObjectsFolder)
                    .insert(&mut *space);
            }
        }

        tokio::spawn(async move {
            if let Err(e) = server.run_with(listener).await {
                tracing::debug!("bench opcua server stopped: {e}");
            }
        });

        if change_rate > 0.0 && points > 0 {
            let subscriptions = handle.subscriptions().clone();
            tokio::spawn(async move {
                let mut values: Vec<f64> = (0..nodes.len()).map(|i| i as f64).collect();
                let mut next = 0usize;
                let mut carry = 0.0f64;
                let mut timer = tokio::time::interval(CHANGE_TICK);
                timer.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
                timer.tick().await;
                let mut last = tokio::time::Instant::now();
                loop {
                    timer.tick().await;
                    let now = tokio::time::Instant::now();
                    let owed = change_rate * (now - last).as_secs_f64() + carry;
                    last = now;
                    let n = owed.floor() as usize;
                    carry = owed - n as f64;
                    if n == 0 {
                        continue;
                    }
                    let stamp = DateTime::now();
                    let mut changed = Vec::with_capacity(n);
                    for _ in 0..n {
                        values[next] += 1.0;
                        changed.push((next, values[next]));
                        next = (next + 1) % values.len();
                    }
                    let updates = changed.iter().map(|(i, v)| {
                        let dv = DataValue {
                            value: Some(Variant::Double(*v)),
                            status: Some(opcua::types::StatusCode::Good),
                            source_timestamp: Some(stamp),
                            server_timestamp: Some(stamp),
                            ..Default::default()
                        };
                        (&nodes[*i], None, dv)
                    });
                    if let Err(e) = nm.set_values(&subscriptions, updates) {
                        tracing::warn!("bench opcua server: update failed: {e}");
                    }
                }
            });
        }

        Ok(BulkServer { endpoint, handle, pki })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use connector_opcua::factory;
    use tedge_dot_sdk::{ConnectorConfig, Quality, Value};

    /// Both connectors read the last bulk node: this is the Rust half; the e2e/bench recipes
    /// cover the C build against the same server.
    #[tokio::test(flavor = "multi_thread")]
    async fn rust_connector_reads_the_last_bulk_node() {
        let server = BulkServer::start(2000, 0.0, "127.0.0.1", 0).await.unwrap();
        let toml = format!(
            "[connector]\nprotocol = \"opcua\"\n[connection]\nsecurity_policy = \"None\"\n[[device]]\nname = \"sim\"\n\
             protocol_address = {{ endpoint = \"{}\" }}\n[[device.point]]\nid = \"last\"\n\
             datatype = \"float64\"\nsubscribe = false\naddress = {{ node_id = \"ns=2;s=Bulk1999\" }}\n",
            server.endpoint
        );
        let cfg: ConnectorConfig = toml::from_str(&toml).unwrap();
        let mut c = factory();
        c.configure(&cfg).unwrap();
        c.connect().await.unwrap();
        let point = tedge_dot_sdk::PointRef {
            id: "last".into(),
            mode: tedge_dot_sdk::Mode::Typed,
            datatype: Some(tedge_dot_sdk::DataType::Float64),
            endianness: tedge_dot_sdk::Endianness::Big,
            word_order: tedge_dot_sdk::WordOrder::Big,
            access: tedge_dot_sdk::Access::Read,
            unit: None,
            transform: Default::default(),
            interval: None,
        };
        let samples = c.read_points(&"sim".into(), &[point]).await.unwrap();
        assert_eq!(samples[0].quality, Quality::Good, "{:?}", samples[0].error);
        assert_eq!(samples[0].value, Some(Value::Number(1999.0)));
        c.disconnect().await.unwrap();
    }
}
