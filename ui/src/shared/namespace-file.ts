/**
 * Namespace files: a stored scan of an OPC UA server's address space with the tedge-dot points
 * proposed for its nodes (doc/namespace-files.md, schema doc/contract/schemas/
 * namespace-file.schema.json). Reading one, the checks a reader makes beyond the schema, and the
 * index the namespace tree works on.
 *
 * Plain TypeScript, without Angular, so it is unit-tested as it is.
 */
import type { Config } from '../generated/config-builder';

/** The Cumulocity binary type of a namespace file. */
export const NAMESPACE_TYPE = 'tedge-dot-namespace';

/** The operation that scans a device's address space on a gateway. */
export const SCAN_OPERATION = 'c8y_ScanAddressSpace';

/** The managed object fragment that holds a namespace file's metadata. */
export const METADATA_FRAGMENT = 'tedgeDotNamespace';

/** The format versions this plugin reads. */
export const KNOWN_VERSIONS = [1];

/** Larger (decompressed) files are refused: a browser tab would struggle with them. */
export const MAX_TEXT_BYTES = 200 * 1024 * 1024;

export const NODE_CLASSES = ['Object', 'Variable', 'Method', 'ObjectType', 'VariableType', 'ReferenceType', 'DataType', 'View'] as const;
export type NodeClass = (typeof NODE_CLASSES)[number];

export interface NamespaceNode {
  id: string;
  parent?: string;
  browse_name: string;
  display_name: string;
  node_class: NodeClass;
  description?: string;
  datatype?: string;
  value_rank?: number;
  access_level?: number;
  user_access_level?: number;
  historizing?: boolean;
  unit?: string;
  subscribable?: boolean;
  value?: string;
  points?: Config[];
  skipped?: string;
  truncated?: 'depth' | 'max_children' | 'error';
}

export interface NamespaceFile {
  format: 'tedge-dot-namespace';
  version: number;
  protocol: 'opcua';
  server: { endpoint: string; application_uri?: string; product_name?: string };
  namespaces: string[];
  scan: {
    time: string;
    tool: string;
    tool_version?: string;
    root: string;
    depth?: number | 'unlimited';
    include?: string[];
    exclude?: string[];
    types?: string[];
    max_children?: number;
    preview_values?: boolean;
    device?: string;
    connector_file?: string;
  };
  nodes: NamespaceNode[];
}

/** The `tedgeDotNamespace` fragment: what the list shows without downloading the file. */
export interface NamespaceMetadata {
  formatVersion: number;
  protocol: string;
  /** The gateway's device id (external id) and managed object id. */
  gateway?: string;
  gatewayId?: string;
  device?: string;
  connectorFile?: string;
  endpoint: string;
  applicationUri?: string;
  productName?: string;
  namespaces: string[];
  scannedAt: string;
  tool: string;
  toolVersion?: string;
  scope: {
    root: string;
    depth?: number | 'unlimited';
    include: string[];
    exclude: string[];
    types: string[];
    maxChildren?: number;
  };
  nodes: number;
  points: number;
  truncated: boolean;
}

export class NamespaceFileError extends Error {}

/** Whether the bytes start like gzip (1f 8b). */
export function isGzip(bytes: Uint8Array): boolean {
  return bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
}

/** The text of a namespace file, compressed or not. */
export async function namespaceText(bytes: Uint8Array): Promise<string> {
  if (!isGzip(bytes)) {
    if (bytes.length > MAX_TEXT_BYTES) {
      throw tooLarge();
    }
    return new TextDecoder().decode(bytes);
  }
  const stream = new Blob([bytes as BlobPart]).stream().pipeThrough(new DecompressionStream('gzip'));
  const reader = stream.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) {
      break;
    }
    total += value.length;
    if (total > MAX_TEXT_BYTES) {
      await reader.cancel();
      throw tooLarge();
    }
    chunks.push(value);
  }
  const all = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    all.set(chunk, at);
    at += chunk.length;
  }
  return new TextDecoder().decode(all);
}

function tooLarge(): NamespaceFileError {
  return new NamespaceFileError(
    `The namespace file is larger than ${MAX_TEXT_BYTES / 1024 / 1024} MB uncompressed. Scan a smaller part of the address space, with include patterns or a depth.`
  );
}

/** Reads and checks a namespace file; throws NamespaceFileError with a reason for people. */
export async function readNamespaceFile(bytes: Uint8Array): Promise<NamespaceFile> {
  const text = await namespaceText(bytes);
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch {
    throw new NamespaceFileError('This file is not JSON. A namespace file is written by opcua-scan with --output tedge-dot-namespace.');
  }
  return checkNamespaceFile(json);
}

/**
 * The checks a reader makes: the format and version, the shape it relies on, and the rules the
 * schema cannot express (parents first, unique ids). Returns the file or throws.
 */
export function checkNamespaceFile(json: unknown): NamespaceFile {
  const file = json as Partial<NamespaceFile> | null;
  if (!file || typeof file !== 'object' || Array.isArray(file)) {
    throw new NamespaceFileError('This file is not a namespace file.');
  }
  if (file.format !== 'tedge-dot-namespace') {
    throw new NamespaceFileError(
      'This file is not a namespace file. opcua-scan writes one with --output tedge-dot-namespace; its plain json export does not work here.'
    );
  }
  if (typeof file.version !== 'number' || !KNOWN_VERSIONS.includes(file.version)) {
    throw new NamespaceFileError(
      `This namespace file has version ${JSON.stringify(file.version)}; this plugin reads version ${KNOWN_VERSIONS.join(', ')}. Update the plugin.`
    );
  }
  if (file.protocol !== 'opcua') {
    throw new NamespaceFileError(`This namespace file is for the protocol ${JSON.stringify(file.protocol)}; only opcua is supported.`);
  }
  if (!file.server || typeof file.server.endpoint !== 'string') {
    throw new NamespaceFileError('This namespace file names no server endpoint.');
  }
  if (!Array.isArray(file.namespaces) || !file.namespaces.length || !file.namespaces.every(n => typeof n === 'string')) {
    throw new NamespaceFileError("This namespace file has no namespace table.");
  }
  if (!file.scan || typeof file.scan.time !== 'string') {
    throw new NamespaceFileError('This namespace file does not say when it was scanned.');
  }
  if (!Array.isArray(file.nodes)) {
    throw new NamespaceFileError('This namespace file has no nodes.');
  }
  const seen = new Set<string>();
  file.nodes.forEach((node, i) => {
    if (!node || typeof node.id !== 'string' || typeof node.node_class !== 'string') {
      throw new NamespaceFileError(`Node ${i + 1} has no id or node class.`);
    }
    if (!NODE_CLASSES.includes(node.node_class)) {
      throw new NamespaceFileError(`The node ${node.id} has the unknown node class ${JSON.stringify(node.node_class)}.`);
    }
    if (node.points !== undefined && node.skipped !== undefined) {
      throw new NamespaceFileError(`The node ${node.id} has proposed points and a reason why it has none.`);
    }
    if (seen.has(node.id)) {
      throw new NamespaceFileError(`The node ${node.id} appears twice.`);
    }
    if (node.parent !== undefined && !seen.has(node.parent)) {
      throw new NamespaceFileError(`The node ${node.id} comes before its parent ${node.parent}; nodes must be listed parents first.`);
    }
    seen.add(node.id);
  });
  return file as NamespaceFile;
}

/** The metadata of a namespace file, as the gateway's scan script writes it. */
export function metadataOf(file: NamespaceFile, where: { gateway?: string; device?: string; connectorFile?: string } = {}): NamespaceMetadata {
  const meta: NamespaceMetadata = {
    formatVersion: file.version,
    protocol: file.protocol,
    gateway: where.gateway,
    device: where.device ?? file.scan.device,
    connectorFile: where.connectorFile ?? file.scan.connector_file,
    endpoint: file.server.endpoint,
    applicationUri: file.server.application_uri,
    productName: file.server.product_name,
    namespaces: file.namespaces,
    scannedAt: file.scan.time,
    tool: file.scan.tool,
    toolVersion: file.scan.tool_version,
    scope: {
      root: file.scan.root,
      depth: file.scan.depth,
      include: file.scan.include ?? [],
      exclude: file.scan.exclude ?? [],
      types: file.scan.types ?? [],
      maxChildren: file.scan.max_children,
    },
    nodes: file.nodes.length,
    points: file.nodes.reduce((n, node) => n + (node.points?.length ?? 0), 0),
    truncated: file.nodes.some(node => node.truncated !== undefined),
  };
  return JSON.parse(JSON.stringify(meta));
}

/**
 * The stored files a new one replaces, so each server keeps one current file: for a gateway scan
 * the earlier files of that gateway and device (the gateway deletes those itself), for an upload
 * the earlier uploads of the same device and endpoint. Uploads never replace a gateway's files.
 */
export function predecessors<T extends { id: string; meta?: NamespaceMetadata }>(stored: readonly T[], meta: NamespaceMetadata, keepId: string): T[] {
  return stored.filter(e => {
    if (e.id === keepId || !e.meta) {
      return false;
    }
    if (meta.gateway) {
      return e.meta.gateway === meta.gateway && e.meta.device === meta.device;
    }
    return !e.meta.gateway && (e.meta.device ?? '') === (meta.device ?? '') && e.meta.endpoint === meta.endpoint;
  });
}

/** A namespace file prepared for the tree: nodes by id, children by parent, search keys. */
export interface NamespaceIndex {
  file: NamespaceFile;
  byId: Map<string, NamespaceNode>;
  children: Map<string, string[]>;
  /** Nodes without a parent (normally just the scan's root). */
  roots: string[];
  /** Lower-cased "display name / browse path" per node id, for search. */
  searchKey: Map<string, string>;
  /** Display-name path from a root, per node id. */
  path: Map<string, string>;
  /** Every node id in tree order: depth first, children by display name (numbers in order). */
  order: string[];
}

const byName = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

export function indexNamespace(file: NamespaceFile): NamespaceIndex {
  const byId = new Map<string, NamespaceNode>();
  const children = new Map<string, string[]>();
  const roots: string[] = [];
  const path = new Map<string, string>();
  const searchKey = new Map<string, string>();
  for (const node of file.nodes) {
    byId.set(node.id, node);
    const name = node.display_name || node.browse_name || node.id;
    if (node.parent === undefined) {
      roots.push(node.id);
      path.set(node.id, name);
    } else {
      let list = children.get(node.parent);
      if (!list) {
        list = [];
        children.set(node.parent, list);
      }
      list.push(node.id);
      path.set(node.id, `${path.get(node.parent) ?? ''}/${name}`);
    }
    searchKey.set(node.id, `${name}\n${path.get(node.id)}\n${node.browse_name}`.toLowerCase());
  }
  // opcua-scan browses concurrently, so the file's order is not the server's: sort by name.
  const nameOf = (id: string) => byId.get(id)!.display_name || byId.get(id)!.browse_name || id;
  for (const list of children.values()) {
    list.sort((a, b) => byName.compare(nameOf(a), nameOf(b)));
  }
  roots.sort((a, b) => byName.compare(nameOf(a), nameOf(b)));
  const index: NamespaceIndex = { file, byId, children, roots, searchKey, path, order: [] };
  index.order = roots.flatMap(r => subtree(index, r));
  return index;
}

/** The ids of `id` and every node below it, parents first. */
export function subtree(index: NamespaceIndex, id: string): string[] {
  const out: string[] = [];
  const stack = [id];
  while (stack.length) {
    const next = stack.pop()!;
    out.push(next);
    const kids = index.children.get(next);
    if (kids) {
      for (let i = kids.length - 1; i >= 0; i--) {
        stack.push(kids[i]);
      }
    }
  }
  return out;
}

/** The ancestors of `id`, nearest last, without `id` itself. */
export function ancestors(index: NamespaceIndex, id: string): string[] {
  const out: string[] = [];
  let at = index.byId.get(id)?.parent;
  while (at !== undefined) {
    out.unshift(at);
    at = index.byId.get(at)?.parent;
  }
  return out;
}

export interface TreeFilter {
  /** Matched against the display name, the browse path and the browse name, ignoring case. */
  text?: string;
  nodeClasses?: readonly NodeClass[];
  /** Only nodes with proposed points. */
  withPoints?: boolean;
  /** Only variables whose (user) access level allows writing. */
  writable?: boolean;
  datatype?: string;
}

/** Whether a node matches a filter by itself (ancestors aside). */
export function matches(index: NamespaceIndex, id: string, filter: TreeFilter): boolean {
  const node = index.byId.get(id);
  if (!node) {
    return false;
  }
  const text = filter.text?.trim().toLowerCase();
  if (text && !index.searchKey.get(id)!.includes(text)) {
    return false;
  }
  if (filter.nodeClasses?.length && !filter.nodeClasses.includes(node.node_class)) {
    return false;
  }
  if (filter.withPoints && !node.points?.length) {
    return false;
  }
  if (filter.writable && !(((node.user_access_level ?? node.access_level ?? 0) & 2) === 2)) {
    return false;
  }
  if (filter.datatype && (node.datatype ?? '').toLowerCase() !== filter.datatype.toLowerCase()) {
    return false;
  }
  return true;
}

export function isFilterActive(filter: TreeFilter): boolean {
  return !!(filter.text?.trim() || filter.nodeClasses?.length || filter.withPoints || filter.writable || filter.datatype);
}

/**
 * The nodes a filter keeps visible: the matching ones and their ancestors. Undefined when no
 * filter is active, meaning every node.
 */
export function visibleNodes(index: NamespaceIndex, filter: TreeFilter): Set<string> | undefined {
  if (!isFilterActive(filter)) {
    return undefined;
  }
  const keep = new Set<string>();
  for (const node of index.file.nodes) {
    if (keep.has(node.id) || !matches(index, node.id, filter)) {
      continue;
    }
    let at: string | undefined = node.id;
    while (at !== undefined && !keep.has(at)) {
      keep.add(at);
      at = index.byId.get(at)?.parent;
    }
  }
  return keep;
}

export interface TreeRow {
  id: string;
  depth: number;
  hasChildren: boolean;
  expanded: boolean;
}

/**
 * The rows the tree shows, in order: the roots and the children of expanded nodes. With a filter
 * (`visible`), only the visible nodes, and every node with visible children is open unless the
 * user closed it (`collapsed`).
 */
export function treeRows(
  index: NamespaceIndex,
  expanded: ReadonlySet<string>,
  visible?: ReadonlySet<string>,
  collapsed: ReadonlySet<string> = new Set()
): TreeRow[] {
  const rows: TreeRow[] = [];
  const kidsOf = (id: string) => (index.children.get(id) ?? []).filter(k => !visible || visible.has(k));
  const stack: { id: string; depth: number }[] = index.roots
    .filter(r => !visible || visible.has(r))
    .reverse()
    .map(id => ({ id, depth: 0 }));
  while (stack.length) {
    const { id, depth } = stack.pop()!;
    const kids = kidsOf(id);
    const open = kids.length > 0 && (visible ? !collapsed.has(id) : expanded.has(id));
    rows.push({ id, depth, hasChildren: kids.length > 0, expanded: open });
    if (open) {
      for (let i = kids.length - 1; i >= 0; i--) {
        stack.push({ id: kids[i], depth: depth + 1 });
      }
    }
  }
  return rows;
}

/** Why a variable or method cannot be checked, or undefined when it can (or is not one). */
export function whyNoPoints(node: NamespaceNode): string | undefined {
  if (node.points?.length) {
    return undefined;
  }
  if (node.node_class !== 'Variable' && node.node_class !== 'Method') {
    return undefined;
  }
  return node.skipped ?? 'opcua-scan proposed no point for this node.';
}
