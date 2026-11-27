# Namespace file test vectors

Files for [`namespace-file.schema.json`](../../schemas/namespace-file.schema.json) and for readers
of namespace files ([doc/namespace-files.md](../../../namespace-files.md)), shown decompressed.

| Directory | Schema | Reader |
| --- | --- | --- |
| `valid/` | accepts | accepts |
| `invalid/` | rejects | rejects |
| `refused/` | accepts | rejects: a rule the schema cannot express |

`refused/` covers an unknown `version` (2), a node listed before its parent, and a node id that
appears twice.

The conformance crate checks the schema verdicts (`cargo test -p ot-conformance --test
namespace_file_vectors`); the OT connectors plugin checks the reader verdicts
(`ui/src/shared/namespace-file.spec.ts`). opcua-scan's tests validate its own output against the
schema.
