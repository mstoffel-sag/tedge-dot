## Context

Measurement output (contract §5.5) and the `ot-measurement` flow name a series with a group and a
series. The group is both the thin-edge measurement type (topic `te/device/<d>///m/<group>`) and
the fragment in the body. thin-edge.io already accepts several fragments in one measurement and
maps the topic type to the Cumulocity `type`; the units metadata on `<topic>/meta` is keyed
`"<fragment>.<series>"`, so it already holds several fragments per topic.

Both SDKs must publish byte-identical messages and refuse the same configurations with the same
messages. The shared test vectors pin this.

## Decisions

### D1. Naming resolution

At each level L (point `meta.measurement`, device `measurement`, `[connector] measurement`):

- `type_L` = L.`type`, else L.`group`
- `fragment_L` = L.`fragment`, else L.`group`
- `series_L` = L.`series`

Only non-empty strings count. With `point_separator` set and the point id containing it at a
position that leaves both sides non-empty, the id splits once into `prefix` and `suffix`.

- **type** = `type_point` ‖ `type_device` ‖ `type_connector` ‖ `prefix` ‖ protocol id
- **fragment** = `fragment_point` ‖ `fragment_device` ‖ `fragment_connector` ‖ `prefix` ‖ **type**
- **series** = `series_point` ‖ `series_device` ‖ `series_connector` ‖ `suffix` ‖ point id

The prefix feeds the type as well as the fragment. Without it a configuration that relies on
`point_separator` today would move its measurements from `m/<prefix>` to `m/<protocol>`. With a
type set, the prefix names the fragment: `type = "Combined"` and the point `c8y_Battery.level`
give `m/Combined` with `"c8y_Battery": {"level": …}`.

`group` at a level sets both keys at that level, so a point's `meta.measurement.group` wins over a
connector `type`. That is what `group` meant before.

The `ot-measurement` flow resolves the same way, without the device level (a sample carries the
point's meta, not the device's tables).

### D2. Keys and validation

- `[connector] measurement`: `type`, `fragment`, `series`, `group`, `point_separator`,
  `include_boolean`.
- Device `measurement` (new): `type`, `fragment`, `series`, `group`.
- `type`, `fragment`, `series`, `group` and `point_separator` must be strings:
  `measurement.<key> must be a string`, prefixed with the place (`[connector] ` or
  `device '<name>': `).
- Unknown keys: `the measurement of [connector]`, `the measurement of device '<name>'`.

### D3. Output values

`OUTPUTS` = `"single"`, `"group"`, `"samples"`, `"measurements"`, in that order (the refusal lists
them in this order). `"samples"` means `"single"` and `"measurements"` means `"group"`. Both builds
log a warning at load for a deprecated value:
`output = "<old>" is deprecated: use "<new>"`. The descriptor reports `"single"` or `"group"`.

### D4. The `grouping` table

`grouping` replaces `batch` at connector, device and point level and in point libraries. Its keys
are `by`, `window`, `every` and `aggregate`. `by` takes the values `group_by` took.

`batch` stays accepted. Its keys are checked as written (`group_by`, `window`, `every`,
`aggregate`, unknown keys reported as `the batch of …`), then the loader rewrites it as `grouping`
(`group_by` → `by`) before anything else reads it, library points included. A table level that
sets both `batch` and `grouping` is refused:
`batch and grouping cannot both be set (batch is the deprecated name of grouping)`, prefixed with
the place like the other value errors. A deprecated `batch` logs a warning once per load:
`batch is deprecated: use grouping (group_by is now by)`.

The messages change from `batch.` to `grouping.` and from `group_by` to `by`:

| Before | After |
| --- | --- |
| `batch must be a table` | `grouping must be a table` |
| `batch.group_by must be "time", "frame" or "sample"` | `grouping.by must be "time", "frame" or "sample"` |
| `batch.<key> does not apply to group_by = "<p>"` | `grouping.<key> does not apply to by = "<p>"` |
| `batch.<key> must be a duration from <range>` | `grouping.<key> must be a duration from <range>` |
| `batch.aggregate must be …` | `grouping.aggregate must be …` |
| `batch.window is required with group_by = "frame"` | `grouping.window is required with by = "frame"` |
| `batch.every is required with group_by = "sample"` | `grouping.every is required with by = "sample"` |
| `batch.aggregate cannot be combined with report: …` | `grouping.aggregate cannot be combined with report: …` |
| `batch does not apply to a call_method point` | `grouping does not apply to a call_method point` |

### D5. Messages with several fragments

A message key is (device, type, grouping policy) and, under `"time"`, the time; the topic is
`te/device/<device>///m/<type>`. Within a topic, fragments get an index in the order their first
point appears in the device's configuration. A message body is

```
{"time":"<ts>","<f0>":{<series>},"<f1>":{<series>},…}
```

with only the fragments that hold a series in this message, in index order, and each fragment's
series in the order they were added. The size estimate is the exact length of that text plus 12
bytes per series:

```
36 + Σ over fragments in the message (len(quoted fragment) + 3 + len(series text)) + (fragments − 1) + 12 × series
```

For one fragment this equals today's `39 + len(quoted group) + len(series text) + 12 × series`, so
every existing split point stays where it is. A series that would push the estimate over 15,000
bytes publishes the message first (when it holds at least one series) and starts an empty one with
the same time.

"A point appears at most once in a message" is unchanged.

### D6. Name clashes

The aggregate clash check (§5.5) keys series by (type, fragment, series):
`device '<d>': points '<a>' and '<b>' both publish the series '<s>' in the fragment '<f>' of the type '<t>'`.

### D7. Descriptor

`outputs[]` = `{ "device", "output": "single"|"group", "grouping": { "by", … } }`, the merged
connector and device `grouping` with `by` filled in (`"time"`). Present under the same condition
as before: grouped output, or any connector or device `grouping` (or `batch`) table.

### D8. Units

Unchanged format: one retained message per topic (type), keyed `"<fragment>.<series>"`, sorted by
key.

## Risks

- **Status consumers** reading `outputs[].batch` break. The docs site's tools are updated in this
  change; nothing else in the repository reads it.
- **Multi-fragment units in the c8y mapper** are not covered by an e2e test yet; the mapper's units
  flow looks keys up as `<fragment>.<series>`, which is the documented format.
