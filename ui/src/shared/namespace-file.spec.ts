import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync } from 'node:zlib';

import {
  type NamespaceFile,
  type NamespaceMetadata,
  NamespaceFileError,
  ancestors,
  checkNamespaceFile,
  indexNamespace,
  metadataOf,
  predecessors,
  readNamespaceFile,
  subtree,
  treeRows,
  visibleNodes,
  whyNoPoints,
} from './namespace-file';

const VECTORS = join(__dirname, '../../../doc/contract/test-vectors/namespace-file');
const vector = (dir: string, name: string) => JSON.parse(readFileSync(join(VECTORS, dir, name), 'utf8'));
const vectors = (dir: string) => readdirSync(join(VECTORS, dir)).filter(f => f.endsWith('.json'));
const bytes = (text: string) => new TextEncoder().encode(text);

/** A generated file: `objects` objects under Objects, each with `perObject` double variables. */
export function bigFile(objects: number, perObject: number): NamespaceFile {
  const base = vector('valid', 'minimal.json') as NamespaceFile;
  const nodes: NamespaceFile['nodes'] = [{ id: 'i=85', browse_name: 'Objects', display_name: 'Objects', node_class: 'Object' }];
  for (let o = 0; o < objects; o++) {
    const obj = `nsu=urn:acme:line;s=Line${o}`;
    nodes.push({ id: obj, parent: 'i=85', browse_name: `Line${o}`, display_name: `Line${o}`, node_class: 'Object' });
    for (let v = 0; v < perObject; v++) {
      const id = `${obj}.Temp${v}`;
      nodes.push({
        id,
        parent: obj,
        browse_name: `Temp${v}`,
        display_name: `Temp${v}`,
        node_class: 'Variable',
        datatype: 'Double',
        points: [{ id: `line${o}_temp${v}`, name: `Temp${v}`, datatype: 'float64', address: { node_id: `ns=2;s=Line${o}.Temp${v}` } }],
      });
    }
  }
  return { ...base, nodes };
}

describe('namespace files', () => {
  it('accepts every valid vector', () => {
    for (const name of vectors('valid')) {
      expect(() => checkNamespaceFile(vector('valid', name))).not.toThrow();
    }
  });

  it('refuses every invalid and refused vector with a reason', () => {
    for (const dir of ['invalid', 'refused']) {
      for (const name of vectors(dir)) {
        expect(() => checkNamespaceFile(vector(dir, name))).toThrow(NamespaceFileError);
      }
    }
  });

  it('names what is wrong', () => {
    expect(() => checkNamespaceFile(vector('invalid', 'plain-export.json'))).toThrow(/--output tedge-dot-namespace/);
    expect(() => checkNamespaceFile(vector('refused', 'version-2.json'))).toThrow(/version 2/);
    expect(() => checkNamespaceFile(vector('refused', 'child-before-parent.json'))).toThrow(/before its parent/);
    expect(() => checkNamespaceFile(vector('refused', 'duplicate-id.json'))).toThrow(/appears twice/);
  });

  it('reads plain and gzip-compressed files', async () => {
    const text = readFileSync(join(VECTORS, 'valid', 'minimal.json'), 'utf8');
    expect((await readNamespaceFile(bytes(text))).nodes).toHaveLength(3);
    expect((await readNamespaceFile(new Uint8Array(gzipSync(text)))).nodes).toHaveLength(3);
    await expect(readNamespaceFile(bytes('nope'))).rejects.toThrow(/not JSON/);
  });

  it('builds the metadata the list shows', () => {
    const meta = metadataOf(vector('valid', 'structure.json'), { gateway: 'gw-1' });
    expect(meta).toMatchObject({ gateway: 'gw-1', device: 'plc-7', connectorFile: 'opcua', nodes: 3, points: 2, truncated: false });
    expect(metadataOf(vector('valid', 'truncated.json')).truncated).toBe(true);
  });

  it('finds the files a new one replaces', () => {
    const meta = (m: Partial<NamespaceMetadata>) => ({ ...metadataOf(vector('valid', 'minimal.json')), ...m });
    const stored = [
      { id: 'new', meta: meta({ gateway: 'gw-1', device: 'plc-7' }) },
      { id: 'same-device', meta: meta({ gateway: 'gw-1', device: 'plc-7' }) },
      { id: 'other-device', meta: meta({ gateway: 'gw-1', device: 'plc-8' }) },
      { id: 'other-gateway', meta: meta({ gateway: 'gw-2', device: 'plc-7' }) },
      { id: 'upload', meta: meta({ gateway: undefined, device: 'plc-7' }) },
      { id: 'upload-other-endpoint', meta: meta({ gateway: undefined, device: 'plc-7', endpoint: 'opc.tcp://other:4840' }) },
      { id: 'no-metadata' },
    ];
    expect(predecessors(stored, stored[0].meta!, 'new').map(e => e.id)).toEqual(['same-device']);
    const upload = meta({ gateway: undefined, device: 'plc-7' });
    expect(predecessors(stored, upload, 'newer-upload').map(e => e.id)).toEqual(['upload']);
  });

  it('indexes the tree', () => {
    const index = indexNamespace(vector('valid', 'minimal.json'));
    expect(index.roots).toEqual(['i=85']);
    expect(index.children.get('i=85')).toEqual(['nsu=urn:acme:line;s=Line1']);
    expect(index.path.get('nsu=urn:acme:line;s=Line1.Temperature')).toBe('Objects/Line1/Temperature');
    expect(subtree(index, 'i=85')).toHaveLength(3);
    expect(ancestors(index, 'nsu=urn:acme:line;s=Line1.Temperature')).toEqual(['i=85', 'nsu=urn:acme:line;s=Line1']);
  });

  it('sorts children by name, numbers in order', () => {
    const index = indexNamespace(bigFile(1, 12));
    expect(index.children.get('nsu=urn:acme:line;s=Line0')!.slice(0, 3).map(id => index.byId.get(id)!.display_name)).toEqual(['Temp0', 'Temp1', 'Temp2']);
    expect(index.order.length).toBe(index.file.nodes.length);
  });

  it('shows expanded nodes only, and opens what a filter finds', () => {
    const index = indexNamespace(vector('valid', 'minimal.json'));
    expect(treeRows(index, new Set()).map(r => r.id)).toEqual(['i=85']);
    expect(treeRows(index, new Set(['i=85'])).map(r => r.depth)).toEqual([0, 1]);
    const visible = visibleNodes(index, { text: 'temp' })!;
    expect(treeRows(index, new Set(), visible).map(r => r.id)).toEqual(['i=85', 'nsu=urn:acme:line;s=Line1', 'nsu=urn:acme:line;s=Line1.Temperature']);
    expect(treeRows(index, new Set(), visible, new Set(['i=85']))).toHaveLength(1);
    expect(visibleNodes(index, {})).toBeUndefined();
    expect(visibleNodes(index, { withPoints: true })!.size).toBe(3);
  });

  it('says why a node has no points', () => {
    const file: NamespaceFile = vector('valid', 'skipped-array.json');
    const array = file.nodes.find(n => n.display_name === 'History')!;
    expect(whyNoPoints(array)).toBe('arrays are not supported');
    expect(whyNoPoints(file.nodes[0])).toBeUndefined();
  });

  it('indexes and searches 200 000 nodes quickly', () => {
    const file = bigFile(1000, 199);
    expect(file.nodes.length).toBeGreaterThan(200_000);
    const started = Date.now();
    const checked = checkNamespaceFile(JSON.parse(JSON.stringify(file)));
    const index = indexNamespace(checked);
    const visible = visibleNodes(index, { text: 'line999/temp19' })!;
    const rows = treeRows(index, new Set(), visible);
    // Generous for shared CI runners; it takes about a second on a laptop.
    expect(Date.now() - started).toBeLessThan(10000);
    expect(rows.map(r => index.byId.get(r.id)!.display_name)).toEqual(['Objects', 'Line999', 'Temp19', ...Array.from({ length: 9 }, (_, i) => `Temp${190 + i}`)]);
  });
});
