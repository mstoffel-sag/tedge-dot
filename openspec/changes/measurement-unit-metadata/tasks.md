# Tasks

## 1. Units state flow

- [x] 1.1 Add `flows/ot-measurement/units-state.toml` + `units-state.js`: subscribe to `te/+/+/+/+/m/+/meta` and record each message's map (empty payload = no units) in `context.mapper` under `ot-measurement-units:<topic>`. Output nothing. Verify with a `flows/test-flows.sh` case: a meta message produces no output.
- [x] 1.2 Confirm both packages ship the new files: `.goreleaser.yaml` and `impl/c/packaging/nfpm.yaml` already copy `flows/ot-measurement/*`, so no manifest change is needed. Verified by those globs and `packaging/check-manifest-parity.sh` (no local package build: nfpm/goreleaser are not installed here; the release build covers it).

## 2. Metadata from ot-measurement

- [x] 2.1 In `flows/ot-measurement/main.js`, derive the meta topic from the measurement topic (default, combine, or a `target_topic` matching `te/+/+/+/+/m/+`; none otherwise). Verify with test-flows cases for the default and a non-measurement `target_topic` (no meta output).
- [x] 2.2 Merge the sample's unit (`"<group>.<series>"` → `{unit}`) into the stored map after the quality, value-type and opt-out checks and before the deprecated filters. Output the full map retained (`mqtt: {retain: true}`) before the measurement, only when the map changed. Publish an empty retained message when the map becomes empty. Verify with test-flows cases: first sample → meta + measurement, same unit again → measurement only, changed unit → meta again, unit removed → meta without the series / empty, opted-out point → no meta, bad quality → no meta, on_change-suppressed sample with a new unit → meta only.
- [x] 2.3 Cover combine mode: two points of one device with units, one combined measurement, one meta holding both keys. Verify with a test-flows case.
- [x] 2.4 Replace the "unit stays in the sample" comment in `main.js` and the units note in `flow.toml` with the new behaviour, and document it in `flows/README.md` (the ot-measurement row and a short "Units" paragraph). Verify the README's example topic/payload matches a test-flows case.

## 3. End to end

- [ ] 3.1 Add a cloud e2e check (`cloud/modbus/tests`) that a point with `unit` arrives in Cumulocity as `{"value":…,"unit":…}`, and run it against a thin-edge.io 2.x device image. Verify the suite passes. *(Test written: `level_f32` has `unit = "m"`, "Measurement Units Reach Cumulocity". Not run yet: needs C8Y_BASEURL/USER/PASSWORD. Verified instead on a live thin-edge.io 2.0.1 device with tedge-dot 0.0.11 and the new flow: Cumulocity measurements carry the units, and the retained metadata survives a mapper restart.)*
- [x] 3.2 Add a release-notes entry (`packaging/release-notes.md`) stating that units now reach Cumulocity on thin-edge.io 2.x, and that hand-published unit metadata can be removed. Verify `openspec validate measurement-unit-metadata --strict` passes.
