## 1. Baseline and vectors

- [x] 1.1 Add `frame:500ms` to `native-push-jitter` in `bench/scenarios.toml`, run it with the current code for both builds, and record the before lines (messages/s, mapper CPU, values per message) in design.md's Performance table
- [x] 1.2 Update the vector "a repeated point starts a new message in the same window" to the layer rule: a repeat goes into the next layer, and both layers publish at the end of the window
- [x] 1.3 Add vectors: readings A, B, A, C give layer 0 {A, B, C} then layer 1 {A}; three changes of one point give three layers in order; polled passes in one frame keep their read times
- [x] 1.4 Add vectors for ordering: a layer whose first time is not later than the previous layer's is stamped 1 ms after it, within a frame and across frames; a split layer's parts share its time (a unit test in each build, `split_frame_layer_parts_share_its_time`: the `split` vector form carries no times)
- [x] 1.5 Add a vector for the cap: 65 readings of one point in a window publish 64 layers at once, then a new frame in the same window holds the 65th
- [x] 1.6 Edit `vectors.json` directly (it is the source; its generator was never committed), and update `doc/contract/test-vectors/measurements/README.md` if a step or field changes

## 2. Rust batcher

- [x] 2.1 In `measure.rs`, replace `DeviceTable::in_open` with a per-point count of values in the open frame, and hold a vector of layers per (topic, policy) for `frame` keys
- [x] 2.2 In `Batcher::add`, put a frame reading into layer `count[k]`, creating it with its time per D2, and publish all layers and start a new frame when the reading would open layer 64
- [x] 2.3 Keep the last layer time per (topic, policy) across frames, and stamp each new layer `max(first time, last + 1 ms)`; split parts keep their layer's time
- [x] 2.4 Publish layers in order from `due` and `flush_all`, resetting the counts of their points; leave `"time"` and `"sample"` paths unchanged
- [x] 2.5 Run the vectors and the Rust workspace tests; add unit tests for the cap and the ordering across frames where a vector cannot express them

## 3. C batcher

- [x] 3.1 In `measure.c`, replace `in_open` with a per-point count and give each `frame` slot a small array of layers, growing up to 64
- [x] 3.2 Implement the layer placement, the cap and the layer-time rule as in the Rust batcher
- [x] 3.3 Publish layers in order from the due and flush paths, resetting counts
- [x] 3.4 Run `ctest` (the `measure-vectors` test included) and check that both builds pass every vector

## 4. Contract, docs and e2e

- [x] 4.1 Update §5.5's grouping table and text in `doc/contract/ot-connector-contract.md` for layers, strictly increasing times and the cap; set the version to 0.2.1 and update the connector specs that state the version
- [x] 4.2 Update the `frame` section of `doc/measurement-output.md`: layers, the 1 ms move under jitter, the latency of a repeat, and that a `min_interval` of at least the window prevents repeats
- [x] 4.3 Change the `frame` examples from `"100ms"` to `"500ms"` (D7): the grouping table, the `frame` example and the descriptor example in `doc/measurement-output.md`, and the device example in the contract
- [x] 4.4 Add `changes/frame-layers.md` with the behavior change for existing `frame` users
- [x] 4.5 Add an OPC UA e2e test: a pushed node that changes faster than a 500 ms frame publishes each change once, in time order, in fewer messages than changes; run it with both builds

## 5. Performance

- [x] 5.1 Run `native-push-jitter` with `frame:100ms` and `frame:500ms` for both builds after the change, three times each, and fill design.md's Performance table with before and after lines from the same host
- [x] 5.2 State in design.md whether each target was met; if not, explain why from the measured numbers
- [x] 5.3 Run `just lint`, the Rust workspace tests, `ctest`, the flow tests and `openspec validate frame-layers --strict`
