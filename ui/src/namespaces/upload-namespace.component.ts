import { Component, inject, signal } from '@angular/core';
import { CoreModule, DropAreaComponent, type DroppedFile } from '@c8y/ngx-components';
import { BsModalRef } from 'ngx-bootstrap/modal';

import { NamespaceService } from '../shared/namespace.service';

type State = 'pick' | 'saving' | 'done' | 'error';

/**
 * Stores a namespace file made elsewhere, e.g. with opcua-scan on a laptop:
 * `opcua-scan browse ... --output tedge-dot-namespace --file line1.json`. The file is checked
 * first; a file that is not a namespace file is refused with the reason.
 */
@Component({
  selector: 'tdot-upload-namespace',
  imports: [CoreModule, DropAreaComponent],
  template: `
    <div class="modal-header dialog-header">
      <i c8yIcon="upload"></i>
      <h4>{{ 'Upload namespace file' | translate }}</h4>
    </div>
    <div class="modal-body">
      @switch (state()) {
        @case ('pick') {
          <c8y-form-group>
            <c8y-drop-area
              class="drop-area-sm"
              accept=".json,.gz"
              [maxAllowedFiles]="1"
              [title]="'Drop a namespace file (.json or .json.gz) or click to browse' | translate"
              (dropped)="onDropped($event)"
            ></c8y-drop-area>
            <c8y-messages
              [helpMessage]="'Written by opcua-scan browse ... --output tedge-dot-namespace. It replaces an earlier upload of the same server.' | translate"
            ></c8y-messages>
          </c8y-form-group>
        }
        @case ('saving') {
          <p><i c8yIcon="spinner" class="icon-spin"></i> {{ 'Checking and storing the file' | translate }}…</p>
        }
        @case ('done') {
          <div class="alert alert-success" role="status">
            {{ 'Namespace file stored.' | translate }}
            @if (replaced()) {
              {{ (replaced() === 1 ? 'It replaced the earlier upload of this server.' : 'It replaced the earlier uploads of this server.') | translate }}
            }
          </div>
        }
        @case ('error') {
          <div class="alert alert-danger" role="alert">{{ error() }}</div>
        }
      }
    </div>
    <div class="modal-footer">
      <button type="button" class="btn btn-default" (click)="close()">{{ (state() === 'done' ? 'Close' : 'Cancel') | translate }}</button>
      @if (state() === 'error') {
        <button type="button" class="btn btn-primary" (click)="state.set('pick')">{{ 'Try another file' | translate }}</button>
      }
    </div>
  `,
})
export class UploadNamespaceComponent {
  private readonly namespaces = inject(NamespaceService);
  private readonly modalRef = inject(BsModalRef);

  /** Set by the opener: called after the file was stored. */
  onFinished?: () => void;
  /** Set by the opener: called with the stored file's id, and the dialog closes. */
  onStored?: (id: string) => void;

  readonly state = signal<State>('pick');
  readonly error = signal('');
  /** How many earlier uploads of the same device and endpoint the new file replaced. */
  readonly replaced = signal(0);

  async onDropped(files: DroppedFile[]): Promise<void> {
    const dropped = files?.[0];
    if (!dropped) {
      return;
    }
    this.state.set('saving');
    try {
      const { id, replaced } = await this.namespaces.upload(dropped.file);
      this.replaced.set(replaced);
      if (this.onStored) {
        this.modalRef.hide();
        this.onStored(id);
        return;
      }
      this.state.set('done');
      this.onFinished?.();
    } catch (e) {
      this.error.set(`${dropped.file.name}: ${e instanceof Error ? e.message : String(e)}`);
      this.state.set('error');
    }
  }

  close(): void {
    this.modalRef.hide();
  }
}
