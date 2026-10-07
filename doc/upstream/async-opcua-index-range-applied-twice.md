# Draft upstream issue: async-opcua server applies a monitored item's IndexRange twice

Target repository: https://github.com/FreeOpcUa/async-opcua (verify the current canonical
repo before filing). Applies to `async-opcua` 0.18.0, server part.

---

**Title:** Server: monitored item with an IndexRange reports `BadIndexRangeNoData` for any
index but 0, because the range is applied to the initial value twice

**Description**

`SimpleNodeManager::create_value_monitored_items` (`node_manager/memory/simple.rs`) reads the
initial value with the item's `item_to_monitor`, which includes its `index_range`, so the value
it gets is already the selected sub-range (a one-element array for `IndexRange = "n"`). It then
passes that value to `set_initial_value`, and `MonitoredItem::notify_data_value`
(`subscriptions/monitored_item.rs`, the `index_range` block) applies the range again, to the
one-element array. Any index other than 0 is then out of range, so the client receives
`BadIndexRangeNoData` with an empty value as the item's first notification. Index 0 works only
by accident.

A Read with the same `IndexRange` returns the correct element.

**Reproduction**

1. Server: a `Double[]` variable holding `[20.0, 21.0, 22.5, 23.0]` in a `SimpleNodeManager`.
2. Client: create a monitored item on it with `index_range = NumericRange::Index(2)`.
3. Expected first notification: `[22.5]`. Actual: status `BadIndexRangeNoData`, value Empty.

**Suggested fix**

Read the initial value without the range in `create_value_monitored_items` (the monitored
item applies it), or skip the range in `notify_data_value` for the initial value.

---

Found by `connector-opcua/tests/structures.rs` (tedge-dot): the subscription test there leaves
array elements out for this reason, and the e2e suite covers pushed array elements against the
asyncua simulator instead.
