## 1. Namespace file format

- [x] 1.1 Add `doc/contract/schemas/namespace-file.schema.json` (version 1: `format`, `version`, `server`, `namespaces`, `scan`, `nodes` with parent links, attributes, `points`, `skipped`, `truncated`), referencing `point-library.schema.json` for the point tables. Verify it is valid JSON Schema and that the conformance tool's schema tests pass.
- [x] 1.2 Add test vectors under `doc/contract/test-vectors/namespace-file/`: a minimal file, a structure variable, an enum, a method, an array (skipped), a truncated branch, invalid files, and `refused/` files the schema accepts but readers refuse (`version = 2`, child before parent, duplicate id). Verify a schema test accepts the valid ones and rejects the invalid ones.
- [x] 1.3 Write `doc/namespace-files.md`: the format, the metadata fragment `tedgeDotNamespace`, the `tedge-dot-namespace` binary type, what is never stored (secrets, values unless previewed). Place it in `site/docs-map.yaml` and verify the site build (`npm run build` in `site/`) passes.

## 2. opcua-scan output (external: ~/git/opcua-discovery, fork branch)

- [x] 2.1 Add `--output tedge-dot-namespace` to `browse`: every visited node with `nsu=` ids, parent id, attributes, and the proposed point tables from the existing library writer or a `skipped` reason; write the namespace table and scan block. Verify with an integration test against the simulator that the output validates against `namespace-file.schema.json` (vendored from 1.1).
- [x] 2.2 Record truncation (depth, `--max-children`, browse errors) on the node. Verify with a test that caps children and checks `truncated = "max_children"`.
- [x] 2.3 Check that `--pki-dir` pointed at a populated tedge-dot PKI directory writes nothing into it; fix or document a copy step. Verify by scanning with the directory mounted read-only.
- [x] 2.4 Document the format in opcua-scan's README, push to the fork, and note the minimum version that has it. Verify the README example runs as written.

## 3. Gateway: scan operation

- [x] 3.1 Add the `command` template `operations/c8y_ScanAddressSpace` (gateway operation, 600 s timeout, fragment and operation id to the script). Verify the template parses: `tedge` lists `c8y_ScanAddressSpace` among the gateway's supported operations in the bench container.
- [x] 3.2 Add the `ot-scan-address-space` script (design D5): device lookup in the OPC UA connector files, opcua-scan version check, scan with scope options and the connector's PKI dir, gzip, size check, upload with metadata, child addition, operation update with `binaryId` and `nodes`, cleanup on every path. Verify with shell tests for device lookup (none, one, two matches), argument building and the size check.
- [x] 3.3 Make every failure in the spec's "Failures" requirement produce its reason. Verify with tests that stub opcua-scan (missing, too old, connection error, empty result) and the proxy (upload error).
- [x] 3.4 No `ot-registration` change (design D5): verify `flows/test-flows.sh` still passes.
- [x] 3.5 Add the template and script to the OPC UA packages and check manifest parity (`packaging/check-manifest-parity.sh`). Verify the built package installs them.
- [x] 3.6 Update `operations/README.md` (mapping table, payload shape) and write the scan guide section in `doc/namespace-files.md` (install opcua-scan, run a scan, limits, the second-session caveat). Verify the documented operation payloads match the spec scenarios.
- [ ] 3.7 Add a bench e2e test: scan the OPC UA simulator device from Cumulocity, check the operation result and that the binary validates against the schema. Verify it passes in CI's bench job.

## 4. Plugin: namespace file logic (ui/src/shared)

- [x] 4.1 ~~Copy the schema in `npm run sync`~~: not needed, the plugin checks namespace files itself against the same vectors (design D3).
- [x] 4.2 `namespace-file.ts`: decompress (gzip or plain), check `format`/`version`, build the index (by id, children by parent, search keys), the 200 MB refusal. Unit tests with the 1.2 vectors and a generated 200 000-node file indexed under 2 s in Jest.
- [x] 4.3 Queries and metadata: `namespaceFilesQuery()` and the metadata read from a managed object and from an uploaded file. Unit tests in `queries.spec.ts` and a new spec file.
- [x] 4.4 Selection (in `namespace-points.ts`): check node or subtree (only nodes with points), counts, per-point overrides applied to one or many, id clash detection. Unit tests for the "Selecting nodes" and "Configuring points" scenarios.
- [x] 4.5 `namespace-points.ts`: selection to point tables in the target's `namespaces` numbering (append missing URIs), new library, merge into a library or a device's inline points by node address, keep points outside the scan, keep comments via `toml-write.ts`. Unit tests for the "Result of the picker" and "Editing an existing target" scenarios, and that each result passes `validateLibraryFile`/`validateConnectorFile`.

## 5. Plugin: UI

- [ ] 5.1 Namespaces list on the OT connectors page and on the gateway's tab (child devices' files): columns, *Download* as `.json.gz`, *Delete* with confirmation, permission-based actions. Verify in a local `npm start` session against a tenant with a namespace file.
- [x] 5.2 *Upload namespace file* dialog: check, refuse with reason, store as binary with metadata. Verify with an opcua-scan file and with a plain `--output json` file (refused).
- [x] 5.3 *Scan address space* dialog: device choice limited to devices announcing the operation, scope fields, operation progress, new row on success, failure reason. Verify against the bench gateway.
- [ ] 5.4 Namespace tree (main thread, design D6) with `@angular/cdk` virtual scroll: expand on demand, search, filters, node details, truncated and unmappable markers. Verify responsiveness with the 200 000-node file in the browser.
- [x] 5.5 Point panel: proposed point fields, edit one or many, clash markers, checked count; target choice (new library, existing library, device of a connector entry) that opens the editor with the result unsaved. Verify the scenarios of "Result of the picker" and "Editing an existing target" by hand.
- [x] 5.6 Update `ui/README.md` (what you get, requirements: opcua-scan on the gateway) and the plugin description in `package.json`. Verify `npm test`, `npm run typecheck` and `npm run build` pass.

## 6. Documentation and release

- [x] 6.1 Update `doc/c8y-connector-files.md` with "Building points from a namespace scan" and link `doc/namespace-files.md`; update the site guides that cover the OT connectors plugin and OPC UA point libraries. Verify the site build passes and the new page is reachable from the navigation.
- [x] 6.2 Add the release highlight `changes/opcua-namespace-browser.md` (scan from Cumulocity, pick nodes, opcua-scan needed on the gateway). Verify it follows `changes/README.md`.

## 7. Integration

- [ ] 7.1 End to end on the bench: scan `plc` from the plugin, create a library with five points from the tree, send the connector file and library to the gateway, and see the five measurements in Cumulocity. Record the result in the PR.
- [x] 7.2 `openspec validate opcua-namespace-browser --strict` passes.

Verified without a tenant: the scan script against the local OPC UA simulator with opcua-scan
0.2.0 and a fake Cumulocity proxy (661 nodes uploaded, metadata, child addition and operation
result written), and the plugin's reader building a valid 310-point library from that file.
In a tenant (dev server against a real tenant, 2026-11-27): the Namespaces list and delete on the
page, the card on a gateway's tab, uploading the simulator scan and refusing a plain export, the
picker with a 661-node file (search, subtree checks, bulk poll and measurement type, id clash
blocking, a new library and points added to a device of an existing connector file, both opened
unsaved with comments kept), and the scan dialog without a gateway that can scan.

On a gateway (thin-edge.io 2.1.0, Ubuntu x86_64, test install without root in
/etc/tedge/plugins/tedge-dot-scan): the mapper announced c8y_ScanAddressSpace without a restart;
a scan of the Cumulocity OPC UA demo server from the plugin produced a 736-node file with 323
points, linked to the device, and the operation carried `binaryId` and `nodes` (after the fix
that sends `status` with the operation update, which Cumulocity requires). Picking the device's
connector file as target checked the 7 points it already had for scanned nodes and kept the 3
array-element points the scan does not propose; a variable and a method were added.
One file per device (added after the test, on request): verified on the same gateway; three
earlier scans of Pump01 were replaced by the new one.
Open: 3.7 (bench test); 5.1 *Download* (not clicked); 5.4 with a 200 000-node file in the
browser; 7.1 sending the result and seeing measurements.

## Workflow follow-up

- opcua-scan's namespace output is proposed upstream in reubenmiller/opcua-discovery#2: once it is merged and released, point the install guide (doc/namespace-files.md) at that release.
- Archive the change after the PR is merged.
