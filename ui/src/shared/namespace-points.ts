/**
 * From a namespace file to points: which proposed points are checked, what the user changed on
 * them, and how they land in a point library or in a device's points, in the target's namespace
 * numbering, keeping the target's other points, settings and comments.
 *
 * A proposed point is addressed by its key `<node id>#<n>`: the n-th point of that node (a
 * structure proposes one per field). Points are matched with a target's by their address, read
 * through each side's namespace table, so `ns=2;s=X` in the file and `ns=4;s=X` in a library
 * with another table are the same node.
 *
 * Plain TypeScript, without Angular, so it is unit-tested as it is.
 */
import { emitToml, parseToml, type Config } from '../generated/config-builder';
import { type NamespaceIndex, subtree } from './namespace-file';
import { emitLibraryToml, writeKeepingLayout } from './toml-write';

const OPCUA_NS = 'http://opcfoundation.org/UA/';

/** What the user may change on a checked point. Unset fields keep the point's own value. */
export interface PointChange {
  id?: string;
  name?: string;
  unit?: string;
  access?: 'read' | 'write' | 'read_write';
  /** Subscribe (pushed by the server) or poll; with an interval in the tedge-dot duration form. */
  mode?: 'subscribe' | 'poll';
  interval?: string;
  measurementType?: string;
  measurementFragment?: string;
}

/** Checked point keys and the changes made to them. */
export type Selection = ReadonlyMap<string, PointChange>;

export function pointKey(nodeId: string, n: number): string {
  return `${nodeId}#${n}`;
}

export function nodeOfKey(key: string): string {
  return key.slice(0, key.lastIndexOf('#'));
}

/** The keys of the points proposed for one node. */
export function pointKeysOf(index: NamespaceIndex, nodeId: string): string[] {
  return (index.byId.get(nodeId)?.points ?? []).map((_, n) => pointKey(nodeId, n));
}

/** The keys of every point proposed at or below a node. */
export function pointKeysUnder(index: NamespaceIndex, nodeId: string): string[] {
  return subtree(index, nodeId).flatMap(id => pointKeysOf(index, id));
}

/**
 * The points ticking a node takes: a variable's or method's own; for an object, the points of the
 * variables below it, not its methods, which are actions to pick one by one. An object with
 * nothing but methods below it takes those.
 */
export function pointKeysTaken(index: NamespaceIndex, nodeId: string): string[] {
  const nodeClass = index.byId.get(nodeId)?.node_class;
  if (nodeClass === 'Variable' || nodeClass === 'Method') {
    return pointKeysOf(index, nodeId);
  }
  const below = subtree(index, nodeId);
  const variables = below.filter(id => index.byId.get(id)?.node_class !== 'Method').flatMap(id => pointKeysOf(index, id));
  return variables.length ? variables : below.flatMap(id => pointKeysOf(index, id));
}

/** Whether a point is a method (`access = "call_method"`), which takes no value settings. */
export function isMethodPoint(point: Config | undefined): boolean {
  return point?.['access'] === 'call_method';
}

/** The order point keys are written in, as the docs and packaged files write them. */
const KEY_ORDER = ['id', 'name', 'description', 'datatype', 'access', 'unit', 'subscribe', 'poll_interval', 'sampling_interval', 'address', 'map', 'meta'];

/**
 * A point with its keys in the order files are written in; others keep their order after those.
 * opcua-scan's JSON lists keys alphabetically, which would put `address` first.
 */
export function orderPointKeys(point: Config): Config {
  const known = KEY_ORDER.filter(k => k in point);
  const rest = Object.keys(point).filter(k => !KEY_ORDER.includes(k));
  return Object.fromEntries([...known, ...rest].map(k => [k, point[k]]));
}

/** The point a key stands for, as proposed, in the file's namespace numbering. */
export function proposedPoint(index: NamespaceIndex, key: string): Config | undefined {
  const node = index.byId.get(nodeOfKey(key));
  const n = Number(key.slice(key.lastIndexOf('#') + 1));
  const point = node?.points?.[n];
  if (!point) {
    return undefined;
  }
  // Some servers set a node's Description to its name, which says nothing about the point.
  const { description, ...rest } = point;
  return orderPointKeys(description === undefined || description === point['name'] ? rest : point);
}

export type CheckState = 'none' | 'some' | 'all';

/**
 * The node's checkbox: all when every point ticking it takes is checked, none when no point at or
 * below it is, else some (also when only methods below an object are checked).
 */
export function checkState(index: NamespaceIndex, selection: Selection, nodeId: string): CheckState {
  if (!pointKeysUnder(index, nodeId).some(k => selection.has(k))) {
    return 'none';
  }
  const taken = pointKeysTaken(index, nodeId);
  return taken.length && taken.every(k => selection.has(k)) ? 'all' : 'some';
}

/** Checks the points ticking a node takes, or unchecks them when all are checked already. */
export function toggleNode(index: NamespaceIndex, selection: Selection, nodeId: string): Map<string, PointChange> {
  const next = new Map(selection);
  const keys = pointKeysTaken(index, nodeId);
  if (keys.length && keys.every(k => next.has(k))) {
    keys.forEach(k => next.delete(k));
  } else {
    keys.forEach(k => next.has(k) || next.set(k, {}));
  }
  return next;
}

/** Applies a change to the given checked points. Empty strings clear a field back to the point's own. */
export function changePoints(selection: Selection, keys: Iterable<string>, change: PointChange): Map<string, PointChange> {
  const next = new Map(selection);
  for (const key of keys) {
    const known = next.get(key);
    if (!known) {
      continue;
    }
    const merged: PointChange = { ...known, ...change };
    for (const field of Object.keys(change) as (keyof PointChange)[]) {
      if (change[field] === '') {
        delete merged[field];
      }
    }
    next.set(key, merged);
  }
  return next;
}

/**
 * A point with a change applied. A method point takes only its id and name: the value settings
 * (unit, access, subscribe or poll, intervals, measurement naming) do not apply to an action, and
 * the connector refuses a method point that declares them.
 */
export function applyChange(point: Config, change: PointChange | undefined): Config {
  const out: Config = structuredClone(point);
  if (!change) {
    return out;
  }
  if (change.id) out['id'] = change.id;
  if (change.name !== undefined) out['name'] = change.name;
  if (isMethodPoint(point)) {
    return out;
  }
  if (change.unit !== undefined) {
    if (change.unit) out['unit'] = change.unit;
    else delete out['unit'];
  }
  if (change.access) out['access'] = change.access;
  if (change.mode === 'poll') {
    out['subscribe'] = false;
    delete out['sampling_interval'];
    if (change.interval) out['poll_interval'] = change.interval;
  } else if (change.mode === 'subscribe') {
    delete out['subscribe'];
    delete out['poll_interval'];
    if (change.interval) out['sampling_interval'] = change.interval;
  }
  if (change.measurementType || change.measurementFragment) {
    const meta: Config = { ...(out['meta'] ?? {}) };
    const measurement: Config = typeof meta['measurement'] === 'object' && meta['measurement'] ? { ...meta['measurement'] } : {};
    if (change.measurementType) measurement['type'] = change.measurementType;
    if (change.measurementFragment) measurement['fragment'] = change.measurementFragment;
    meta['measurement'] = measurement;
    out['meta'] = meta;
  }
  return out;
}

// --- namespace numbering -------------------------------------------------------------------

const NS_ID = /^ns=(\d+);(.+)$/s;

/** An id without its namespace index: the namespace URI (undefined for 0) and the rest. */
export function splitNodeId(id: string, table: readonly string[] | undefined): { uri?: string; rest: string } {
  const m = NS_ID.exec(id);
  if (!m) {
    return { rest: id };
  }
  const n = Number(m[1]);
  if (n === 0) {
    return { rest: m[2] };
  }
  // Without a table the index is the server's: keep it as it is.
  return { uri: table ? table[n] ?? `#${n}` : `#${n}`, rest: m[2] };
}

/** An id in another table's numbering; a namespace the table lacks is appended to `to`. */
export function translateNodeId(id: string, from: readonly string[], to: string[]): string {
  const m = NS_ID.exec(id);
  if (!m || m[1] === '0') {
    return id;
  }
  const uri = from[Number(m[1])];
  if (uri === undefined) {
    throw new Error(`${id}: namespace index ${m[1]} is not in the namespace table (${from.length} entries)`);
  }
  const rest = m[2];
  if (uri === OPCUA_NS) {
    return `ns=0;${rest}`;
  }
  let n = to.indexOf(uri);
  if (n < 0) {
    to.push(uri);
    n = to.length - 1;
  }
  return `ns=${n};${rest}`;
}

const ID_FIELDS = ['node_id', 'object_id', 'method_id'];

/** A point with its address ids in another table's numbering. */
export function translatePoint(point: Config, from: readonly string[], to: string[]): Config {
  const out: Config = structuredClone(point);
  const address = out['address'];
  if (address && typeof address === 'object') {
    for (const field of ID_FIELDS) {
      if (typeof address[field] === 'string') {
        address[field] = translateNodeId(address[field], from, to);
      }
    }
  }
  return out;
}

/** What a point addresses, independent of namespace numbering; undefined without an address. */
export function addressKey(point: Config, table: readonly string[] | undefined): string | undefined {
  const address = point['address'];
  if (!address || typeof address !== 'object') {
    return undefined;
  }
  const part = (field: string) => {
    if (typeof address[field] !== 'string') {
      return '';
    }
    const { uri, rest } = splitNodeId(address[field], table);
    return `${uri ?? ''}|${rest}`;
  };
  if (typeof address['method_id'] === 'string') {
    return `method:${part('object_id')}:${part('method_id')}`;
  }
  if (typeof address['node_id'] === 'string') {
    return `node:${part('node_id')}:${address['field'] ?? ''}`;
  }
  return undefined;
}

// --- targets -------------------------------------------------------------------------------

/** The points a target already has and the namespace table they are written against. */
export interface TargetPoints {
  points: Config[];
  namespaces?: string[];
}

export function libraryTarget(config: Config): TargetPoints {
  const namespaces = config['library']?.['namespaces'];
  return { points: Array.isArray(config['point']) ? config['point'] : [], namespaces: Array.isArray(namespaces) ? namespaces : undefined };
}

export function deviceTarget(device: Config): TargetPoints {
  const namespaces = device['protocol_address']?.['namespaces'];
  return { points: Array.isArray(device['point']) ? device['point'] : [], namespaces: Array.isArray(namespaces) ? namespaces : undefined };
}

/** Where a target's points meet the file's: target point index by file point key. */
export interface TargetMatch {
  /** File point key → index of the target point with the same address. */
  existing: Map<string, number>;
  /** Target points whose node is not in the namespace file, by index. */
  outside: number[];
}

export function matchTarget(index: NamespaceIndex, target: TargetPoints): TargetMatch {
  const byAddress = new Map<string, string>();
  for (const node of index.file.nodes) {
    node.points?.forEach((p, n) => {
      const key = addressKey(p, index.file.namespaces);
      if (key) byAddress.set(key, pointKey(node.id, n));
    });
  }
  const existing = new Map<string, number>();
  const outside: number[] = [];
  target.points.forEach((p, i) => {
    // Without a table a target's indexes are the server's, which the file recorded.
    const key = addressKey(p, target.namespaces ?? index.file.namespaces);
    const fileKey = key ? byAddress.get(key) : undefined;
    if (fileKey && !existing.has(fileKey)) existing.set(fileKey, i);
    else outside.push(i);
  });
  return { existing, outside };
}

/** The selection a target starts with: its points that the file has, checked. */
export function initialSelection(match: TargetMatch): Map<string, PointChange> {
  return new Map([...match.existing.keys()].map(k => [k, {}]));
}

/**
 * The point a checked key becomes: the target's own point when it has that node, else the
 * proposed one. Before translation, so its ids are in the numbering of where it came from.
 */
export function basePoint(index: NamespaceIndex, target: TargetPoints | undefined, match: TargetMatch | undefined, key: string): Config | undefined {
  const at = match?.existing.get(key);
  return at !== undefined && target ? target.points[at] : proposedPoint(index, key);
}

/** The checked points with their changes, in tree order, as the picker lists them. */
export function checkedPoints(index: NamespaceIndex, selection: Selection, target?: TargetPoints, match?: TargetMatch): { key: string; point: Config }[] {
  const out: { key: string; point: Config }[] = [];
  for (const id of index.order) {
    const node = index.byId.get(id)!;
    node.points?.forEach((_, n) => {
      const key = pointKey(node.id, n);
      if (!selection.has(key)) return;
      const base = basePoint(index, target, match, key);
      if (base) out.push({ key, point: applyChange(base, selection.get(key)) });
    });
  }
  return out;
}

/** Point ids used more than once among the result's points. */
export function clashingIds(points: readonly Config[]): Set<string> {
  const seen = new Set<string>();
  const clash = new Set<string>();
  for (const p of points) {
    const id = String(p['id'] ?? '');
    if (seen.has(id)) clash.add(id);
    seen.add(id);
  }
  return clash;
}

/** The target's points after the picker: kept and changed ones in place, new ones at the end. */
export interface PointsResult {
  points: Config[];
  namespaces: string[];
  /** How many points are new, kept (checked or outside the scan), and removed. */
  added: number;
  removed: number;
  outside: number;
}

export function resultPoints(index: NamespaceIndex, selection: Selection, target: TargetPoints, match: TargetMatch): PointsResult {
  const fileTable = index.file.namespaces;
  // A target without a table takes the file's: its ids were the server's, and so are the file's.
  const namespaces = [...(target.namespaces ?? fileTable)];
  const keyOfTarget = new Map([...match.existing].map(([k, i]) => [i, k]));
  const points: Config[] = [];
  let removed = 0;
  target.points.forEach((p, i) => {
    const key = keyOfTarget.get(i);
    if (key === undefined) {
      points.push(p);
    } else if (selection.has(key)) {
      points.push(applyChange(p, selection.get(key)));
    } else {
      removed++;
    }
  });
  let added = 0;
  for (const { key, point } of checkedPoints(index, selection)) {
    if (match.existing.has(key)) continue;
    points.push(translatePoint(point, fileTable, namespaces));
    added++;
  }
  return { points, namespaces, added, removed, outside: match.outside.length };
}

// --- writing -------------------------------------------------------------------------------

/** A new library of the scan's protocol (OPC UA unless given) with the given points. */
export function newLibraryText(
  points: readonly Config[],
  namespaces: readonly string[],
  options: { type?: string; description?: string; protocol?: string } = {}
): string {
  const library: Config = { protocol: options.protocol ?? 'opcua' };
  if (options.type) library['type'] = options.type;
  if (options.description) library['description'] = options.description;
  library['namespaces'] = [...namespaces];
  return emitLibraryToml({ library, point: [...points] });
}

export interface WriteResult {
  text: string;
  /** False when the layout could not be kept and the file was written anew. */
  keptLayout: boolean;
}

/**
 * Writes `next` into `text` in two steps that toml-write can each do while keeping comments:
 * first everything but the added points (changed values, removed points, the namespace table),
 * then the added points at the end. Falls back to writing the file anew.
 */
function writeInTwoSteps(text: string, withoutAdded: Config, next: Config, emit: (c: Config) => string): WriteResult {
  const first = writeKeepingLayout(text, withoutAdded);
  const second = first === undefined ? undefined : writeKeepingLayout(first, next);
  return second === undefined ? { text: emit(next), keptLayout: false } : { text: second, keptLayout: true };
}

/** The library's text with the result's points and namespace table. */
export function writeLibrary(text: string, result: PointsResult): WriteResult {
  const current = parseToml(text);
  const library = { ...(current['library'] ?? {}), namespaces: result.namespaces };
  const kept = result.points.slice(0, result.points.length - result.added);
  return writeInTwoSteps(text, { ...current, library, point: kept }, { ...current, library, point: result.points }, emitLibraryToml);
}

/** The connector file's text with the result's points and namespace table on device `d`. */
export function writeDevice(text: string, d: number, result: PointsResult): WriteResult {
  const current = parseToml(text);
  const devices: Config[] = Array.isArray(current['device']) ? [...current['device']] : [];
  const device = devices[d];
  if (!device) {
    throw new Error(`The file has no device ${d + 1}.`);
  }
  const protocolAddress = { ...(device['protocol_address'] ?? {}), namespaces: result.namespaces };
  const kept = result.points.slice(0, result.points.length - result.added);
  const withPoints = (points: Config[]) => {
    const copy = [...devices];
    copy[d] = { ...device, protocol_address: protocolAddress, point: points };
    if (!points.length) delete copy[d]['point'];
    return { ...current, device: copy };
  };
  return writeInTwoSteps(text, withPoints(kept), withPoints(result.points), emitToml);
}
