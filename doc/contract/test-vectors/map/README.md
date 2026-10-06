# Value-mapping test vectors (contract §4.3)

`vectors.json` pins the behaviour of a point's value map. The Rust SDK
(`impl/rust/crates/sdk/src/map.rs`) and the C SDK (`impl/c/tests/map.c`, over
`impl/c/sdk/src/map.c`) both run every vector in their unit tests. This file is how the two
implementations are kept identical below the conformance suite, messages included.

`maps` names the maps the vectors share. Each map is written as it would appear in a point's
`map` table. The C runner renders it as TOML and loads it through the config loader. An integer
with 16 or more digits stays exact on that path.

## `read`

A read vector maps one decoded value:

| Field | Meaning |
| --- | --- |
| `name` | What the vector shows. |
| `map` | The name of a map in `maps`, or a map table inline. |
| `datatype` | The point's datatype. |
| `transform` | Optional. The point's transform, applied to `in` before the map. |
| `in` | The decoded value. |
| `out` | The mapped value. Absent when `error` is set. |
| `error` | The sample's error text, without the `point <id>: ` prefix the runtime adds. |

`in` and `out` carry one of these fields:

| Field | Meaning |
| --- | --- |
| `num` | A number. |
| `nan` | `true` for a NaN number. |
| `bool` | A boolean. |
| `str` | A string, including a 64-bit integer outside the safe range (§4.1). |

## `write`

A write vector maps one written value back. Both runners go through the SDK's write path, so the
inverse transform and the raw bypass apply as they do for the `write` verb.

| Field | Meaning |
| --- | --- |
| `name`, `map`, `datatype`, `transform` | As for `read`. |
| `value` | The written value, as a write request's JSON `value`. |
| `raw` | A raw (hex) write in place of `value`. The C runtime has no raw writes and skips these vectors. |
| `device` | The value the connector is asked to write. Numbers compare by value. |
| `device_raw` | For a `raw` write: the hex the connector receives. |
| `error` | The write's failure reason, without the `point <id>: ` prefix. |

## `invalid`

An invalid vector is a map the loader refuses:

| Field | Meaning |
| --- | --- |
| `map` | The map table, inline. |
| `datatype`, `mode` | The point's datatype, and `"raw"` for a raw-mode point. |
| `error` | The message, without the device and point prefix. |
