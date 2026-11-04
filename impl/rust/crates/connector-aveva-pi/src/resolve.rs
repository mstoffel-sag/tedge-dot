//! WebId resolution (spec §3.3, design D2): every point is looked up once per connection, and
//! the result (WebId, PI point type, units, digital set) is cached for its reads and writes.

use crate::client::{query, Client, HttpError};
use crate::config::Address;
use serde_json::Value as Json;
use std::collections::HashMap;
use std::sync::Arc;

/// A digital state set: `(name, code)` pairs.
pub type DigitalSet = Arc<Vec<(String, i64)>>;

/// A resolved point: the stream it reads and writes.
#[derive(Clone, Debug, PartialEq)]
pub struct Stream {
    pub web_id: String,
    /// The path as configured (or the WebId), echoed in `addr.path`.
    pub path: String,
    /// The PI point type (`Float64`, `Digital`, ...) or AF attribute type, when known.
    pub point_type: Option<String>,
    /// The PI engineering unit or the attribute's default unit, when non-empty.
    pub units: Option<String>,
    pub digital_set: Option<String>,
    pub digital: Option<DigitalSet>,
}

impl Stream {
    pub fn is_digital(&self) -> bool {
        self.point_type
            .as_deref()
            .is_some_and(|t| t == "Digital" || t == "EnumerationValue")
    }
}

fn text(v: &Json, key: &str) -> Option<String> {
    v.get(key)
        .and_then(Json::as_str)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

/// Look up one point. `Ok(None)` when the server does not have it: any 4xx other than 401/403
/// (the reference documents no status for a missing path), or an answer without a WebId.
pub async fn resolve(client: &Client, address: &Address) -> Result<Option<Stream>, HttpError> {
    let (endpoint, fields) = match address {
        Address::WebId(w) => {
            return Ok(Some(Stream {
                web_id: w.clone(),
                path: w.clone(),
                point_type: None,
                units: None,
                digital_set: None,
                digital: None,
            }))
        }
        Address::Point(_) => ("points", "WebId;Name;PointType;EngineeringUnits;DigitalSetName"),
        Address::Attribute(_) => ("attributes", "WebId;Name;Type;DefaultUnitsNameAbbreviation"),
    };
    let answer = client
        .get(endpoint, &query(&[("path", address.text()), ("selectedFields", fields)]))
        .await;
    let doc = match answer {
        Ok(doc) => doc,
        Err(HttpError::Status { code, .. }) if (400..500).contains(&code) => return Ok(None),
        Err(e) => return Err(e),
    };
    let Some(web_id) = text(&doc, "WebId") else {
        return Ok(None);
    };
    Ok(Some(match address {
        Address::Point(path) => Stream {
            web_id,
            path: path.clone(),
            point_type: text(&doc, "PointType"),
            units: text(&doc, "EngineeringUnits"),
            digital_set: text(&doc, "DigitalSetName"),
            digital: None,
        },
        _ => Stream {
            web_id,
            path: address.text().to_string(),
            point_type: text(&doc, "Type"),
            units: text(&doc, "DefaultUnitsNameAbbreviation"),
            digital_set: None,
            digital: None,
        },
    }))
}

/// The server of a PI point path `\\server\tag`.
pub fn server_of(path: &str) -> Option<&str> {
    path.strip_prefix("\\\\")?.split('\\').next().filter(|s| !s.is_empty())
}

/// Digital sets, looked up once per connection and shared by the points that use them.
#[derive(Default)]
pub struct DigitalSets {
    sets: HashMap<(String, String), DigitalSet>,
    server_ids: HashMap<String, String>,
}

impl DigitalSets {
    /// The states of `set` on `server`. `Ok(None)` when the server or set does not exist.
    pub async fn get(&mut self, client: &Client, server: &str, set: &str) -> Result<Option<DigitalSet>, HttpError> {
        let key = (server.to_uppercase(), set.to_uppercase());
        if let Some(s) = self.sets.get(&key) {
            return Ok(Some(s.clone()));
        }
        let server_id = match self.server_ids.get(&key.0) {
            Some(id) => id.clone(),
            None => {
                let path = format!("\\\\{server}");
                let doc = match client.get("dataservers", &query(&[("path", &path), ("selectedFields", "WebId")])).await {
                    Ok(doc) => doc,
                    Err(HttpError::Status { code, .. }) if (400..500).contains(&code) => return Ok(None),
                    Err(e) => return Err(e),
                };
                let Some(id) = text(&doc, "WebId") else {
                    return Ok(None);
                };
                self.server_ids.insert(key.0.clone(), id.clone());
                id
            }
        };
        let sets = client
            .get(&format!("dataservers/{server_id}/enumerationsets"), &query(&[("selectedFields", "Items.WebId;Items.Name")]))
            .await?;
        let Some(set_id) = sets
            .get("Items")
            .and_then(Json::as_array)
            .and_then(|items| {
                items
                    .iter()
                    .find(|i| i.get("Name").and_then(Json::as_str).is_some_and(|n| n.eq_ignore_ascii_case(set)))
            })
            .and_then(|i| text(i, "WebId"))
        else {
            return Ok(None);
        };
        let values = client
            .get(&format!("enumerationsets/{set_id}/values"), &query(&[("selectedFields", "Items.Name;Items.Value")]))
            .await?;
        let states: Vec<(String, i64)> = values
            .get("Items")
            .and_then(Json::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(|i| Some((text(i, "Name")?, i.get("Value")?.as_i64()?)))
                    .collect()
            })
            .unwrap_or_default();
        let states = Arc::new(states);
        self.sets.insert(key, states.clone());
        Ok(Some(states))
    }
}

/// The digital state a write names: a state name or a code of the set.
pub fn digital_state(set: &[(String, i64)], value: &Json) -> Option<(String, i64)> {
    match value {
        Json::String(name) => set
            .iter()
            .find(|(n, _)| n == name)
            .or_else(|| set.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)))
            .cloned(),
        Json::Number(n) => {
            let code = n.as_i64().or_else(|| n.as_f64().filter(|f| f.fract() == 0.0).map(|f| f as i64))?;
            set.iter().find(|(_, c)| *c == code).cloned()
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn server_of_a_path() {
        assert_eq!(server_of("\\\\PISRV01\\SINUSOID"), Some("PISRV01"));
        assert_eq!(server_of("SINUSOID"), None);
    }

    #[test]
    fn digital_state_by_name_or_code() {
        let set = vec![("Stopped".to_string(), 0), ("Running".to_string(), 1)];
        assert_eq!(digital_state(&set, &serde_json::json!("Running")), Some(("Running".into(), 1)));
        assert_eq!(digital_state(&set, &serde_json::json!("running")), Some(("Running".into(), 1)));
        assert_eq!(digital_state(&set, &serde_json::json!(0)), Some(("Stopped".into(), 0)));
        assert_eq!(digital_state(&set, &serde_json::json!("Exploded")), None);
        assert_eq!(digital_state(&set, &serde_json::json!(7)), None);
    }
}
