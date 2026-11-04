# Reporting-policy test vectors (contract §5.3)

`vectors.json` pins the behaviour of the reporting policy for one point. The Rust SDK
(`impl/rust/crates/sdk/src/report.rs`) and the C SDK (`impl/c/sdk/src/report.c`) both run
every vector in their unit tests. The file is how the two implementations are kept identical
below the conformance suite.

Each vector has these fields:

| Field | Meaning |
| --- | --- |
| `name` | What the vector shows. |
| `policy` | The point's **effective** `report` table, after inheritance, exactly as it would appear in a configuration. |
| `pushed` | `true` for a subscribed (pushed) point, and `false` (the default) for a polled one. Only pushed points ask for heartbeat reads. |
| `steps` | Events in time order. `at` is the monotonic time in milliseconds since the point's state was created. |

A step is one of the following:

| Step | Meaning | Checked |
| --- | --- | --- |
| `{"at": t, "sample": {...}}` | A reading is offered to the policy. | `publish` |
| `{"at": t, "tick": true}` | A pass of the runtime loop. | `publish`, `read` |
| `{"at": t, "no_data": true}` | A heartbeat read returned nothing for the point (Rust `Unsupported` or no sample; C `TDOT_READ_NO_DATA`). | — |
| `{"at": t, "reset": true}` | The point's state is reset: a reload, a device reconnect or an MQTT session restore. | — |

A `sample` has an `id`, which names it in the expectations, and an optional `quality`
(`"good"` by default, or `"bad"` or `"stale"`). It carries **one** of these value fields:

| Field | Meaning |
| --- | --- |
| `num` | A number (`value_repr = "number"`). |
| `nan` | `true` for a NaN number. The envelope carries it as JSON `null`. |
| `bool` | A boolean. |
| `str` | A string (`value_repr = "string"`), including 64-bit integers outside the safe range. |
| `raw` | Hex bytes, for a point with no decoded value (raw mode, or a bad sample). |

A `sample` with a `src` field is **replayed** (§5.3 rule 7): `src` is its source time in Unix
milliseconds, and the policy measures its intervals on it. `at` is still the runtime time it
arrives at. A replayed step's `publish` can name two ids: a held reading the replay released,
then the sample itself.

A step's expectations are:

| Expectation | Meaning |
| --- | --- |
| `publish` | The ids of the samples the policy publishes as a result of the step, in order. The default is `[]`. A held sample is named by its own id, and it is published with its own `ts`. `seq` is the position in the published sequence, so it is gap-free by construction. |
| `read` | Tick steps only: whether the policy asks the runtime for an on-demand heartbeat read of the point. The default is `false`. |
