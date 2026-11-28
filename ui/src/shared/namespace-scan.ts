/**
 * Asking a gateway to scan an OPC UA device: the c8y_ScanAddressSpace operation the scan dialog
 * sends, and the devices it offers (operations/c8y_ScanAddressSpace, doc/namespace-files.md).
 *
 * Plain TypeScript, without Angular, so it is unit-tested as it is.
 */
import { SCAN_OPERATION } from './namespace-file';

/** What the dialog asks for. Empty fields fall back to the gateway's defaults. */
export interface ScanScope {
  root?: string;
  /** A number, or empty for unlimited. */
  depth?: string;
  /** One glob per line. */
  include?: string;
  exclude?: string;
  /** Node classes, e.g. `Variable,Method`. */
  types?: string[];
  maxChildren?: string;
}

/** Lines of a text field, trimmed, without empty ones. */
export function lines(text: string | undefined): string[] {
  return (text ?? '')
    .split('\n')
    .map(l => l.trim())
    .filter(Boolean);
}

/** A problem with the scope, for people, or undefined. */
export function scopeProblem(scope: ScanScope): string | undefined {
  const depth = scope.depth?.trim();
  if (depth && depth !== 'unlimited' && !/^\d+$/.test(depth)) {
    return 'The depth must be a whole number, or empty for unlimited.';
  }
  const max = scope.maxChildren?.trim();
  if (max && !/^[1-9]\d*$/.test(max)) {
    return 'The child limit must be a whole number above 0, or empty for none.';
  }
  return undefined;
}

/** The c8y_ScanAddressSpace fragment for a device and a scope. */
export function scanFragment(device: string, scope: ScanScope): Record<string, unknown> {
  const fragment: Record<string, unknown> = { device };
  if (scope.root?.trim()) fragment['root'] = scope.root.trim();
  const depth = scope.depth?.trim();
  fragment['depth'] = depth && depth !== 'unlimited' ? Number(depth) : 'unlimited';
  const include = lines(scope.include);
  if (include.length) fragment['include'] = include;
  const exclude = lines(scope.exclude);
  if (exclude.length) fragment['exclude'] = exclude;
  if (scope.types?.length) fragment['type'] = scope.types.join(',');
  if (scope.maxChildren?.trim()) fragment['max_children'] = Number(scope.maxChildren.trim());
  return fragment;
}

/** The operation for a gateway. */
export function scanOperation(gatewayId: string, device: string, scope: ScanScope): Record<string, unknown> {
  return {
    deviceId: gatewayId,
    description: `Scan the address space of ${device}`,
    [SCAN_OPERATION]: scanFragment(device, scope),
  };
}

/** Whether a gateway announces the scan operation. */
export function canScan(gateway: unknown): boolean {
  const ops = (gateway as { c8y_SupportedOperations?: unknown } | null)?.c8y_SupportedOperations;
  return Array.isArray(ops) && ops.includes(SCAN_OPERATION);
}

/**
 * The child devices a scan can name: those registered as OPC UA devices (`ot-protocol`) when the
 * gateway reports the protocol, else all of them (the gateway refuses a device that is not in an
 * OPC UA connector file).
 */
export function scanDevices<T extends { name?: string; [key: string]: unknown }>(children: readonly T[]): T[] {
  const named = children.filter(c => typeof c.name === 'string' && c.name);
  const withProtocol = named.filter(c => typeof c['ot-protocol'] === 'string');
  return withProtocol.length ? withProtocol.filter(c => c['ot-protocol'] === 'opcua') : named;
}

/** The binary a successful scan created, from the completed operation. */
export function scanResult(operation: Record<string, unknown>): { binaryId?: string; nodes?: number } {
  const fragment = operation[SCAN_OPERATION] as { binaryId?: unknown; nodes?: unknown } | undefined;
  return {
    binaryId: typeof fragment?.binaryId === 'string' ? fragment.binaryId : undefined,
    nodes: typeof fragment?.nodes === 'number' ? fragment.nodes : undefined,
  };
}

/** The part of a stored namespace file the scan dialog needs. */
export interface ExistingScan {
  id: string;
  lastUpdated?: string;
  meta?: { gatewayId?: string; device?: string; scannedAt?: string; nodes?: number; points?: number };
}

/**
 * The namespace file a new scan of `device` on `gatewayId` would replace, newest first if
 * several are left over; undefined when the device has none from that gateway.
 */
export function currentScan<T extends ExistingScan>(stored: readonly T[], gatewayId: string, device: string): T | undefined {
  return stored
    .filter(s => s.meta?.gatewayId === gatewayId && s.meta?.device === device)
    .sort((a, b) => String(b.meta?.scannedAt ?? b.lastUpdated ?? '').localeCompare(String(a.meta?.scannedAt ?? a.lastUpdated ?? '')))[0];
}
