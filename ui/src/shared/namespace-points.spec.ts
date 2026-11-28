import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { parseToml } from '../generated/config-builder';
import { type NamespaceFile, checkNamespaceFile, indexNamespace } from './namespace-file';
import {
  addressKey,
  changePoints,
  checkState,
  checkedPoints,
  clashingIds,
  deviceTarget,
  initialSelection,
  libraryTarget,
  matchTarget,
  newLibraryText,
  pointKey,
  proposedPoint,
  resultPoints,
  toggleNode,
  translateNodeId,
  writeDevice,
  writeLibrary,
} from './namespace-points';
import { validateConnectorFile, validateLibraryFile } from './validation';

const VECTORS = join(__dirname, '../../../doc/contract/test-vectors/namespace-file/valid');
const vector = (name: string): NamespaceFile => JSON.parse(readFileSync(join(VECTORS, name), 'utf8'));

/** Line1 with Temperature (one point), Motor (a structure, two points) and Reset (a method). */
function lineFile(): NamespaceFile {
  const minimal = vector('minimal.json');
  const motor = vector('structure.json').nodes[2];
  const reset = vector('method.json').nodes[2];
  return checkNamespaceFile({ ...minimal, nodes: [...minimal.nodes, motor, reset] });
}

const L = 'nsu=urn:acme:line;s=Line1';
const TEMP = pointKey(`${L}.Temperature`, 0);
const CURRENT = pointKey(`${L}.Motor`, 0);
const VOLTAGE = pointKey(`${L}.Motor`, 1);
const RESET = pointKey(`${L}.Reset`, 0);

describe('selecting points', () => {
  const index = indexNamespace(lineFile());

  it('checks and unchecks a subtree: its variables, not its methods', () => {
    let selection = toggleNode(index, new Map(), L);
    expect([...selection.keys()].sort()).toEqual([CURRENT, TEMP, VOLTAGE].sort());
    expect(checkState(index, selection, 'i=85')).toBe('all');
    selection = toggleNode(index, selection, `${L}.Motor`);
    expect(checkState(index, selection, L)).toBe('some');
    expect(checkState(index, selection, `${L}.Motor`)).toBe('none');
    selection = toggleNode(index, toggleNode(index, selection, L), L);
    expect(selection.size).toBe(0);
  });

  it('takes a method only when it is ticked itself', () => {
    let selection = toggleNode(index, new Map(), `${L}.Reset`);
    expect([...selection.keys()]).toEqual([RESET]);
    expect(checkState(index, selection, L)).toBe('some');
    selection = toggleNode(index, selection, L);
    expect([...selection.keys()].sort()).toEqual([CURRENT, RESET, TEMP, VOLTAGE].sort());
    expect(checkState(index, selection, L)).toBe('all');
    // Unticking the object drops its variables and leaves the method the user picked.
    expect([...toggleNode(index, selection, L).keys()]).toEqual([RESET]);
  });

  it('takes the methods of an object that has nothing else', () => {
    const file = lineFile();
    const reset = file.nodes.find(n => n.display_name === 'Reset')!;
    const ix = indexNamespace(
      checkNamespaceFile({
        ...file,
        nodes: [
          ...file.nodes,
          { id: 'nsu=urn:acme:line;s=Service', parent: 'i=85', browse_name: 'Service', display_name: 'Service', node_class: 'Object' },
          { ...reset, id: 'nsu=urn:acme:line;s=Service.Reset', parent: 'nsu=urn:acme:line;s=Service', points: [{ ...reset.points![0], id: 'service_reset' }] },
        ],
      })
    );
    expect([...toggleNode(ix, new Map(), 'nsu=urn:acme:line;s=Service').keys()]).toEqual([pointKey('nsu=urn:acme:line;s=Service.Reset', 0)]);
  });

  it('changes only the id and name of a method point', () => {
    let selection = toggleNode(index, new Map(), L);
    selection = toggleNode(index, selection, `${L}.Reset`);
    selection = changePoints(selection, [...selection.keys()], {
      mode: 'poll', interval: '5s', access: 'read', unit: 'A', measurementType: 'Pump', name: 'Line reset',
    });
    const reset = checkedPoints(index, selection).find(p => p.key === RESET)!.point;
    expect(reset).toEqual({ ...proposedPoint(index, RESET), name: 'Line reset' });
    // The variables took the change, and the result passes the editor's check.
    expect(checkedPoints(index, selection).find(p => p.key === TEMP)!.point).toMatchObject({ subscribe: false, poll_interval: '5s', access: 'read' });
    const result = resultPoints(index, selection, { points: [] }, matchTarget(index, { points: [] }));
    expect(validateLibraryFile(newLibraryText(result.points, result.namespaces)).fields).toEqual([]);
  });

  it('writes a new library of the given protocol', () => {
    expect(parseToml(newLibraryText([], ['http://opcfoundation.org/UA/'], { protocol: 'canopen' }))['library']['protocol']).toBe('canopen');
    expect(parseToml(newLibraryText([], ['http://opcfoundation.org/UA/']))['library']['protocol']).toBe('opcua');
  });

  it('the editor refuses value settings on a method point', () => {
    const text = newLibraryText([{ ...proposedPoint(index, RESET), access: 'read', poll_interval: '5s' }], index.file.namespaces);
    expect(validateLibraryFile(text).fields.map(f => f.message)).toContain('applies only to a call_method point');
    const method = newLibraryText([{ ...proposedPoint(index, RESET), poll_interval: '5s' }], index.file.namespaces);
    expect(validateLibraryFile(method).fields.map(f => f.message)).toContain('does not apply to a call_method point');
  });

  it('changes several points at once', () => {
    let selection = toggleNode(index, new Map(), `${L}.Motor`);
    selection = changePoints(selection, [CURRENT, VOLTAGE], { mode: 'poll', interval: '5s' });
    const points = checkedPoints(index, selection).map(p => p.point);
    expect(points.map(p => [p['subscribe'], p['poll_interval']])).toEqual([
      [false, '5s'],
      [false, '5s'],
    ]);
    selection = changePoints(selection, [CURRENT], { measurementType: 'Motor', measurementFragment: 'c8y_Motor', unit: 'A' });
    expect(checkedPoints(index, selection)[0].point).toMatchObject({ unit: 'A', meta: { measurement: { type: 'Motor', fragment: 'c8y_Motor' } } });
    selection = changePoints(selection, [CURRENT], { unit: '' });
    expect(checkedPoints(index, selection)[0].point['unit']).toBeUndefined();
  });

  it('writes proposed points in the usual key order', () => {
    const selection = toggleNode(index, new Map(), `${L}.Temperature`);
    expect(Object.keys(checkedPoints(index, selection)[0].point)).toEqual(['id', 'name', 'datatype', 'access', 'unit', 'address']);
  });

  it('drops a description that only repeats the name', () => {
    const file = lineFile();
    const temp = file.nodes.find(n => n.display_name === 'Temperature')!;
    temp.points = [{ ...temp.points![0], description: 'Temperature' }];
    const ix = indexNamespace(file);
    expect(checkedPoints(ix, toggleNode(ix, new Map(), temp.id))[0].point['description']).toBeUndefined();
    temp.points = [{ ...temp.points![0], description: 'Line temperature at the outlet' }];
    const ix2 = indexNamespace(file);
    expect(checkedPoints(ix2, toggleNode(ix2, new Map(), temp.id))[0].point['description']).toBe('Line temperature at the outlet');
  });

  it('finds clashing ids', () => {
    let selection = toggleNode(index, new Map(), L);
    selection = changePoints(selection, [CURRENT, VOLTAGE], { id: 'motor' });
    expect([...clashingIds(checkedPoints(index, selection).map(p => p.point))]).toEqual(['motor']);
  });
});

describe('namespace numbering', () => {
  it('translates ids and appends namespaces the target lacks', () => {
    const to = ['http://opcfoundation.org/UA/', 'urn:other'];
    expect(translateNodeId('ns=2;s=X', ['http://opcfoundation.org/UA/', 'urn:a', 'urn:b'], to)).toBe('ns=2;s=X');
    expect(to).toEqual(['http://opcfoundation.org/UA/', 'urn:other', 'urn:b']);
    expect(translateNodeId('ns=1;s=Y', ['http://opcfoundation.org/UA/', 'urn:other'], to)).toBe('ns=1;s=Y');
    expect(translateNodeId('i=85', [], to)).toBe('i=85');
    expect(() => translateNodeId('ns=7;s=Z', ['http://opcfoundation.org/UA/'], to)).toThrow(/index 7/);
  });

  it('matches addresses across tables', () => {
    const a = addressKey({ address: { node_id: 'ns=2;s=X', field: 'F' } }, ['u', 'v', 'urn:x']);
    const b = addressKey({ address: { node_id: 'ns=5;s=X', field: 'F' } }, ['u', 'v', 'w', 'y', 'z', 'urn:x']);
    expect(a).toBe(b);
  });
});

describe('the result', () => {
  const index = indexNamespace(lineFile());

  it('is a valid new library', () => {
    const selection = toggleNode(index, toggleNode(index, new Map(), L), `${L}.Reset`);
    const result = resultPoints(index, selection, { points: [] }, matchTarget(index, { points: [] }));
    const text = newLibraryText(result.points, result.namespaces, { type: 'acme-line' });
    expect(validateLibraryFile(text).fields).toEqual([]);
    const library = parseToml(text);
    expect(library['library']).toMatchObject({ protocol: 'opcua', type: 'acme-line', namespaces: index.file.namespaces });
    // In tree order: Line1's children by name.
    expect(library['point'].map((p: any) => p.id)).toEqual(['line1_motor_current', 'line1_motor_voltage', 'line1_reset', 'line1_temperature']);
  });

  const libraryText = `# ACME line, hand-written
[library]
protocol = "opcua"
namespaces = ["http://opcfoundation.org/UA/", "urn:acme:line"]

# The line temperature
[[point]]
id = "temp"
datatype = "float64"
address = { node_id = "ns=1;s=Line1.Temperature" }

# Not in any scan
[[point]]
id = "legacy"
datatype = "int32"
address = { node_id = "ns=1;s=Old" }
`;

  it('merges into a library: keeps comments and points outside the scan', () => {
    const target = libraryTarget(parseToml(libraryText));
    const match = matchTarget(index, target);
    expect(match.existing.get(TEMP)).toBe(0);
    expect(match.outside).toEqual([1]);
    let selection = initialSelection(match);
    expect([...selection.keys()]).toEqual([TEMP]);
    selection = toggleNode(index, selection, `${L}.Motor`);
    selection = changePoints(selection, [TEMP], { unit: 'K' });
    const result = resultPoints(index, selection, target, match);
    expect(result).toMatchObject({ added: 2, removed: 0, outside: 1 });
    const { text, keptLayout } = writeLibrary(libraryText, result);
    expect(keptLayout).toBe(true);
    expect(text).toContain('# ACME line, hand-written');
    expect(text).toContain('# Not in any scan');
    expect(validateLibraryFile(text).fields).toEqual([]);
    const library = parseToml(text);
    // The file numbers urn:acme:line 2, this library 1.
    expect(library['point'].map((p: any) => [p.id, p.address.node_id, p.unit])).toEqual([
      ['temp', 'ns=1;s=Line1.Temperature', 'K'],
      ['legacy', 'ns=1;s=Old', undefined],
      ['line1_motor_current', 'ns=1;s=Line1.Motor', undefined],
      ['line1_motor_voltage', 'ns=1;s=Line1.Motor', undefined],
    ]);
  });

  it('removes an unchecked point and adds new ones in one go', () => {
    const target = libraryTarget(parseToml(libraryText));
    const match = matchTarget(index, target);
    const selection = toggleNode(index, new Map(), `${L}.Reset`);
    const result = resultPoints(index, selection, target, match);
    expect(result).toMatchObject({ added: 1, removed: 1 });
    const { text } = writeLibrary(libraryText, result);
    expect(text).toContain('# Not in any scan');
    expect(parseToml(text)['point'].map((p: any) => p.id)).toEqual(['legacy', 'line1_reset']);
  });

  it('adds points to a device of a connector file', () => {
    const connector = `[connector]
protocol = "opcua"

# The line PLC
[[device]]
name = "plc-7"
protocol_address = { endpoint = "opc.tcp://plc-7:4840" }
`;
    const device = parseToml(connector)['device'][0];
    const target = deviceTarget(device);
    const match = matchTarget(index, target);
    const selection = toggleNode(index, new Map(), `${L}.Temperature`);
    const result = resultPoints(index, selection, target, match);
    const { text, keptLayout } = writeDevice(connector, 0, result);
    expect(keptLayout).toBe(true);
    expect(text).toContain('# The line PLC');
    expect(validateConnectorFile(text).fields).toEqual([]);
    const written = parseToml(text)['device'][0];
    expect(written['protocol_address']['namespaces']).toEqual(index.file.namespaces);
    expect(written['point'].map((p: any) => p.address.node_id)).toEqual(['ns=2;s=Line1.Temperature']);
  });
});
