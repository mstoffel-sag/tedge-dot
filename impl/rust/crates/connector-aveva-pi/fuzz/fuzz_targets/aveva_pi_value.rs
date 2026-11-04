//! Fuzz the PI value decoder: whatever a PI Web API server (or something pretending to be
//! one) sends as a value.
//!
//!   cd impl/rust/crates/connector-aveva-pi && cargo +nightly fuzz run aveva_pi_value
//!
//! No input may panic, in any mode or datatype; a good sample has a value and a bad one an
//! error; the raw text is valid JSON, and writing it again gives the same text.
#![no_main]

use connector_aveva_pi::config::DATATYPES;
use connector_aveva_pi::value::{canonical, decode};
use libfuzzer_sys::fuzz_target;
use tedge_dot_sdk::{Mode, Quality};

fuzz_target!(|data: &[u8]| {
    let Some((&selector, body)) = data.split_first() else { return };
    let Ok(pi) = serde_json::from_slice::<serde_json::Value>(body) else { return };
    let datatype = DATATYPES[selector as usize % DATATYPES.len()];
    let mode = if selector & 0x80 != 0 { Mode::Raw } else { Mode::Typed };
    let d = decode(&pi, Some(datatype), mode);
    assert_eq!(d.quality == Quality::Bad, d.error.is_some());
    if mode == Mode::Typed {
        assert_eq!(d.quality == Quality::Good, d.value.is_some());
    }
    let back: serde_json::Value = serde_json::from_str(&d.raw).expect("raw is JSON");
    assert_eq!(canonical(&back), d.raw);
});
