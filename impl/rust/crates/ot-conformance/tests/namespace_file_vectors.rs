//! The namespace-file schema's verdicts on its test vectors
//! (doc/contract/test-vectors/namespace-file): `valid/` and `refused/` are well-formed,
//! `invalid/` is not. `refused/` breaks reader rules the schema cannot express, which the
//! OT connectors plugin checks.

use ot_conformance::layer1::{Kind, Schemas};
use std::path::PathBuf;

fn vectors(dir: &str) -> Vec<(String, serde_json::Value)> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../../doc/contract/test-vectors/namespace-file")
        .join(dir);
    let mut out: Vec<_> = std::fs::read_dir(&root)
        .unwrap_or_else(|e| panic!("{}: {e}", root.display()))
        .map(|entry| entry.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "json"))
        .map(|p| {
            let text = std::fs::read_to_string(&p).unwrap();
            let name = p.file_name().unwrap().to_string_lossy().into_owned();
            (name, serde_json::from_str(&text).unwrap())
        })
        .collect();
    out.sort_by(|a, b| a.0.cmp(&b.0));
    assert!(!out.is_empty(), "no vectors in {}", root.display());
    out
}

#[test]
fn valid_and_refused_vectors_match_the_schema() {
    let schemas = Schemas::load().unwrap();
    for dir in ["valid", "refused"] {
        for (name, value) in vectors(dir) {
            schemas
                .validate(Kind::NamespaceFile, &value)
                .unwrap_or_else(|e| panic!("{dir}/{name}: {e}"));
        }
    }
}

#[test]
fn invalid_vectors_do_not_match_the_schema() {
    let schemas = Schemas::load().unwrap();
    for (name, value) in vectors("invalid") {
        assert!(
            schemas.validate(Kind::NamespaceFile, &value).is_err(),
            "invalid/{name} was accepted"
        );
    }
}
