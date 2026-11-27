import { Injectable, inject } from '@angular/core';
import { IManagedObject, InventoryBinaryService, InventoryService, OperationService } from '@c8y/client';

import {
  METADATA_FRAGMENT,
  predecessors,
  NAMESPACE_TYPE,
  type NamespaceFile,
  NamespaceFileError,
  type NamespaceMetadata,
  metadataOf,
  readNamespaceFile,
} from './namespace-file';
import { type ScanScope, scanOperation, scanResult } from './namespace-scan';
import { namespaceFilesQuery } from './queries';
import { waitForOperation } from './wait';

/** A namespace file as the list shows it: its managed object and metadata. */
export interface NamespaceEntry {
  mo: IManagedObject;
  meta?: NamespaceMetadata;
}

/**
 * Namespace files in Cumulocity: listing, reading, uploading, deleting, and scans on a gateway
 * (doc/namespace-files.md).
 */
@Injectable({ providedIn: 'root' })
export class NamespaceService {
  private readonly inventory = inject(InventoryService);
  private readonly binaries = inject(InventoryBinaryService);
  private readonly operations = inject(OperationService);

  /** Every namespace file, newest first; with `gatewayId`, those that gateway's scans produced. */
  async list(gatewayId?: string): Promise<NamespaceEntry[]> {
    const { data } = await this.inventory.list({ query: namespaceFilesQuery(), pageSize: 2000, withParents: false });
    const entries = data.map(mo => ({ mo, meta: mo[METADATA_FRAGMENT] as NamespaceMetadata | undefined }));
    return gatewayId === undefined ? entries : entries.filter(e => e.meta?.gatewayId === gatewayId);
  }

  /** The bytes of a namespace file, as stored (gzip-compressed). */
  async bytes(id: string): Promise<Uint8Array> {
    const response = await this.binaries.download(id);
    if (!response.ok) {
      throw new Error(`The namespace file could not be downloaded (HTTP ${response.status}).`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }

  /** A namespace file, read and checked. */
  async read(id: string): Promise<NamespaceFile> {
    return readNamespaceFile(await this.bytes(id));
  }

  /**
   * Stores a namespace file made elsewhere: checked first, then uploaded compressed, with its
   * metadata, replacing earlier uploads of the same device and endpoint. Throws
   * NamespaceFileError with the reason for a file that is not one.
   */
  async upload(file: File): Promise<{ id: string; replaced: number }> {
    const raw = new Uint8Array(await file.arrayBuffer());
    const parsed = await readNamespaceFile(raw);
    const meta = metadataOf(parsed);
    const compressed = raw[0] === 0x1f && raw[1] === 0x8b ? raw : await gzip(raw);
    const base = file.name.replace(/\.json(\.gz)?$/i, '').replace(/\.gz$/i, '') || 'namespace';
    const blob = new File([compressed as BlobPart], `${base}.json.gz`, { type: 'application/gzip' });
    const name = meta.device ? `${meta.device} ${meta.scannedAt}` : `${base} ${meta.scannedAt}`;
    const { data } = await this.binaries.create(blob, { name, type: NAMESPACE_TYPE });
    await this.inventory.update({ id: data.id, [METADATA_FRAGMENT]: meta });
    const id = String(data.id);
    const older = predecessors((await this.list()).map(e => ({ id: String(e.mo.id), meta: e.meta })), meta, id);
    for (const e of older) {
      await this.binaries.delete(e.id);
    }
    return { id, replaced: older.length };
  }

  async delete(id: string): Promise<void> {
    await this.binaries.delete(id);
  }

  /**
   * Asks `gateway` to scan `device` and waits for the result. `progress` gets the operation's
   * status as it changes. Resolves with the new namespace file's id.
   */
  async scan(gateway: IManagedObject, device: string, scope: ScanScope, progress?: (status: string) => void): Promise<string> {
    const { data: created } = await this.operations.create(scanOperation(String(gateway.id), device, scope) as any);
    const id = String(created.id);
    const done = await waitForOperation(
      async () => {
        const { data } = await this.operations.detail(id);
        progress?.(String(data.status));
        return data;
      },
      `The scan of ${device}`,
      { timeoutMs: 15 * 60 * 1000, intervalMs: 3000 }
    );
    const { binaryId } = scanResult(done as unknown as Record<string, unknown>);
    if (binaryId) {
      return binaryId;
    }
    // A gateway that could not write the result into the operation: the newest file of that
    // device on that gateway, stored after the operation was created. Binaries carry no
    // creationTime; lastUpdated is when the gateway stored the metadata.
    const since = String(created.creationTime ?? '');
    const newest = (await this.list(String(gateway.id))).find(
      e => e.meta?.device === device && String(e.mo['lastUpdated'] ?? '') >= since
    );
    if (!newest) {
      throw new NamespaceFileError(`The scan of ${device} finished, but its namespace file was not found.`);
    }
    return String(newest.mo.id);
  }
}

async function gzip(bytes: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([bytes as BlobPart]).stream().pipeThrough(new CompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
