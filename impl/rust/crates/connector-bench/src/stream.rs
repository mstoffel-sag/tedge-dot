//! The value streams, defined in `doc/contract/test-vectors/bench/README.md`. The C module
//! implements the same algorithm, and both check `values.json` bit for bit.

use tedge_dot_sdk::{DataType, Value};

/// FNV-1a 64 over `<a>\0<b>`; a zero hash becomes 1, so xorshift never sticks at 0.
pub fn seed(a: &str, b: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in a.bytes().chain([0u8]).chain(b.bytes()) {
        h ^= byte as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    if h == 0 {
        1
    } else {
        h
    }
}

/// xorshift64*.
#[derive(Clone, Debug)]
pub struct Rng(u64);

impl Rng {
    pub fn new(a: &str, b: &str) -> Self {
        Rng(seed(a, b))
    }

    pub fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    /// A draw in [0, 1).
    pub fn draw(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 * (1.0 / (1u64 << 53) as f64)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Model {
    Walk,
    Constant,
    Counter,
}

/// One point's stream: the generator state and the current reading.
#[derive(Clone, Debug)]
pub struct PointStream {
    rng: Rng,
    model: Model,
    step: f64,
    change: f64,
    acc: f64,
    changes: u64,
}

impl PointStream {
    pub fn new(device: &str, point: &str, model: Model, step: f64, change: f64) -> Self {
        PointStream {
            rng: Rng::new(device, point),
            model,
            step,
            change,
            acc: 0.0,
            changes: 0,
        }
    }

    /// Take the next reading.
    pub fn advance(&mut self) {
        let u1 = self.rng.draw();
        if u1 < self.change && self.model != Model::Constant {
            self.changes += 1;
            match self.model {
                Model::Walk => {
                    let u2 = self.rng.draw();
                    self.acc += (2.0 * u2 - 1.0) * self.step;
                }
                Model::Counter => self.acc += 1.0,
                Model::Constant => {}
            }
        }
    }

    /// The current reading as `datatype`.
    pub fn value(&self, datatype: DataType) -> Value {
        match datatype {
            DataType::Int32 => Value::Number(self.acc.round().clamp(i32::MIN as f64, i32::MAX as f64)),
            DataType::Bool => Value::Bool(self.changes % 2 == 1),
            DataType::String => Value::Text(self.changes.to_string()),
            _ => Value::Number(self.acc),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../../../doc/contract/test-vectors/bench/values.json");

    fn datatype(s: &str) -> DataType {
        match s {
            "float64" => DataType::Float64,
            "int32" => DataType::Int32,
            "bool" => DataType::Bool,
            "string" => DataType::String,
            other => panic!("unknown datatype {other}"),
        }
    }

    #[test]
    fn shared_vectors_match_bit_for_bit() {
        let doc: serde_json::Value = serde_json::from_str(VECTORS).unwrap();
        let cases = doc["cases"].as_array().unwrap();
        assert!(!cases.is_empty());
        for case in cases {
            let name = case["name"].as_str().unwrap();
            let model = match case["model"].as_str().unwrap() {
                "walk" => Model::Walk,
                "constant" => Model::Constant,
                "counter" => Model::Counter,
                m => panic!("unknown model {m}"),
            };
            let dt = datatype(case["datatype"].as_str().unwrap());
            let mut s = PointStream::new(
                case["device"].as_str().unwrap(),
                case["point"].as_str().unwrap(),
                model,
                case["step"].as_f64().unwrap(),
                case["change"].as_f64().unwrap(),
            );
            for (n, want) in case["values"].as_array().unwrap().iter().enumerate() {
                s.advance();
                let got = s.value(dt);
                let ok = match (&got, dt) {
                    (Value::Number(v), DataType::Float64) => {
                        format!("0x{:016x}", v.to_bits()) == want.as_str().unwrap()
                    }
                    (Value::Number(v), DataType::Int32) => *v == want.as_i64().unwrap() as f64,
                    (Value::Bool(b), _) => *b == want.as_bool().unwrap(),
                    (Value::Text(t), _) => t == want.as_str().unwrap(),
                    _ => false,
                };
                assert!(ok, "case '{name}' reading {n}: got {got:?}, want {want}");
            }
        }
    }

    #[test]
    fn change_ratio() {
        let mut s = PointStream::new("plc0", "ratio", Model::Walk, 1.0, 0.2);
        let mut last = s.value(DataType::Float64);
        let mut changed = 0;
        for _ in 0..10_000 {
            s.advance();
            let v = s.value(DataType::Float64);
            if v != last {
                changed += 1;
            }
            last = v;
        }
        assert!((1_800..=2_200).contains(&changed), "{changed} changes");
    }

    #[test]
    fn same_seed_same_sequence() {
        let mut a = PointStream::new("plc0", "p", Model::Walk, 1.0, 1.0);
        let mut b = a.clone();
        for _ in 0..100 {
            a.advance();
            b.advance();
            assert_eq!(a.value(DataType::Float64), b.value(DataType::Float64));
        }
    }
}
