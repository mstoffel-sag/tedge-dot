//! Arbitrary bytes as a structured value: the first byte picks one of the paths the shared
//! vectors compile (doc/contract/test-vectors/opcua-struct), or a built-in type, and the rest is
//! the body the connector walks. Decoding must return a value or an error, and never panic or
//! read past the body. Bodies come from servers, so this is external input.
//!
//! The paths are derived from vectors.json at startup. Seed the corpus with the vectors' bodies
//! (the `body` hex of each, decoded, prefixed with the index of its plan) to start from
//! realistic inputs.
#![no_main]

use std::sync::OnceLock;

use connector_opcua::structure::{
    accepted, compile, decode, decode_builtin, parse_path, split_extension_object,
    type_set_from_json, Builtin, Leaf, Plan, TypeSet,
};
use libfuzzer_sys::fuzz_target;

const VECTORS: &str = include_str!("../../../../../../doc/contract/test-vectors/opcua-struct/vectors.json");

struct Cases {
    types: TypeSet,
    namespaces: Vec<String>,
    plans: Vec<Plan>,
}

fn cases() -> &'static Cases {
    static CASES: OnceLock<Cases> = OnceLock::new();
    CASES.get_or_init(|| {
        let v: serde_json::Value = serde_json::from_str(VECTORS).unwrap();
        let types = type_set_from_json(&v).unwrap();
        let namespaces = v["namespaces"]
            .as_array()
            .unwrap()
            .iter()
            .map(|s| s.as_str().unwrap().to_string())
            .collect();
        let plans = v["read"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|c| {
                let path = parse_path(c["path"].as_str()?).ok()?;
                let dt = serde_json::from_value(c["datatype"].clone()).ok()?;
                compile(&types, c["root"].as_str()?, &path, dt).ok()
            })
            .collect();
        Cases { types, namespaces, plans }
    })
}

fuzz_target!(|data: &[u8]| {
    let Some((&pick, body)) = data.split_first() else { return };
    let c = cases();
    let n = c.plans.len() + 25;
    match pick as usize % n {
        i if i < c.plans.len() => {
            let _ = decode(&c.plans[i], &c.types, body, &c.namespaces);
        }
        i => {
            let b = Builtin::from_id((i - c.plans.len() + 1) as u32).unwrap();
            for dt in accepted(&Leaf::Builtin(b)) {
                let _ = decode_builtin(b, body, *dt, &c.namespaces);
            }
        }
    }
    let _ = split_extension_object(body);
});
