# `bench` module value vectors

The synthetic `bench` connector module (Rust crate `connector-bench`, C file
`impl/c/connectors/bench/connector_bench.c`) generates its readings from the configuration
alone. Both builds must produce the same values from the same configuration, so the algorithm
is defined here and `values.json` pins its output. The Rust and C unit tests both check every
case for bit-exact equality.

`generate.py` is an independent implementation of the algorithm that writes `values.json`:

```sh
python3 doc/contract/test-vectors/bench/generate.py > doc/contract/test-vectors/bench/values.json
```

## Algorithm

Each point has its own stream. All integer arithmetic is unsigned 64-bit and wraps.

**Seed.** FNV-1a 64 over the UTF-8 bytes of `<device>`, one zero byte, and `<point id>`:
start at `0xcbf29ce484222325`, and for each byte XOR it in, then multiply by
`0x100000001b3`. A seed of 0 becomes 1. The state `x` starts at the seed.

**Next.** xorshift64*:

```
x ^= x >> 12
x ^= x << 25
x ^= x >> 27
return x * 0x2545f4914f6cdd1d
```

**Draw.** `u = (next() >> 11) · 2⁻⁵³`, a double in [0, 1).

**Per reading.** The point keeps an accumulator `acc` (a double, starting at 0.0) and a change
counter `changes` (starting at 0). For each reading, in this order:

1. Take a draw `u₁`. The reading changes the value when `u₁ < change` and the model is not
   `"constant"`. The draw is taken for every model, so the stream advances the same way.
2. If it changes, add 1 to `changes`, then apply the model:
   - `"walk"`: take a draw `u₂` and set `acc = acc + ((2.0 · u₂ − 1.0) · step)`. Evaluate
     exactly in that order, as separate IEEE 754 double operations with no fused multiply-add.
   - `"counter"`: `acc = acc + 1.0`.
3. The reading's value depends on the point's datatype:
   - `float64`: `acc`.
   - `int32`: `acc` rounded half away from zero, clamped to [−2³¹, 2³¹ − 1].
   - `bool`: `true` when `changes` is odd.
   - `string`: `changes` in decimal, with no sign or leading zeros.

The first reading is the result of the first pass through these steps, not the start value.

**Defaults.** `model = "walk"`, `step = 1.0`, `change = 1.0`. `change` lies in [0, 1], and
`step` is finite and not negative.

## `values.json`

```json
{
  "cases": [
    {
      "name": "...",
      "device": "plc0",
      "point": "p0",
      "model": "walk",
      "datatype": "float64",
      "step": 1.0,
      "change": 1.0,
      "values": ["0x...", ...]
    }
  ]
}
```

Each case lists the first 1,000 readings. `float64` values are the IEEE 754 bit pattern as a
`0x`-prefixed 16-digit hex string, so no decimal conversion can hide a difference in the last
bit. `int32` values are JSON integers, `bool` values JSON booleans and `string` values JSON
strings.

Timestamps (`ts_jitter`, `ts_resolution`, `publish_interval`) depend on the wall clock and
are not part of the vectors.
