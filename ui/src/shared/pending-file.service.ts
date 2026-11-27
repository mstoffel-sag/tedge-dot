import { Injectable } from '@angular/core';

import { type FileKind } from './naming';

export interface PendingFile {
  kind: FileKind;
  /** The file name, without `.toml`. */
  fileName: string;
  text: string;
  /** The entry's name, description and device type; the name defaults to the file name. */
  name?: string;
  description?: string;
  deviceType?: string;
  /** For a copy: the name of the entry it was made from. */
  copyOf?: string;
  /** For changes to a saved entry: its id. The editor opens that entry with `text` instead. */
  entryId?: string;
  /** Shown above the editor until the file is saved, e.g. where the changes came from. */
  note?: string;
}

/**
 * Hands a file to the editor, which opens it as a new entry (an uploaded file that needs fixing,
 * a copy of an entry, a library built from a namespace file) or as changes to a saved entry
 * (points added from a namespace file). The editor takes it once.
 */
@Injectable({ providedIn: 'root' })
export class PendingFileService {
  private pending?: PendingFile;

  hand(file: PendingFile): void {
    this.pending = file;
  }

  /** A new file, if one was handed. */
  take(): PendingFile | undefined {
    if (this.pending?.entryId !== undefined) {
      return undefined;
    }
    const file = this.pending;
    this.pending = undefined;
    return file;
  }

  /** Changes to the saved entry `entryId`, if they were handed. */
  takeFor(entryId: string): PendingFile | undefined {
    if (this.pending?.entryId !== entryId) {
      return undefined;
    }
    const file = this.pending;
    this.pending = undefined;
    return file;
  }
}
