# Measurement-output test vectors (contract §5.5)

`vectors.json` pins what a connector in measurement output publishes for given readings: the
measurement messages, their grouping and splits, the units metadata, and which readings still
publish a sample. The Rust SDK (`impl/rust/crates/sdk/src/measure.rs`) and the C SDK
(`impl/c/sdk/src/measure.c`, run by `impl/c/tests/measure.c`) both run every case in their unit
tests. The file is how the two implementations are kept identical below the end-to-end suites.

Each case has these fields:

| Field | Meaning |
| --- | --- |
| `name` | What the case shows. |
| `config` | A complete connector configuration (TOML), loaded by the build's own loader. Its `[connector]` sets `output = "measurements"`. |
| `generate` | Optional. Points appended to the configuration: `count` points of `datatype`, with ids from the `id` pattern, where `{i:04}` is the point's index with four digits. |
| `steps` | Events in order. `at` is the device clock in milliseconds since the point tables were built. |
| `messages` | The messages published, in order: `topic`, `retain`, and `payload` as JSON (`""` for an empty, clearing payload). Compared as JSON, not as text. |
| `routing` | One entry per `read` step: `sample` (the sample envelope is published) and `measured` (the reading also became a series, so the envelope carries `"measured": true`). |
| `split` | Instead of `messages` and `routing`, for a case too large to list: the `topic` of every message and the number of `series` in each, in order. |

A step is one of the following:

| Step | Meaning |
| --- | --- |
| `{"at": t, "read": {...}}` | A reading the report policy let through is offered. |
| `{"at": t, "read_all": {...}}` | The same reading of every point of a device, in configuration order. |
| `{"at": t, "pass": true}` | The end of a runtime pass: messages grouped by time are published. |
| `{"at": t, "due": true}` | Frames whose window ended and samples that are due are published. The sample time is `2026-01-01T00:00:00.000Z` plus `at`. |
| `{"at": t, "stale": {"device": d, "stale": b}}` | The device's link went down (`true`) or came back (`false`). |

A reading has a `device`, a `point`, a `value` (a number, a boolean or a string), a `ts` (RFC 3339
with milliseconds) and an optional `quality` (`"good"` by default, `"bad"` or `"stale"`). The
runner builds the sample as the module would return it for the configured point: a `raw` point
carries no value.

Numbers in a payload are written as an integer when they have no fraction and are below 1e15,
and otherwise in the shortest form that reads back as the same double, laid out as Rust's `ryu`
prints it (`1e20`, `1.5e-7`, `0.0001`). Both builds format them alike, so their size estimates and
split points agree.

The `split` case checks the size budget: 2,000 series of about 65 bytes each split into messages
of 191 series, the most that keeps the thin-edge size plus 12 bytes per series under 15,000 bytes.
