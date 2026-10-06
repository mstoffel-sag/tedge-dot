//! Fuzz value mapping (contract §4.3): an arbitrary TOML `map` table must parse or be refused,
//! never panic, and a parsed map must answer every value — read (`apply`) and write (`invert`)
//! — with an output or a reason. Maps arrive from hand-edited files and remote `set-config`
//! commands, and the values they map come straight off the wire.
//!
//! The input is split at the first NUL: the map table as TOML, then the value as text, which is
//! also tried as a number and a bool.

#![no_main]

use libfuzzer_sys::fuzz_target;
use tedge_dot_sdk::map::{parse_number, ValueMap};
use tedge_dot_sdk::model::{DataType, Mode, Value};

const DATATYPES: [DataType; 5] = [
    DataType::Bool,
    DataType::Uint16,
    DataType::Uint64,
    DataType::Float64,
    DataType::String,
];

fuzz_target!(|data: &[u8]| {
    let Ok(text) = std::str::from_utf8(data) else {
        return;
    };
    let (table, value) = text.split_once('\0').unwrap_or((text, ""));
    let Ok(table) = toml::from_str::<toml::Value>(&format!("map = {table}")) else {
        return;
    };
    let Ok(json) = serde_json::to_value(&table["map"]) else {
        return;
    };
    let Ok(Some(map)) = ValueMap::parse(&json) else {
        return;
    };
    let number = parse_number(value).unwrap_or(f64::NAN);
    for dt in DATATYPES {
        let _ = map.check_point(Mode::Typed, Some(dt));
        for v in [Value::Text(value.to_string()), Value::Number(number), Value::Bool(value.len() % 2 == 0)] {
            let _ = map.apply(&v, Some(dt));
        }
        for w in [serde_json::json!(value), serde_json::json!(number.is_finite().then_some(number)), serde_json::json!(true)] {
            let _ = map.invert(&w, Some(dt));
        }
    }
});
