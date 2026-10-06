//! Value mapping (contract §4.3).
//!
//! A point's `map` converts its value between the device's representation and the one its
//! samples carry: state codes to labels, ranges to bands with a catch-all, numeric text to
//! numbers. The runtime applies it after `transform` on reads ([`ValueMap::apply`]) and maps a
//! written value back before the inverse transform on writes ([`ValueMap::invert`]), so a mapped
//! point works as a device parameter without any flow logic.
//!
//! The C SDK (impl/c/sdk/src/map.c) implements the same rules with the same messages; the shared
//! test vectors in `doc/contract/test-vectors/map/` hold both to them.

use crate::model::{DataType, Mode, Value};
use serde_json::Value as Json;

/// The keys a `map` table may carry.
pub const MAP_KEYS: &[&str] = &["cases", "default", "as"];
/// The keys one entry of `map.cases` may carry.
pub const CASE_KEYS: &[&str] = &["eq", "min", "max", "to", "write"];

/// The JSON type of a value, as `value_repr` names it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Number,
    String,
    Bool,
}

impl Kind {
    fn of(value: &Json) -> Option<Kind> {
        match value {
            Json::Number(_) => Some(Kind::Number),
            Json::String(_) => Some(Kind::String),
            Json::Bool(_) => Some(Kind::Bool),
            _ => None,
        }
    }

    /// The kind of value a typed point of `datatype` produces before mapping.
    pub fn of_datatype(datatype: DataType) -> Kind {
        match datatype {
            DataType::Bool => Kind::Bool,
            DataType::String | DataType::Bytes => Kind::String,
            _ => Kind::Number,
        }
    }

    fn parse(name: &str) -> Option<Kind> {
        match name {
            "number" => Some(Kind::Number),
            "string" => Some(Kind::String),
            "bool" => Some(Kind::Bool),
            _ => None,
        }
    }

    /// What a value of this kind is called in messages.
    fn noun(self) -> &'static str {
        match self {
            Kind::Number => "a number",
            Kind::String => "a string",
            Kind::Bool => "a bool",
        }
    }

    /// The `value_repr` of a value of this kind.
    pub fn repr(self) -> &'static str {
        match self {
            Kind::Number => "number",
            Kind::String => "string",
            Kind::Bool => "boolean",
        }
    }
}

/// How one case matches a value.
#[derive(Clone, Debug, PartialEq)]
pub enum Matcher {
    /// Any of these values (a scalar `eq` is a list of one).
    Eq(Vec<Json>),
    /// A numeric range, both bounds inclusive, either open; `write` is the raw value a write of
    /// the case's output sends (a range has no other inverse).
    Range {
        min: Option<f64>,
        max: Option<f64>,
        write: Option<Json>,
    },
}

/// One entry of `map.cases`.
#[derive(Clone, Debug, PartialEq)]
pub struct Case {
    pub matcher: Matcher,
    pub to: Json,
}

/// A point's parsed `map`.
#[derive(Clone, Debug, PartialEq)]
pub struct ValueMap {
    pub cases: Vec<Case>,
    pub default: Option<Json>,
    pub convert: Option<Kind>,
}

impl ValueMap {
    /// Parse and check a `map` table as written. `Ok(None)` for an empty table, which is how a
    /// site removes a map a point library declares. Errors name the key (`map.cases[1].to`) but
    /// not the point; callers prefix that.
    pub fn parse(table: &Json) -> Result<Option<ValueMap>, String> {
        let Some(map) = table.as_object() else {
            return Err("map must be a table".to_string());
        };
        if map.is_empty() {
            return Ok(None);
        }
        let convert = match map.get("as") {
            None => None,
            Some(v) => Some(v.as_str().and_then(Kind::parse).ok_or_else(|| {
                "map.as must be one of \"number\", \"string\", \"bool\"".to_string()
            })?),
        };
        let default = match map.get("default") {
            None => None,
            Some(v) if Kind::of(v).is_some() => Some(v.clone()),
            Some(_) => return Err("map.default must be a number, a string or a bool".to_string()),
        };
        if default.is_some() && convert.is_some() {
            return Err("map.default and map.as cannot be combined".to_string());
        }
        let cases = match map.get("cases") {
            None => Vec::new(),
            Some(Json::Array(items)) => items
                .iter()
                .enumerate()
                .map(|(i, item)| parse_case(item).map_err(|e| format!("map.cases[{i}]{e}")))
                .collect::<Result<_, _>>()?,
            Some(_) => return Err("map.cases must be an array of tables".to_string()),
        };
        if cases.is_empty() && default.is_none() && convert.is_none() {
            return Err("map needs cases, a default or as".to_string());
        }
        let parsed = ValueMap {
            cases,
            default,
            convert,
        };
        parsed.check_output_kinds()?;
        Ok(Some(parsed))
    }

    /// Every output — each `to`, the `default`, the `as` type — is of one kind (§4.3), so a
    /// mapped point always publishes the same `value_repr`.
    fn check_output_kinds(&self) -> Result<(), String> {
        let first = self.output_kind();
        for (i, case) in self.cases.iter().enumerate() {
            if Kind::of(&case.to) != Some(first) {
                return Err(format!(
                    "map.cases[{i}].to must be {} like the map's other outputs",
                    first.noun()
                ));
            }
        }
        if let Some(default) = &self.default {
            if Kind::of(default) != Some(first) {
                return Err(format!(
                    "map.default must be {} like the map's other outputs",
                    first.noun()
                ));
            }
        }
        Ok(())
    }

    /// The kind of every value this map produces.
    pub fn output_kind(&self) -> Kind {
        self.convert
            .or_else(|| self.cases.first().and_then(|c| Kind::of(&c.to)))
            .or_else(|| self.default.as_ref().and_then(Kind::of))
            .unwrap_or(Kind::String)
    }

    /// The checks that need the point's resolved `mode` and `datatype`: a map applies to typed
    /// values only, a range only to numbers, and an `eq` only to values the point can have.
    pub fn check_point(&self, mode: Mode, datatype: Option<DataType>) -> Result<(), String> {
        let datatype = match (mode, datatype) {
            (Mode::Raw, _) => return Err("map is not allowed on a raw-mode point".to_string()),
            (_, Some(DataType::Bytes)) => {
                return Err("map is not allowed on a bytes point".to_string())
            }
            (_, Some(dt)) => dt,
            (_, None) => return Ok(()), // a typed point without datatype is refused elsewhere
        };
        let native = Kind::of_datatype(datatype);
        for (i, case) in self.cases.iter().enumerate() {
            match &case.matcher {
                Matcher::Eq(values) => {
                    if let Some(bad) = values.iter().find(|v| Kind::of(v) != Some(native)) {
                        return Err(format!(
                            "map.cases[{i}].eq {} can never match a {} value",
                            json_text(bad),
                            datatype_name(datatype)
                        ));
                    }
                }
                Matcher::Range { .. } if native != Kind::Number => {
                    return Err(format!(
                        "map.cases[{i}] is a range, which needs a numeric datatype (got {})",
                        datatype_name(datatype)
                    ));
                }
                Matcher::Range { .. } => {}
            }
        }
        Ok(())
    }

    /// The mapped value of a decoded (and transformed) `value` of a point of `datatype`: the
    /// first matching case's `to`, else the `default`, else the `as` conversion. `Err` carries
    /// the sample's error text without the point prefix.
    pub fn apply(&self, value: &Value, datatype: Option<DataType>) -> Result<Value, String> {
        let big = datatype.is_some_and(|dt| matches!(dt, DataType::Int64 | DataType::Uint64));
        for case in &self.cases {
            if case_matches(&case.matcher, value, big) {
                return Ok(json_to_value(&case.to));
            }
        }
        if let Some(default) = &self.default {
            return Ok(json_to_value(default));
        }
        match self.convert {
            Some(kind) => convert(value, kind, big),
            None => Err(format!("no mapping for value {}", value_text(value))),
        }
    }

    /// The device value a write of the mapped `value` sends to a point of `datatype` (§4.3):
    /// the first case whose `to` equals it gives its `eq` (the first, for a list) or its range's
    /// `write`; else, with `as`, the reverse conversion. `Err` is the write's failure reason,
    /// without the point prefix.
    pub fn invert(&self, value: &Json, datatype: Option<DataType>) -> Result<Json, String> {
        let out = self.output_kind();
        if Kind::of(value) != Some(out) {
            return Err(format!(
                "cannot write {}: the point's mapped values are {}s",
                json_text(value),
                out.repr()
            ));
        }
        for case in &self.cases {
            if !json_equal(&case.to, value) {
                continue;
            }
            match &case.matcher {
                Matcher::Eq(values) => return Ok(values[0].clone()),
                Matcher::Range { write: Some(w), .. } => return Ok(w.clone()),
                Matcher::Range { write: None, .. } => {} // read-only; a later case may write it
            }
        }
        if let Some(kind) = self.convert {
            let native = datatype.map(Kind::of_datatype).unwrap_or(kind);
            let converted = convert(&json_to_value(value), native, false)
                .map_err(|e| format!("cannot write {}: {e}", json_text(value)))?;
            return Ok(value_to_json(&converted));
        }
        let accepted = self.writable_outputs();
        Err(if accepted.is_empty() {
            format!("cannot write {}: the point's map has no writable values", json_text(value))
        } else {
            let listed: Vec<String> = accepted.iter().map(json_text).collect();
            format!(
                "cannot write {}; accepted values: {}",
                json_text(value),
                listed.join(", ")
            )
        })
    }

    /// The outputs a write accepts by case, in case order without repeats: those of `eq` cases
    /// and of ranges with a `write`. Complete — a closed set — when the map has no `as`.
    pub fn writable_outputs(&self) -> Vec<Json> {
        let mut out: Vec<Json> = Vec::new();
        for case in &self.cases {
            let writable = match &case.matcher {
                Matcher::Eq(_) => true,
                Matcher::Range { write, .. } => write.is_some(),
            };
            if writable && !out.iter().any(|o| json_equal(o, &case.to)) {
                out.push(case.to.clone());
            }
        }
        out
    }
}

/// One case as written: the error suffix names the key after `map.cases[i]`.
fn parse_case(item: &Json) -> Result<Case, String> {
    let Some(case) = item.as_object() else {
        return Err(" must be a table".to_string());
    };
    let to = match case.get("to") {
        None => return Err(" needs a to".to_string()),
        Some(v) if Kind::of(v).is_some() => v.clone(),
        Some(_) => return Err(".to must be a number, a string or a bool".to_string()),
    };
    let number = |key: &str| -> Result<Option<f64>, String> {
        match case.get(key) {
            None => Ok(None),
            Some(v) => v
                .as_f64()
                .filter(|n| n.is_finite())
                .map(Some)
                .ok_or_else(|| format!(".{key} must be a number")),
        }
    };
    let (min, max) = (number("min")?, number("max")?);
    let range = min.is_some() || max.is_some();
    let matcher = match (case.get("eq"), range) {
        (Some(_), true) => return Err(" cannot have both eq and min/max".to_string()),
        (None, false) => return Err(" needs eq, or min and/or max".to_string()),
        (Some(eq), false) => {
            if case.contains_key("write") {
                return Err(".write is only for a range (the eq value is what is written)".to_string());
            }
            let values = match eq {
                Json::Array(items) if items.is_empty() => {
                    return Err(".eq must not be an empty list".to_string())
                }
                Json::Array(items) => items.clone(),
                scalar => vec![scalar.clone()],
            };
            if values.iter().any(|v| Kind::of(v).is_none()) {
                return Err(".eq must be a number, a string, a bool or a list of them".to_string());
            }
            Matcher::Eq(values)
        }
        (None, true) => {
            if let (Some(lo), Some(hi)) = (min, max) {
                if lo > hi {
                    return Err(".min must not be greater than .max".to_string());
                }
            }
            let write = match case.get("write") {
                None => None,
                Some(w) => {
                    let n = w
                        .as_f64()
                        .filter(|n| n.is_finite())
                        .ok_or_else(|| ".write must be a number".to_string())?;
                    if min.is_some_and(|lo| n < lo) || max.is_some_and(|hi| n > hi) {
                        return Err(".write must lie within the case's range".to_string());
                    }
                    Some(w.clone())
                }
            };
            Matcher::Range { min, max, write }
        }
    };
    Ok(Case { matcher, to })
}

/// A 64-bit integer carried as a string (§4.1), as an exact integer.
fn big_integer(text: &str) -> Option<i128> {
    text.parse::<i128>().ok()
}

fn case_matches(matcher: &Matcher, value: &Value, big: bool) -> bool {
    // The value as a number, when it is one: a 64-bit integer carried as text counts.
    let number = match value {
        Value::Number(n) => Some(*n),
        Value::Text(t) if big => big_integer(t).map(|i| i as f64),
        _ => None,
    };
    match matcher {
        Matcher::Eq(values) => values.iter().any(|eq| match (eq, value) {
            (Json::Bool(a), Value::Bool(b)) => a == b,
            (Json::String(a), Value::Text(b)) if !big => a == b,
            (Json::Number(a), Value::Text(t)) if big => match (big_integer(t), a.as_i64(), a.as_u64()) {
                (Some(v), Some(i), _) => v == i as i128,
                (Some(v), None, Some(u)) => v == u as i128,
                (Some(v), None, None) => a.as_f64() == Some(v as f64),
                (None, ..) => false,
            },
            (Json::Number(a), Value::Number(n)) => a.as_f64() == Some(*n),
            _ => false,
        }),
        Matcher::Range { min, max, .. } => match number {
            Some(n) => min.is_none_or(|lo| lo <= n) && max.is_none_or(|hi| n <= hi),
            None => false,
        },
    }
}

/// The `as` conversion (§4.3) of `value` to `kind`. `big` marks a 64-bit integer carried as
/// text, which is a number, not a string.
fn convert(value: &Value, kind: Kind, big: bool) -> Result<Value, String> {
    let fail = || format!("cannot convert {} to {}", value_text(value), kind.noun());
    match (kind, value) {
        (Kind::Number, Value::Number(n)) => Ok(Value::Number(*n)),
        (Kind::Number, Value::Bool(b)) => Ok(Value::Number(if *b { 1.0 } else { 0.0 })),
        (Kind::Number, Value::Text(t)) => parse_number(t).map(Value::Number).ok_or_else(fail),
        (Kind::String, Value::Text(t)) => Ok(Value::Text(t.clone())),
        (Kind::String, Value::Bool(b)) => Ok(Value::Text(b.to_string())),
        (Kind::String, Value::Number(n)) => {
            format_number(*n).map(Value::Text).ok_or_else(fail)
        }
        (Kind::Bool, Value::Bool(b)) => Ok(Value::Bool(*b)),
        (Kind::Bool, Value::Number(n)) if n.is_nan() => Err(fail()),
        (Kind::Bool, Value::Number(n)) => Ok(Value::Bool(*n != 0.0)),
        (Kind::Bool, Value::Text(t)) if big => {
            big_integer(t).map(|i| Value::Bool(i != 0)).ok_or_else(fail)
        }
        (Kind::Bool, Value::Text(t)) => parse_bool(t).map(Value::Bool).ok_or_else(fail),
    }
}

/// A decimal number as text: optional surrounding whitespace, then
/// `[+-]?(digits[.digits?] | .digits)([eE][+-]?digits)?`, finite. The grammar is spelled out —
/// not left to `str::parse` or C's `strtod` — so both SDKs accept exactly the same strings (no
/// `inf`, `nan` or C's hexadecimal floats).
pub fn parse_number(text: &str) -> Option<f64> {
    let s = crate::library::trim_c(text);
    let b = s.as_bytes();
    let mut i = 0;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let int_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    let mut digits = i - int_start;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let frac_start = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        digits += i - frac_start;
    }
    if digits == 0 {
        return None;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
            i += 1;
        }
        let exp_start = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        if i == exp_start {
            return None;
        }
    }
    if i != b.len() {
        return None;
    }
    s.parse::<f64>().ok().filter(|n| n.is_finite())
}

/// `true`/`1`/`on`/`yes` and `false`/`0`/`off`/`no`, case-insensitively, whitespace trimmed.
fn parse_bool(text: &str) -> Option<bool> {
    match crate::library::trim_c(text).to_ascii_lowercase().as_str() {
        "true" | "1" | "on" | "yes" => Some(true),
        "false" | "0" | "off" | "no" => Some(false),
        _ => None,
    }
}

/// A finite number as text: the shortest decimal digits that read back as the same number, in
/// positional notation (never an exponent), with `-0` written `0`. The C SDK produces the same
/// text (`tdot_map_format_number`).
pub fn format_number(n: f64) -> Option<String> {
    if !n.is_finite() {
        return None;
    }
    if n == 0.0 {
        return Some("0".to_string());
    }
    Some(format!("{n}"))
}

/// A value as messages show it: numbers as [`format_number`] writes them, strings quoted.
fn value_text(value: &Value) -> String {
    match value {
        Value::Number(n) => format_number(*n).unwrap_or_else(|| {
            if n.is_nan() {
                "NaN".to_string()
            } else if *n > 0.0 {
                "inf".to_string()
            } else {
                "-inf".to_string()
            }
        }),
        Value::Text(t) => Json::String(t.clone()).to_string(),
        Value::Bool(b) => b.to_string(),
    }
}

fn json_text(value: &Json) -> String {
    match value {
        Json::Number(n) => match (n.as_i64(), n.as_u64()) {
            (Some(i), _) => i.to_string(),
            (None, Some(u)) => u.to_string(),
            _ => value_text(&Value::Number(n.as_f64().unwrap_or(f64::NAN))),
        },
        other => value_text(&json_to_value(other)),
    }
}

/// Two outputs or written values are equal: numbers numerically (`1` equals `1.0`), the rest
/// exactly.
fn json_equal(a: &Json, b: &Json) -> bool {
    match (a, b) {
        (Json::Number(x), Json::Number(y)) => x.as_f64() == y.as_f64(),
        _ => a == b,
    }
}

fn json_to_value(value: &Json) -> Value {
    match value {
        Json::Bool(b) => Value::Bool(*b),
        Json::Number(n) => Value::Number(n.as_f64().unwrap_or(f64::NAN)),
        Json::String(s) => Value::Text(s.clone()),
        other => Value::Text(other.to_string()),
    }
}

/// A converted value as a write request carries it: whole numbers as JSON integers, as
/// connectors read integer datatypes with `as_i64`/`as_u64`.
fn value_to_json(value: &Value) -> Json {
    match value {
        Value::Bool(b) => Json::Bool(*b),
        Value::Text(t) => Json::String(t.clone()),
        Value::Number(n) if n.fract() == 0.0 && n.abs() < 9_007_199_254_740_992.0 => {
            Json::from(*n as i64)
        }
        Value::Number(n) => serde_json::json!(n),
    }
}

fn datatype_name(datatype: DataType) -> String {
    serde_json::to_value(datatype)
        .ok()
        .and_then(|v| v.as_str().map(str::to_string))
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn map(table: Json) -> ValueMap {
        ValueMap::parse(&table).unwrap().unwrap()
    }

    fn state() -> ValueMap {
        map(json!({
            "cases": [
                { "eq": 0, "to": "stopped" },
                { "eq": [1, 5], "to": "running" },
                { "min": 10, "max": 19, "to": "warning" },
                { "min": 20, "to": "fault", "write": 20 },
            ],
            "default": "unknown",
        }))
    }

    #[test]
    fn cases_default_and_first_match() {
        let m = state();
        let read = |n: f64| m.apply(&Value::Number(n), Some(DataType::Uint16)).unwrap();
        assert_eq!(read(0.0), Value::Text("stopped".into()));
        assert_eq!(read(5.0), Value::Text("running".into()));
        assert_eq!(read(15.0), Value::Text("warning".into()));
        assert_eq!(read(250.0), Value::Text("fault".into()));
        assert_eq!(read(7.0), Value::Text("unknown".into()));
        assert_eq!(read(f64::NAN), Value::Text("unknown".into()));
    }

    #[test]
    fn writes_invert_by_case() {
        let m = state();
        let dt = Some(DataType::Uint16);
        assert_eq!(m.invert(&json!("stopped"), dt), Ok(json!(0)));
        assert_eq!(m.invert(&json!("running"), dt), Ok(json!(1)));
        assert_eq!(m.invert(&json!("fault"), dt), Ok(json!(20)));
        assert_eq!(
            m.invert(&json!("warning"), dt).unwrap_err(),
            "cannot write \"warning\"; accepted values: \"stopped\", \"running\", \"fault\""
        );
        assert!(m.invert(&json!("unknown"), dt).is_err());
        assert_eq!(
            m.invert(&json!(1), dt).unwrap_err(),
            "cannot write 1: the point's mapped values are strings"
        );
    }

    #[test]
    fn unmatched_without_catch_all() {
        let m = map(json!({ "cases": [{ "eq": 0, "to": "off" }] }));
        assert_eq!(
            m.apply(&Value::Number(3.0), Some(DataType::Uint16)),
            Err("no mapping for value 3".to_string())
        );
    }

    #[test]
    fn as_conversions_both_ways() {
        let number = map(json!({ "as": "number", "cases": [{ "eq": "N/A", "to": -1 }] }));
        let dt = Some(DataType::String);
        assert_eq!(number.apply(&Value::Text(" 21.5 ".into()), dt), Ok(Value::Number(21.5)));
        assert_eq!(number.apply(&Value::Text("N/A".into()), dt), Ok(Value::Number(-1.0)));
        assert_eq!(
            number.apply(&Value::Text("abc".into()), dt),
            Err("cannot convert \"abc\" to a number".to_string())
        );
        assert_eq!(number.invert(&json!(21.5), dt), Ok(json!("21.5")));
        assert_eq!(number.invert(&json!(-1), dt), Ok(json!("N/A")));

        let text = map(json!({ "as": "string" }));
        let f = Some(DataType::Float64);
        assert_eq!(text.apply(&Value::Number(0.1), f), Ok(Value::Text("0.1".into())));
        assert_eq!(text.invert(&json!("2.5"), f), Ok(json!(2.5)));
        assert_eq!(text.invert(&json!("12"), Some(DataType::Uint16)), Ok(json!(12)));

        let flag = map(json!({ "as": "bool" }));
        assert_eq!(flag.apply(&Value::Text("ON".into()), dt), Ok(Value::Bool(true)));
        assert_eq!(flag.invert(&json!(true), Some(DataType::Uint16)), Ok(json!(1)));
    }

    #[test]
    fn number_grammar_and_formatting() {
        for ok in ["1", "-1.5", "+.5", "5.", "1e3", " 2E-2 "] {
            assert!(parse_number(ok).is_some(), "{ok}");
        }
        for bad in ["", ".", "e3", "1e", "0x1A", "inf", "NaN", "1 2", "1e999"] {
            assert!(parse_number(bad).is_none(), "{bad}");
        }
        assert_eq!(format_number(0.1).unwrap(), "0.1");
        assert_eq!(format_number(-0.0).unwrap(), "0");
        assert_eq!(format_number(1e21).unwrap(), "1000000000000000000000");
        assert_eq!(format_number(12.0).unwrap(), "12");
        assert_eq!(format_number(1.5e-7).unwrap(), "0.00000015");
    }

    #[test]
    fn big_integers_match_exactly() {
        let m = map(json!({ "cases": [{ "eq": 9007199254740993_i64, "to": "x" }], "default": "y" }));
        let dt = Some(DataType::Uint64);
        assert_eq!(m.apply(&Value::Text("9007199254740993".into()), dt), Ok(Value::Text("x".into())));
        assert_eq!(m.apply(&Value::Text("9007199254740992".into()), dt), Ok(Value::Text("y".into())));
    }

    #[test]
    fn no_coercion_when_matching() {
        let m = map(json!({ "cases": [{ "eq": "1", "to": true }], "default": false }));
        assert_eq!(m.apply(&Value::Text("1".into()), Some(DataType::String)), Ok(Value::Bool(true)));
        assert_eq!(m.apply(&Value::Number(1.0), None), Ok(Value::Bool(false)));
    }

    #[test]
    fn validation() {
        let err = |t: Json| ValueMap::parse(&t).unwrap_err();
        assert_eq!(ValueMap::parse(&json!({})), Ok(None));
        assert_eq!(
            err(json!({ "cases": [{ "eq": 0, "to": "off" }, { "eq": 1, "to": 1 }] })),
            "map.cases[1].to must be a string like the map's other outputs"
        );
        assert_eq!(err(json!({ "default": 0, "as": "number" })), "map.default and map.as cannot be combined");
        assert_eq!(
            err(json!({ "cases": [{ "min": 10, "max": 19, "to": "w", "write": 25 }] })),
            "map.cases[0].write must lie within the case's range"
        );
        assert_eq!(err(json!({ "cases": [{ "eq": 1, "min": 0, "to": "x" }] })), "map.cases[0] cannot have both eq and min/max");
        assert_eq!(err(json!({ "cases": [{ "to": "x" }] })), "map.cases[0] needs eq, or min and/or max");
        assert_eq!(err(json!({ "cases": [{ "eq": [], "to": "x" }] })), "map.cases[0].eq must not be an empty list");
        assert_eq!(err(json!({ "cases": [{ "min": 2, "max": 1, "to": "x" }] })), "map.cases[0].min must not be greater than .max");
        assert_eq!(err(json!({ "cases": [{ "eq": 1, "to": "x", "write": 1 }] })), "map.cases[0].write is only for a range (the eq value is what is written)");
        assert_eq!(err(json!({ "as": "text" })), "map.as must be one of \"number\", \"string\", \"bool\"");
        assert_eq!(err(json!({ "cases": [] })), "map needs cases, a default or as");

        let m = map(json!({ "cases": [{ "min": 0, "to": "x" }] }));
        assert_eq!(
            m.check_point(Mode::Typed, Some(DataType::String)).unwrap_err(),
            "map.cases[0] is a range, which needs a numeric datatype (got string)"
        );
        assert_eq!(
            m.check_point(Mode::Raw, Some(DataType::Uint16)).unwrap_err(),
            "map is not allowed on a raw-mode point"
        );
        let m = map(json!({ "cases": [{ "eq": "a", "to": 1 }] }));
        assert_eq!(
            m.check_point(Mode::Typed, Some(DataType::Uint16)).unwrap_err(),
            "map.cases[0].eq \"a\" can never match a uint16 value"
        );
    }

    // ---- shared vectors (doc/contract/test-vectors/map/vectors.json) ----------------------

    const VECTORS: &str = include_str!("../../../../../doc/contract/test-vectors/map/vectors.json");

    fn datatype(name: &str) -> DataType {
        serde_json::from_value(json!(name)).unwrap()
    }

    /// A vector value: `{"num": n}`, `{"nan": true}`, `{"bool": b}` or `{"str": s}`.
    fn vector_value(v: &Json) -> Value {
        if let Some(n) = v.get("num") {
            Value::Number(n.as_f64().unwrap())
        } else if v.get("nan").is_some() {
            Value::Number(f64::NAN)
        } else if let Some(b) = v.get("bool") {
            Value::Bool(b.as_bool().unwrap())
        } else {
            Value::Text(v["str"].as_str().unwrap().to_string())
        }
    }

    fn named_map(vectors: &Json, v: &Json) -> Json {
        match &v["map"] {
            Json::String(name) => vectors["maps"][name.as_str()].clone(),
            inline => inline.clone(),
        }
    }

    #[test]
    fn shared_read_vectors() {
        let vectors: Json = serde_json::from_str(VECTORS).unwrap();
        for v in vectors["read"].as_array().unwrap() {
            let name = v["name"].as_str().unwrap();
            let m = map(named_map(&vectors, v));
            let dt = datatype(v["datatype"].as_str().unwrap());
            let mut input = vector_value(&v["in"]);
            if let Some(t) = v.get("transform") {
                let t: crate::model::Transform = serde_json::from_value(t.clone()).unwrap();
                input = t.apply(input);
            }
            let got = m.apply(&input, Some(dt));
            match v.get("error") {
                Some(e) => assert_eq!(got, Err(e.as_str().unwrap().to_string()), "{name}"),
                None => {
                    let want = vector_value(&v["out"]);
                    assert_eq!(got.as_ref().map(Value::repr), Ok(want.repr()), "{name}");
                    assert_eq!(got, Ok(want), "{name}");
                }
            }
        }
    }

    /// Writes go through the runtime's write path, so the inverse transform and the raw bypass
    /// are exercised as every write verb runs them.
    #[test]
    fn shared_write_vectors() {
        let vectors: Json = serde_json::from_str(VECTORS).unwrap();
        for v in vectors["write"].as_array().unwrap() {
            let name = v["name"].as_str().unwrap();
            let mut point = json!({
                "id": "p",
                "datatype": v["datatype"],
                "access": "read_write",
                "address": {},
                "map": named_map(&vectors, v),
            });
            if let Some(t) = v.get("transform") {
                point["transform"] = t.clone();
            }
            let config: crate::config::ConnectorConfig = serde_json::from_value(json!({
                "connector": { "protocol": "test" },
                "device": [{ "name": "d", "protocol_address": {}, "point": [point] }],
            }))
            .unwrap();
            let request = crate::connector::CommandRequest {
                point: "p".into(),
                value: v.get("value").cloned(),
                value_repr: None,
                raw: v.get("raw").and_then(Json::as_str).map(str::to_string),
            };
            let got = crate::runtime::raw_unit_request(&config, "d", &request);
            match (v.get("error"), v.get("device"), v.get("device_raw")) {
                (Some(e), ..) => {
                    assert_eq!(got.unwrap_err(), format!("point p: {}", e.as_str().unwrap()), "{name}")
                }
                (None, Some(device), _) => {
                    let got = got.unwrap().value.unwrap();
                    assert!(json_equal(&got, device) && Kind::of(&got) == Kind::of(device), "{name}: {got}");
                }
                (None, None, Some(raw)) => {
                    let got = got.unwrap();
                    assert_eq!(got.raw.as_deref(), raw.as_str(), "{name}");
                }
                _ => panic!("{name}: no expectation"),
            }
        }
    }

    #[test]
    fn shared_invalid_vectors() {
        let vectors: Json = serde_json::from_str(VECTORS).unwrap();
        for v in vectors["invalid"].as_array().unwrap() {
            let name = v["name"].as_str().unwrap();
            let mode = match v.get("mode").and_then(Json::as_str) {
                Some("raw") => Mode::Raw,
                _ => Mode::Typed,
            };
            let dt = datatype(v["datatype"].as_str().unwrap());
            let got = ValueMap::parse(&v["map"])
                .and_then(|m| m.unwrap().check_point(mode, Some(dt)));
            assert_eq!(got, Err(v["error"].as_str().unwrap().to_string()), "{name}");
        }
    }
}

