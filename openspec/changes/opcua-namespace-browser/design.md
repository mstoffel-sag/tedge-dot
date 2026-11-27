## Context

See proposal.md for the motivation. Relevant current state:

- **opcua-scan** (fork `mstoffel-sag/opcua-discovery`, branch `feat/tedge-dot-point-library`)
  already connects with a tedge-dot connector file (`--tedge-dot-config <file> --device <name>`),
  maps nodes to tedge-dot points (data types, structure fields, enums, methods, `subscribe =
  false`) and writes or merges point libraries with a `namespaces` table. Its JSON export
  (`NodeRecord`) has neither parent links nor proposed points.
- **tedge-dot keeps no browse code.** Discovery stays in opcua-scan; tedge-dot defines formats and
  the Cumulocity glue. This change keeps that line: tedge-dot gets a format, an operation shim and
  UI, not an OPC UA browser.
- **Operation shims** (`operations/`) map Cumulocity operations to thin-edge commands.
  `c8y_CallMethod` (#130) completes its operation with a custom fragment by writing it through the
  mapper's Cumulocity proxy (`127.0.0.1:8001/c8y/...`), because the template `output` drops custom
  fragments.
- **Child devices** are registered by `ot-registration` as `te/device/<device>//`. Device names are
  unique on a gateway, since each owns that topic.
- **OT connectors plugin** (`ui/`): plain TypeScript logic in `src/shared/` with Jest tests,
  components in Angular signals (no zone.js), comment-preserving TOML writes
  (`toml-write.ts`), library resolution (`libraries.ts`), `@angular/cdk` available.
- **Namespace tables**: libraries and devices may carry `namespaces = [...]`; node ids use
  `ns=<index>` relative to that table and the connector translates them to the server's indexes
  (OPC UA connector spec §3.9).

## Goals / Non-Goals

**Goals:**
- One set of point-mapping rules, in opcua-scan, used by the TUI, the CLI and the plugin.
- A namespace file that stands alone: readable without the server, the gateway or the scan
  parameters.
- A picker that stays responsive at 200 000 nodes in a normal browser.

**Non-Goals:**
- Live browsing from the browser.
- Diffing two scans of a server.
- Protocols other than OPC UA. The file format keeps protocol-specific parts in the proposed
  points and node classes; a second protocol would get its own `format` value.
- Installing opcua-scan through tedge-dot's packages.

## Decisions

### D1. The gateway runs opcua-scan; the browser never talks to the server

The gateway reaches the server and has its credentials and certificates. Running opcua-scan there
with `--tedge-dot-config` keeps secrets on the gateway and reuses the connector's settings.

*Alternatives:* a connector `browse` verb (puts discovery into every connector and the C and Rust
SDKs, against the "no browse code in tedge-dot" rule); live browsing through operations (one round
trip per expand, minutes per level on slow links, nothing to work with offline).

### D2. Points are proposed by opcua-scan and stored in the file

opcua-scan gets `--output tedge-dot-namespace`, which writes every visited node plus, for mappable
nodes, the point tables its library writer would produce, or a `skipped` reason. The plugin takes
these as defaults and only edits fields.

*Alternative:* the plugin maps raw attributes itself. Rejected: structure fields, enum cases and
method inputs need `DataTypeDefinition` and `InputArguments` reads the browser cannot make, and the
TypeScript copy would drift from the Rust one.

### D3. One gzip JSON document, parents first, node ids with `nsu=`

A single document is easy to validate with a JSON schema, to upload and to download. Parents-first
order lets a reader build the tree in one pass. `nsu=` ids make the file independent of the
server's index order; the plugin converts them to `ns=<index>` of the target's `namespaces` table,
appending URIs the table lacks.

The schema lives at `doc/contract/schemas/namespace-file.schema.json`, with test vectors under
`doc/contract/test-vectors/namespace-file/`. opcua-scan's tests validate against a vendored copy,
and the conformance crate checks the vectors. The plugin does not run the schema: it checks what
it relies on (format, version, node classes, parents first, unique ids, points or a reason) with
its own messages, against the same vectors.

*Alternatives:* NDJSON (streams, but needs a header line convention and is harder to validate);
a binary format (smaller, but needs a decoder in the browser and in tests).

### D4. Stored as an inventory binary, linked to the scanned device

The scan script uploads with `POST /inventory/binaries` (multipart, `object` part carrying `name`
and `type = "tedge-dot-namespace"`), then sets the `tedgeDotNamespace` metadata fragment with
`PUT /inventory/managedObjects/<id>` (binary creation keeps only a few fields), and adds the binary
as a child addition of the scanned device's managed object, found by its external id
(`c8y_Serial`, `<gateway>:device:<device>`), or of the gateway when the device is not registered.

The plugin lists with `type eq 'tedge-dot-namespace'` and reads the metadata from the managed
objects, so listing never downloads a binary.

*Alternatives:* the configuration repository (`c8y_ConfigurationDump`) is for files that go to a
gateway, which a namespace file never does; event binaries (`c8y_UploadConfigFile` style) expire
with event retention and can't carry list metadata.

### D5. A `command` shim on the gateway, naming the device

`operations/c8y_ScanAddressSpace` is a `command` template, like `c8y_ModbusDevice`: the c8y mapper
runs `/usr/bin/ot-scan-address-space` with the operation's fragment and the operation id, reports
EXECUTING, applies the template's `timeout`, and completes the operation from the exit code (the
last line on stderr is the failure reason). The operation goes to the **gateway** and names the
device in `device`.

The operation cannot go to the OPC UA child device through a thin-edge command: tedge-agent runs
workflows only for its own entity, and the flows runtime that handles child-device commands
(`ot-command-forward`) cannot run programs or make HTTP calls. Templates in
`/etc/tedge/operations/c8y/` are announced on the gateway by the mapper, so nothing in
`ot-registration` changes.

The script:

1. finds the connector file that defines the device: the `*.toml` files under
   `/etc/tedge/plugins/ot/` with `[connector] protocol = "opcua"` and a `[[device]]` of that name
   (device names are unique on a gateway, since each owns `te/device/<name>`). None or more than
   one is a failure.
2. checks `opcua-scan --version` against the minimum version that has the format;
3. runs `opcua-scan browse --tedge-dot-config <file> --device <device> --output tedge-dot-namespace`
   with the scope options and `--pki-dir` set to the connector's PKI directory, writing to a
   temporary directory;
4. compresses, checks the size limit, uploads (D4), and writes `binaryId` and `nodes` into the
   operation's `c8y_ScanAddressSpace` fragment through the mapper proxy, as `c8y_CallMethod`'s
   design intends;
5. removes the temporary files on every path.

The script's logic that needs no gateway (argument building, connector-file lookup, metadata) has
an offline mode for shell tests, as `c8y-fieldbus-import --translate` does.

*Alternative:* a thin-edge workflow on the gateway. It would need its own capability and a
template that maps to it, for no gain over the mapper's own `command` handling.

### D6. Picker logic is plain TypeScript; the tree is virtual

New modules under `ui/src/shared/`:
- `namespace-file.ts`: decompress (`DecompressionStream('gzip')`, plain JSON accepted too), the
  reader checks, the metadata, and an index (`id → node`, `parent → children` sorted by name with
  numbers in order, lower-cased search keys, tree order). opcua-scan browses concurrently, so the
  file's order is not the server's; the index sorts.
- `namespace-points.ts`: checked set, subtree checks, per-point changes, id clashes; point tables
  in the target's namespace numbering; matching a target's points (by `node_id` plus `field`, or
  `object_id` plus `method_id`, through each side's table) and writing the result with
  `toml-write.ts` in two steps (changes and removals, then additions), so comments stay.
- `namespace-scan.ts`: the operation the scan dialog sends, and the devices it offers.

Parsing and indexing run on the main thread: the unit test checks, indexes and searches a
200 000-node file in about a second on a laptop (the test allows 10 s for shared CI runners), so a worker is not needed yet; it is the first
step if large files make the page stall. The tree uses `@angular/cdk` virtual scrolling over the
flattened visible rows; expansion state lives in signals. Search walks the index, not the DOM.

The picker never saves. It hands the resulting text to the existing editor (new entry, or the
opened entry with changes), so validation, saving and sending stay as they are.

### D7. Scope and limits

The operation defaults to `type = Variable,Method` with the objects above them, which drops type
definitions and keeps files small. The time limit (10 minutes) is the template's `timeout`.
The script reads the opcua-scan path (`OPCUA_SCAN`, default `opcua-scan` on `PATH`) and the size
limit (`TEDGE_DOT_SCAN_MAX_BYTES`, default 50 MB) from its environment, which a systemd drop-in for
the mapper can set; no new configuration file. The plugin refuses to open a file above 200 MB
uncompressed and says so.

## Risks / Trade-offs

- [The scan opens a second session next to the connector's; servers with a session limit may
  refuse it] → the failure names the server's status code; the guide says to stop the connector
  or raise the limit for the scan.
- [A large scan loads the server] → opcua-scan's worker count and page size stay at their
  defaults, and the dialog suggests a scope; scans run only on request.
- [Sharing the connector's PKI directory: opcua-scan could write into it] → pass it read-only
  (task to verify opcua-scan does not generate certificates when one is present), or copy the
  trust list to a temporary directory.
- [opcua-scan is a separate release] → the script checks its version and the failure says which
  version is needed; the plugin still accepts uploaded namespace files without it.
- [Cumulocity binary size limits on some tenants] → the 50 MB default stays below the usual
  limit; the failure names it.
- [Node ids change between scans or server versions] → merge matches on node ids; points whose
  node is missing are kept and listed (spec: "Point outside the scan"), never silently dropped.

## Migration Plan

Additive. Older gateways do not announce `c8y_ScanAddressSpace`, so the plugin offers no scan for
them; uploading namespace files works with any gateway. Rollback is removing the shim and the
plugin version; stored namespace files are plain binaries and can be deleted from the list.

## Open Questions

- Whether opcua-scan ships as a thin-edge software package from the opcua-discovery release, and
  under which name.
