/**
 * Where Create from namespace comes from and where it goes back to: the editor's file (its
 * current, possibly unsaved text) and device, and which stored namespace file and repository
 * entry fit a device.
 *
 * Plain TypeScript, without Angular, so it is unit-tested as it is; PickerContextService
 * (picker-context.service.ts) holds the context between the editor and the picker.
 */
import type { NamespaceMetadata } from './namespace-file';
import type { FileKind } from './naming';

/** The editor's file, handed to the picker so the points land in it and the editor gets it back. */
export interface PickerContext {
  kind: FileKind;
  /** The saved entry's id; undefined for a file not saved yet. */
  entryId?: string;
  /** The editor's fields, so a new file comes back as it was. */
  name: string;
  description: string;
  fileName: string;
  deviceType: string;
  /** The editor's current text, unsaved edits included. */
  text: string;
  /** For a connector file: the device the points go to. */
  device?: string;
}

/** The part of a stored namespace file these helpers need. */
export interface StoredNamespace {
  id: string;
  meta?: NamespaceMetadata;
  lastUpdated?: string;
}

/**
 * The namespace files that fit an editor's file, best first. For a device: the scans of that
 * device, those of the same connector file first, newest first. For a library: every OPC UA
 * file, newest first.
 */
export function scansFor(stored: readonly StoredNamespace[], context: Pick<PickerContext, 'kind' | 'device' | 'fileName'>): StoredNamespace[] {
  const opcua = stored.filter(s => (s.meta?.protocol ?? 'opcua') === 'opcua');
  const fits = context.kind === 'connector' && context.device ? opcua.filter(s => s.meta?.device === context.device) : opcua;
  const rank = (s: StoredNamespace) => (context.kind === 'connector' && s.meta?.connectorFile === context.fileName ? 0 : 1);
  return [...fits].sort((a, b) => rank(a) - rank(b) || String(b.lastUpdated ?? '').localeCompare(String(a.lastUpdated ?? '')));
}

/** A repository connector entry as the picker knows it: its file name and its devices. */
export interface ConnectorCandidate {
  id: string;
  fileName: string;
  devices: string[];
}

/**
 * The connector entry and device a scan belongs to, from its metadata: the entry whose file name
 * is the scan's connector file and that defines the scanned device. Undefined when none fits.
 */
export function targetForScan(
  meta: Pick<NamespaceMetadata, 'connectorFile' | 'device'> | undefined,
  candidates: readonly ConnectorCandidate[]
): { entryId: string; device: number } | undefined {
  if (!meta?.connectorFile || !meta.device) {
    return undefined;
  }
  for (const c of candidates) {
    const device = c.devices.indexOf(meta.device);
    if (c.fileName === meta.connectorFile && device >= 0) {
      return { entryId: c.id, device };
    }
  }
  return undefined;
}
