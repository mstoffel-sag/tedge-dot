## 1. Contract and vectors

- [ ] 1.1 Contract §3.3 (keys), §5.5 (output names, naming, grouping table, several fragments, size estimate) and §7 (`outputs`)
- [ ] 1.2 Config schema: `output` values, `measurement` keys at connector and device level, `grouping` with `batch` deprecated; status schema `outputs`
- [ ] 1.3 Measurement test vectors: new names in most cases, the aliases in a few, and cases for several fragments, a device-level type, the separator under a type, units of several fragments and a split across fragments

## 2. Rust SDK

- [ ] 2.1 `library.rs`: keys, `batch` → `grouping` rewrite (library points included), both-set refusal, deprecation warnings, device `measurement`
- [ ] 2.2 `measure.rs`: `Output` names, naming resolution (D1), messages with several fragments and the size estimate (D5), clash check (D6), descriptor (D7), new messages (D4)
- [ ] 2.3 `config.rs`, `descriptor.rs`, `runtime.rs`, bench: field renames
- [ ] 2.4 Unit tests pass, vectors included

## 3. C SDK

- [ ] 3.1 `config.c`/`config.h`: the same keys, rewrite, refusals and messages
- [ ] 3.2 `measure.c`/`measure.h`, `runtime.c`: the same naming, messages and descriptor
- [ ] 3.3 `impl/c/tests`: config and measure tests pass, vectors included

## 4. Flows and integrations

- [ ] 4.1 `ot-measurement`: `type` and `fragment` params, `meta.measurement.type`/`.fragment`, `group` as alias; `flows/test-flows.sh`
- [ ] 4.2 Cloud Fieldbus import: `measurementMapping.type` → `meta.measurement.type` and `.fragment`
- [ ] 4.3 Connector example configs, packaged configs, demo configs, bench scenarios and the aveva-pi plugin converter use the new names

## 5. Docs

- [ ] 5.1 `doc/measurement-output.md`, connector specs, migration guides, flows README
- [ ] 5.2 Docs site: guides, config builder, grouping playground and their tests
- [ ] 5.3 Release highlight in `changes/measurement-type-fragment.md`
