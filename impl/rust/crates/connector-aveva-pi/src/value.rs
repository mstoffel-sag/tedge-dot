//! Decoding a PI value (spec §4): one pure function, checked against the golden vectors in
//! `connectors/aveva-pi/conformance/decode-vectors.json` (which the C build runs too).
//!
//! No I/O here: the decoder is fuzzed (`fuzz/fuzz_targets/aveva_pi_value.rs`) and property
//! tested.

use serde_json::Value as Json;
use tedge_dot_sdk::{DataType, Mode, Quality, Value};

/// Integers up to this magnitude are carried as JSON numbers; larger ones as text (contract §4.1).
const SAFE: f64 = 9_007_199_254_740_992.0; // 2^53

/// A decoded PI value: what the sample is built from.
#[derive(Clone, Debug, PartialEq)]
pub struct Decoded {
    pub quality: Quality,
    /// Typed mode, good samples only.
    pub value: Option<Value>,
    pub error: Option<String>,
    /// The canonical JSON text of `Value` (the sample's raw bytes).
    pub raw: String,
    /// `questionable`, `substituted`, `annotated`, in that order, as PI set them.
    pub flags: Vec<&'static str>,
    /// The PI `Timestamp`, as PI wrote it.
    pub timestamp: Option<String>,
    /// The value's `UnitsAbbreviation`, when non-empty.
    pub units: Option<String>,
}

/// The configuration and error-message name of a datatype (`int32`, `float64`, ...).
pub fn datatype_name(dt: DataType) -> &'static str {
    match dt {
        DataType::Bool => "bool",
        DataType::Int8 => "int8",
        DataType::Uint8 => "uint8",
        DataType::Int16 => "int16",
        DataType::Uint16 => "uint16",
        DataType::Int32 => "int32",
        DataType::Uint32 => "uint32",
        DataType::Int64 => "int64",
        DataType::Uint64 => "uint64",
        DataType::Float32 => "float32",
        DataType::Float64 => "float64",
        DataType::String => "string",
        DataType::Bytes => "bytes",
    }
}

fn int_range(dt: DataType) -> Option<(i128, i128)> {
    Some(match dt {
        DataType::Int8 => (i8::MIN as i128, i8::MAX as i128),
        DataType::Uint8 => (0, u8::MAX as i128),
        DataType::Int16 => (i16::MIN as i128, i16::MAX as i128),
        DataType::Uint16 => (0, u16::MAX as i128),
        DataType::Int32 => (i32::MIN as i128, i32::MAX as i128),
        DataType::Uint32 => (0, u32::MAX as i128),
        DataType::Int64 => (i64::MIN as i128, i64::MAX as i128),
        DataType::Uint64 => (0, u64::MAX as i128),
        _ => return None,
    })
}

// ---- canonical text (spec §4, "Canonical text") --------------------------------------------

/// A float in its shortest round-trip digits, as Python's `repr` writes it: fixed notation
/// with at least one fractional digit when the decimal exponent is in [-4, 16), else exponent
/// form with a sign and at least two exponent digits (`1e+300`, `5e-05`).
fn float_repr(f: f64) -> String {
    let sci = format!("{:e}", f.abs()); // shortest digits: "1.2345e1", "1e300", "5e-5"
    let (mantissa, exp) = sci.split_once('e').expect("{:e} has an exponent");
    let exp: i32 = exp.parse().expect("{:e} exponent is an integer");
    let digits: String = mantissa.chars().filter(|c| *c != '.').collect();
    let sign = if f.is_sign_negative() { "-" } else { "" };
    if (-4..16).contains(&exp) {
        let body = if exp >= 0 {
            let point = exp as usize + 1;
            if digits.len() <= point {
                format!("{digits}{}.0", "0".repeat(point - digits.len()))
            } else {
                format!("{}.{}", &digits[..point], &digits[point..])
            }
        } else {
            format!("0.{}{digits}", "0".repeat((-exp - 1) as usize))
        };
        format!("{sign}{body}")
    } else {
        let m = if digits.len() == 1 { digits.clone() } else { format!("{}.{}", &digits[..1], &digits[1..]) };
        format!("{sign}{m}e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs())
    }
}

/// A JSON number in canonical form: integral and below 2^53 in magnitude as an integer, any
/// other in [`float_repr`] form. Integers PI sent as integers are written exactly.
pub fn number_text(n: &serde_json::Number) -> String {
    if let Some(i) = n.as_i64() {
        return i.to_string();
    }
    if let Some(u) = n.as_u64() {
        return u.to_string();
    }
    let f = n.as_f64().unwrap_or(f64::NAN);
    float_text(f)
}

/// [`number_text`] for a float.
pub fn float_text(f: f64) -> String {
    if f.is_finite() && f == f.trunc() && f.abs() < SAFE {
        format!("{}", f as i64)
    } else {
        float_repr(f)
    }
}

/// Compact JSON with object keys sorted and numbers in canonical form.
pub fn canonical(v: &Json) -> String {
    let mut out = String::new();
    write_canonical(v, &mut out);
    out
}

fn write_canonical(v: &Json, out: &mut String) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Number(n) => out.push_str(&number_text(n)),
        Json::String(s) => out.push_str(&serde_json::to_string(s).expect("a string serializes")),
        Json::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_canonical(item, out);
            }
            out.push(']');
        }
        Json::Object(m) => {
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            out.push('{');
            for (i, k) in keys.into_iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&serde_json::to_string(k).expect("a string serializes"));
                out.push(':');
                write_canonical(&m[k], out);
            }
            out.push('}');
        }
    }
}

// ---- decoding ------------------------------------------------------------------------------

/// A PI flag: a JSON boolean or the string "true"/"false" (any case); absent is false.
pub fn flag(pi: &Json, key: &str) -> bool {
    match pi.get(key) {
        Some(Json::Bool(b)) => *b,
        Some(Json::String(s)) => s.eq_ignore_ascii_case("true"),
        _ => false,
    }
}

/// The messages of an `{"Errors": [...]}` object, joined with "; ".
pub fn errors_of(v: &Json) -> Option<String> {
    let list = v.get("Errors")?.as_array()?;
    let mut msgs = Vec::new();
    for e in list {
        match e {
            Json::String(s) => msgs.push(s.clone()),
            Json::Object(_) => match e.get("Message") {
                Some(Json::Array(m)) => msgs.extend(m.iter().map(|x| match x {
                    Json::String(s) => s.clone(),
                    other => other.to_string(),
                })),
                Some(Json::String(s)) => msgs.push(s.clone()),
                Some(other) => msgs.push(other.to_string()),
                None => {}
            },
            _ => {}
        }
    }
    Some(if msgs.is_empty() { "error".into() } else { msgs.join("; ") })
}

fn is_digital(v: &Json) -> bool {
    v.as_object()
        .is_some_and(|m| m.contains_key("Name") && m.contains_key("Value") && !m.contains_key("Errors"))
}

fn int_value(v: i128) -> Value {
    if (v as f64).abs() < SAFE {
        Value::Number(v as f64)
    } else {
        Value::Text(v.to_string())
    }
}

/// Convert a PI value to the point's datatype.
pub fn to_datatype(v: &Json, dt: DataType) -> Result<Value, String> {
    let mut v = v;
    if is_digital(v) {
        if dt == DataType::String {
            return match &v["Name"] {
                Json::String(name) => Ok(Value::Text(name.clone())),
                _ => Err("unsupported PI value".into()),
            };
        }
        v = &v["Value"];
    }
    let name = datatype_name(dt);
    match v {
        Json::Bool(b) => match dt {
            DataType::Bool => Ok(Value::Bool(*b)),
            DataType::String => Ok(Value::Text(b.to_string())),
            _ => Err(format!("expected {name}, got a boolean")),
        },
        Json::Number(n) => {
            let f = n.as_f64().unwrap_or(f64::NAN);
            let doesnt_fit = || format!("{} does not fit {name}", number_text(n));
            match dt {
                DataType::String => Ok(Value::Text(number_text(n))),
                DataType::Bool => match f {
                    0.0 => Ok(Value::Bool(false)),
                    1.0 => Ok(Value::Bool(true)),
                    _ => Err(doesnt_fit()),
                },
                DataType::Float64 => Ok(Value::Number(f)),
                DataType::Float32 => {
                    let r = f as f32;
                    if r.is_finite() {
                        Ok(Value::Number(r as f64))
                    } else {
                        Err(doesnt_fit())
                    }
                }
                _ => {
                    let (lo, hi) = int_range(dt).ok_or_else(|| format!("unsupported datatype {name}"))?;
                    let exact = n
                        .as_i64()
                        .map(i128::from)
                        .or_else(|| n.as_u64().map(i128::from))
                        .or_else(|| (f.is_finite() && f == f.trunc() && f.abs() < 1e39).then_some(f as i128));
                    match exact {
                        Some(i) if (lo..=hi).contains(&i) => Ok(int_value(i)),
                        _ => Err(doesnt_fit()),
                    }
                }
            }
        }
        Json::String(s) => match dt {
            DataType::String => Ok(Value::Text(s.clone())),
            _ => Err(format!("expected {name}, got a string")),
        },
        _ => Err("unsupported PI value".into()),
    }
}

/// Decode one PI value object (`{Timestamp, Value, Good, ...}`) for a point.
pub fn decode(pi: &Json, datatype: Option<DataType>, mode: Mode) -> Decoded {
    let v = pi.get("Value").unwrap_or(&Json::Null);
    let mut error = errors_of(v).or_else(|| errors_of(pi));
    if error.is_none() && is_digital(v) && v.get("IsSystem") == Some(&Json::Bool(true)) {
        error = Some(match &v["Name"] {
            Json::String(s) => s.clone(),
            _ => "not good".into(),
        });
    }
    if error.is_none() && pi.get("Good").is_some() && !flag(pi, "Good") {
        error = Some(match v.get("Name") {
            Some(Json::String(s)) if is_digital(v) => s.clone(),
            _ => "not good".into(),
        });
    }
    if error.is_none() && v.is_null() {
        error = Some("unsupported PI value".into());
    }
    let mut value = None;
    if error.is_none() && mode == Mode::Typed {
        match datatype.map(|dt| to_datatype(v, dt)) {
            Some(Ok(x)) => value = Some(x),
            Some(Err(e)) => error = Some(e),
            None => error = Some("no datatype".into()),
        }
    }
    let flags = [("Questionable", "questionable"), ("Substituted", "substituted"), ("Annotated", "annotated")]
        .into_iter()
        .filter(|(key, _)| flag(pi, key))
        .map(|(_, name)| name)
        .collect();
    Decoded {
        quality: if error.is_none() { Quality::Good } else { Quality::Bad },
        value,
        error,
        raw: canonical(v),
        flags,
        timestamp: pi.get("Timestamp").and_then(Json::as_str).map(str::to_string),
        units: pi
            .get("UnitsAbbreviation")
            .and_then(Json::as_str)
            .filter(|u| !u.is_empty())
            .map(str::to_string),
    }
}

/// The JSON a write sends as `Value` for a point (spec §6): the requested value encoded for the
/// datatype. Digital points are handled by the caller (they need the point's digital set).
pub fn encode_write(value: &Json, dt: Option<DataType>) -> Result<Json, String> {
    let Some(dt) = dt else {
        return Ok(value.clone());
    };
    let name = datatype_name(dt);
    match dt {
        DataType::String => match value {
            Json::String(_) => Ok(value.clone()),
            other => Err(format!("expected a string for {name}, got {other}")),
        },
        DataType::Bool => match value {
            Json::Bool(_) => Ok(value.clone()),
            Json::Number(n) if n.as_f64() == Some(0.0) || n.as_f64() == Some(1.0) => {
                Ok(Json::Bool(n.as_f64() == Some(1.0)))
            }
            other => Err(format!("expected a boolean for {name}, got {other}")),
        },
        _ => {
            let n = match value {
                Json::Number(n) => n.clone(),
                Json::String(s) => s
                    .parse::<serde_json::Number>()
                    .map_err(|_| format!("expected a number for {name}, got \"{s}\""))?,
                other => return Err(format!("expected a number for {name}, got {other}")),
            };
            // The same range and type check as a read: a value that would not decode back is refused.
            to_datatype(&Json::Number(n.clone()), dt)?;
            Ok(Json::Number(n))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../../../connectors/aveva-pi/conformance/decode-vectors.json");

    fn datatype(name: &str) -> DataType {
        serde_json::from_value(Json::String(name.into())).unwrap()
    }

    fn json_of(v: &Value) -> Json {
        match v {
            Value::Bool(b) => Json::Bool(*b),
            Value::Number(n) => serde_json::Number::from_f64(*n).map(Json::Number).unwrap(),
            Value::Text(t) => Json::String(t.clone()),
        }
    }

    fn same_number(a: &Json, b: &Json) -> bool {
        match (a.as_f64(), b.as_f64()) {
            (Some(x), Some(y)) => x == y,
            _ => a == b,
        }
    }

    #[test]
    fn golden_vectors() {
        let doc: Json = serde_json::from_str(VECTORS).unwrap();
        let vectors = doc["vectors"].as_array().unwrap();
        assert!(vectors.len() > 40);
        for v in vectors {
            let name = v["name"].as_str().unwrap();
            let mode = if v["mode"] == "raw" { Mode::Raw } else { Mode::Typed };
            let d = decode(&v["value"], Some(datatype(v["datatype"].as_str().unwrap())), mode);
            let e = &v["expect"];
            let quality = if d.quality == Quality::Good { "good" } else { "bad" };
            assert_eq!(quality, e["quality"], "{name}: quality");
            match e.get("value") {
                Some(expected) => {
                    let got = json_of(d.value.as_ref().unwrap_or_else(|| panic!("{name}: no value")));
                    assert!(same_number(&got, expected), "{name}: value {got} != {expected}");
                }
                None => assert!(d.value.is_none() || mode == Mode::Raw, "{name}: unexpected value"),
            }
            assert_eq!(d.error.as_deref(), e.get("error").and_then(Json::as_str), "{name}: error");
            if let Some(raw) = e.get("raw") {
                assert_eq!(d.raw, raw.as_str().unwrap(), "{name}: raw");
            }
            let flags: Vec<&str> = e
                .get("flags")
                .and_then(Json::as_array)
                .map(|a| a.iter().map(|f| f.as_str().unwrap()).collect())
                .unwrap_or_default();
            assert_eq!(d.flags, flags, "{name}: flags");
        }
    }

    #[test]
    fn float_repr_matches_python() {
        for (f, text) in [
            (0.1, "0.1"),
            (2.5, "2.5"),
            (-0.5, "-0.5"),
            (1e300, "1e+300"),
            (1.5e-5, "1.5e-05"),
            (0.0001, "0.0001"),
            (1e16, "1e+16"),
            (1.2345e16, "1.2345e+16"),
            (9007199254740992.0, "9007199254740992.0"),
            (123456.789, "123456.789"),
            (0.10000000149011612, "0.10000000149011612"),
        ] {
            assert_eq!(float_repr(f), text, "{f}");
        }
        assert_eq!(float_text(7.0), "7");
        assert_eq!(float_text(-0.0), "0");
        assert_eq!(float_text(1e16), "1e+16");
    }

    #[test]
    fn writes_are_checked_like_reads() {
        assert_eq!(encode_write(&serde_json::json!(42.5), Some(DataType::Float64)).unwrap(), serde_json::json!(42.5));
        assert!(encode_write(&serde_json::json!(42.5), Some(DataType::Int32)).is_err());
        assert!(encode_write(&serde_json::json!(70000), Some(DataType::Int16)).is_err());
        assert_eq!(encode_write(&serde_json::json!("12"), Some(DataType::Int16)).unwrap(), serde_json::json!(12));
        assert_eq!(encode_write(&serde_json::json!(1), Some(DataType::Bool)).unwrap(), serde_json::json!(true));
        assert!(encode_write(&serde_json::json!(5), Some(DataType::String)).is_err());
    }

    mod properties {
        use super::*;
        use proptest::prelude::*;

        fn arb_json() -> impl Strategy<Value = Json> {
            let leaf = prop_oneof![
                Just(Json::Null),
                any::<bool>().prop_map(Json::Bool),
                any::<i64>().prop_map(Json::from),
                any::<f64>().prop_filter("finite", |f| f.is_finite()).prop_map(Json::from),
                ".{0,8}".prop_map(Json::String),
            ];
            leaf.prop_recursive(3, 24, 4, |inner| {
                prop_oneof![
                    prop::collection::vec(inner.clone(), 0..4).prop_map(Json::Array),
                    prop::collection::btree_map(
                        prop_oneof![Just("Name".to_string()), Just("Value".to_string()), Just("IsSystem".to_string()),
                                    Just("Errors".to_string()), Just("Good".to_string()), ".{0,4}".prop_map(|s| s)],
                        inner,
                        0..4
                    )
                    .prop_map(|m| Json::Object(m.into_iter().collect())),
                ]
            })
        }

        proptest! {
            #[test]
            fn never_panics_and_good_has_a_value(value in arb_json(), good in prop::option::of(any::<bool>()), dt in 0usize..12) {
                let mut pi = serde_json::json!({"Timestamp": "2026-11-02T08:00:00Z", "Value": value});
                if let Some(g) = good {
                    pi["Good"] = Json::Bool(g);
                }
                let d = decode(&pi, Some(crate::config::DATATYPES[dt]), Mode::Typed);
                prop_assert_eq!(d.quality == Quality::Good, d.value.is_some());
                prop_assert_eq!(d.quality == Quality::Bad, d.error.is_some());
                prop_assert!(serde_json::from_str::<Json>(&d.raw).is_ok());
            }

            #[test]
            fn canonical_round_trips(value in arb_json()) {
                let text = canonical(&value);
                let back: Json = serde_json::from_str(&text).unwrap();
                prop_assert_eq!(canonical(&back), text);
            }
        }
    }
}
