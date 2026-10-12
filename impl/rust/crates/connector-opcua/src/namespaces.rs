//! The local namespace table (openspec change `opcua-namespace-table`).
//!
//! A configured node id such as `ns=3;i=1234` names its namespace by position in the server's
//! `NamespaceArray`, and a server may number its namespaces differently after a restart (OPC
//! 10000-5 §6.3.1), or another instance of the same device type may number them differently
//! from the start. A device (`protocol_address.namespaces`) or a point library
//! (`[library] namespaces`) can therefore record the array its ids were written against. For
//! every point that has such a table, each session translates the configured index to the URI the
//! table lists there, and that URI to the index the server uses for it now.
//!
//! The translation is made once per session: the server may add namespaces while a session is
//! open, but it "shall not modify or delete entries" (§6.3.1), so a translation stays valid until
//! the session ends. Only points that could not be translated are tried again.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::Duration;

use opcua::client::Session;
use opcua::types::{AttributeId, NodeId, ReadValueId, TimestampsToReturn, Variant};

/// The URI of namespace 0, which is never translated.
pub const OPCUA_URI: &str = "http://opcfoundation.org/UA/";

/// `Server_NamespaceArray`.
const NAMESPACE_ARRAY: u32 = 2255;

/// A namespace table: the server's `NamespaceArray` when the node ids were written.
pub type Table = Arc<Vec<String>>;

/// Parse a `namespaces` value: an array of URI strings whose entry 0 is the OPC UA namespace.
pub fn parse_table(value: &serde_json::Value) -> Result<Table, String> {
    let entries = value
        .as_array()
        .ok_or("namespaces must be an array of namespace URI strings")?;
    let table: Vec<String> = entries
        .iter()
        .map(|entry| entry.as_str().map(str::to_string))
        .collect::<Option<_>>()
        .ok_or("namespaces must be an array of namespace URI strings")?;
    match table.first() {
        Some(first) if first == OPCUA_URI => Ok(Arc::new(table)),
        Some(first) => Err(format!("namespaces entry 0 must be '{OPCUA_URI}', found '{first}'")),
        None => Err(format!("namespaces entry 0 must be '{OPCUA_URI}', found none")),
    }
}

/// Check that a point's namespace index is listed in its table.
pub fn check_index(node: &NodeId, table: &[String]) -> Result<(), String> {
    let ns = node.namespace as usize;
    if ns >= table.len() {
        return Err(format!(
            "namespace index {ns} is not in the namespace table ({} entries)",
            table.len()
        ));
    }
    Ok(())
}

/// A point that is translated through a table: its configured node id has a namespace other
/// than 0.
pub struct Mapped<'a> {
    pub point: &'a str,
    pub node: &'a NodeId,
    pub table: &'a [String],
}

/// A namespace the server lists at another index than the table, for the log.
#[derive(Debug, PartialEq, Eq)]
pub struct Moved {
    pub uri: String,
    pub local: u16,
    pub server: u16,
}

/// The translations of one session.
#[derive(Default)]
pub struct Translation {
    /// Per mapped point: the node id to use on the server, or why there is none.
    pub nodes: HashMap<String, Result<NodeId, String>>,
    /// Namespaces already logged as moved in this session.
    logged: HashSet<String>,
}

impl Translation {
    /// Whether any of `points` (all mapped points when `None`) is untranslated.
    pub fn has_failures<'a>(&self, mut points: impl Iterator<Item = &'a str>) -> bool {
        points.any(|p| matches!(self.nodes.get(p), Some(Err(_))))
    }

    /// Translate `points` against the server's array (or the reason it could not be read),
    /// replacing what was recorded for them. Returns the namespaces to log as moved: each
    /// once per session.
    pub fn apply(&mut self, points: &[Mapped], server: &Result<Vec<String>, String>) -> Vec<Moved> {
        let (nodes, moved) = translate(points, server);
        self.nodes.extend(nodes);
        moved.into_iter().filter(|m| self.logged.insert(m.uri.clone())).collect()
    }
}

/// Translate each point: configured index → URI in its table → index of that URI on the server
/// (exact, case-sensitive match; the first occurrence). Pure, so the rules are unit-tested.
pub fn translate(
    points: &[Mapped],
    server: &Result<Vec<String>, String>,
) -> (HashMap<String, Result<NodeId, String>>, Vec<Moved>) {
    let mut nodes = HashMap::new();
    let mut moved: Vec<Moved> = Vec::new();
    for p in points {
        let local = p.node.namespace;
        // `check_index` ran at configure, so the table lists the index.
        let uri = &p.table[local as usize];
        let result = match server {
            Err(e) => Err(format!("namespace array unavailable: {e}")),
            Ok(array) => match array.iter().position(|u| u == uri) {
                None => Err(format!("namespace not found: {uri}")),
                Some(index) => {
                    let index = index as u16;
                    if index != local && !moved.iter().any(|m| &m.uri == uri) {
                        moved.push(Moved { uri: uri.clone(), local, server: index });
                    }
                    Ok(NodeId { namespace: index, identifier: p.node.identifier.clone() })
                }
            },
        };
        nodes.insert(p.point.to_string(), result);
    }
    (nodes, moved)
}

/// Read the server's `NamespaceArray`; the error is the reason for the points' bad samples.
pub async fn read_array(session: &Session, timeout: Duration) -> Result<Vec<String>, String> {
    let read = ReadValueId {
        node_id: NodeId::new(0, NAMESPACE_ARRAY),
        attribute_id: AttributeId::Value as u32,
        index_range: Default::default(),
        data_encoding: Default::default(),
    };
    let values = tokio::time::timeout(timeout, session.read(&[read], TimestampsToReturn::Neither, 0.0))
        .await
        .map_err(|_| format!("timed out after {}s", timeout.as_secs()))?
        .map_err(|status| status.to_string())?;
    let value = values.into_iter().next().ok_or("no value returned")?;
    if let Some(status) = value.status {
        if !status.is_good() {
            return Err(status.to_string());
        }
    }
    match value.value {
        Some(Variant::Array(array)) => Ok(array
            .values
            .into_iter()
            .map(|v| match v {
                Variant::String(s) => s.as_ref().to_string(),
                _ => String::new(),
            })
            .collect()),
        _ => Err("not an array of strings".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn table(uris: &[&str]) -> Vec<String> {
        uris.iter().map(|u| u.to_string()).collect()
    }

    const PLC: &str = "urn:acme:plc";

    #[test]
    fn a_table_is_an_array_of_uris_starting_with_the_opcua_namespace() {
        let t = parse_table(&json!([OPCUA_URI, "urn:server", PLC])).unwrap();
        assert_eq!(t.len(), 3);
        assert_eq!(
            parse_table(&json!([PLC])).unwrap_err(),
            format!("namespaces entry 0 must be '{OPCUA_URI}', found '{PLC}'")
        );
        assert_eq!(
            parse_table(&json!([])).unwrap_err(),
            format!("namespaces entry 0 must be '{OPCUA_URI}', found none")
        );
        for bad in [json!(PLC), json!([OPCUA_URI, 3])] {
            assert_eq!(parse_table(&bad).unwrap_err(), "namespaces must be an array of namespace URI strings");
        }
    }

    #[test]
    fn an_index_outside_the_table_is_refused() {
        let t = table(&[OPCUA_URI, "urn:server", PLC]);
        assert!(check_index(&NodeId::new(2, 1u32), &t).is_ok());
        assert_eq!(
            check_index(&NodeId::new(5, 1u32), &t).unwrap_err(),
            "namespace index 5 is not in the namespace table (3 entries)"
        );
    }

    #[test]
    fn a_moved_namespace_is_followed_and_reported_once() {
        let t = table(&[OPCUA_URI, "urn:server", PLC]);
        let (a, b) = (NodeId::new(2, 1234u32), NodeId::new(2, "Line1.Temp"));
        let points = [
            Mapped { point: "a", node: &a, table: &t },
            Mapped { point: "b", node: &b, table: &t },
        ];
        let server = Ok(table(&[OPCUA_URI, "urn:server", "urn:other", PLC]));
        let (nodes, moved) = translate(&points, &server);
        assert_eq!(nodes["a"], Ok(NodeId::new(3, 1234u32)));
        assert_eq!(nodes["b"], Ok(NodeId::new(3, "Line1.Temp")));
        assert_eq!(moved, [Moved { uri: PLC.into(), local: 2, server: 3 }]);
    }

    #[test]
    fn the_same_order_translates_to_the_configured_ids() {
        let t = table(&[OPCUA_URI, "urn:server", PLC]);
        let a = NodeId::new(2, 1u32);
        let (nodes, moved) = translate(&[Mapped { point: "a", node: &a, table: &t }], &Ok(t.clone()));
        assert_eq!(nodes["a"], Ok(a.clone()));
        assert!(moved.is_empty());
    }

    #[test]
    fn uris_match_exactly_and_the_first_occurrence_wins() {
        let t = table(&[OPCUA_URI, PLC]);
        let a = NodeId::new(1, 1u32);
        let points = [Mapped { point: "a", node: &a, table: &t }];
        let (nodes, _) = translate(&points, &Ok(table(&[OPCUA_URI, "URN:ACME:PLC"])));
        assert_eq!(nodes["a"], Err(format!("namespace not found: {PLC}")));
        let (nodes, _) = translate(&points, &Ok(table(&[OPCUA_URI, "x", PLC, PLC])));
        assert_eq!(nodes["a"], Ok(NodeId::new(2, 1u32)));
    }

    #[test]
    fn an_unreadable_array_fails_every_mapped_point() {
        let t = table(&[OPCUA_URI, PLC]);
        let a = NodeId::new(1, 1u32);
        let (nodes, moved) =
            translate(&[Mapped { point: "a", node: &a, table: &t }], &Err("BadTimeout".into()));
        assert_eq!(nodes["a"], Err("namespace array unavailable: BadTimeout".into()));
        assert!(moved.is_empty());
    }

    #[test]
    fn a_moved_namespace_is_logged_once_per_session() {
        let t = table(&[OPCUA_URI, PLC]);
        let a = NodeId::new(1, 1u32);
        let points = [Mapped { point: "a", node: &a, table: &t }];
        let server = Ok(table(&[OPCUA_URI, "x", PLC]));
        let mut translation = Translation::default();
        assert_eq!(translation.apply(&points, &server).len(), 1);
        assert!(translation.apply(&points, &server).is_empty());
        assert!(!translation.has_failures(["a"].into_iter()));
    }
}
