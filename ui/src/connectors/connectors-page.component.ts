import { NgTemplateOutlet } from '@angular/common';
import { Component, OnInit, computed, inject, signal } from '@angular/core';
import { Router } from '@angular/router';
import { IManagedObject, InventoryService } from '@c8y/client';
import { AlertService, CoreModule, ModalService, Permissions, Status } from '@c8y/ngx-components';
import { gettext } from '@c8y/ngx-components/gettext';
import { RepositoryService, SharedRepositoryModule } from '@c8y/ngx-components/repository/shared';
import { BsModalService } from 'ngx-bootstrap/modal';

import { ConnectorOperationsService } from '../shared/connector-operations.service';
import { copyFileName, duplicateText } from '../shared/duplicate';
import { packagedLibraries } from '../shared/libraries';
import { type FileKind, entriesOfKind, fileOfType } from '../shared/naming';
import { PendingFileService } from '../shared/pending-file.service';
import { canAddEntries } from '../shared/permissions';
import { managedEntriesQuery } from '../shared/queries';
import { SendToGatewayComponent } from './send-to-gateway.component';
import { SendToGroupComponent } from './send-to-group.component';
import { NamespacesCardComponent } from '../namespaces/namespaces-card.component';
import { UploadFileComponent } from './upload-file.component';

/**
 * Management > OT connectors: the configuration repository's connector files, in a second list
 * its point libraries, and in a third the namespace files that point libraries can be built from.
 */
@Component({
  selector: 'tdot-connectors-page',
  imports: [CoreModule, SharedRepositoryModule, NgTemplateOutlet, NamespacesCardComponent],
  providers: [ConnectorOperationsService],
  template: `
    <c8y-title>{{ 'OT connectors' | translate }}</c8y-title>
    <!-- Adding entries needs the roles Device Management asks for to save to the repository. -->
    @if (canAdd) {
      <c8y-action-bar-item [placement]="'right'">
        <button class="btn btn-link" type="button" (click)="add('connector')">
          <i c8yIcon="plus-circle"></i>
          {{ 'Add connector file' | translate }}
        </button>
      </c8y-action-bar-item>
      <c8y-action-bar-item [placement]="'right'">
        <button class="btn btn-link" type="button" (click)="upload('connector')">
          <i c8yIcon="upload"></i>
          {{ 'Upload connector file' | translate }}
        </button>
      </c8y-action-bar-item>
      <c8y-action-bar-item [placement]="'right'">
        <button class="btn btn-link" type="button" (click)="add('library')">
          <i c8yIcon="plus-circle"></i>
          {{ 'Add point library' | translate }}
        </button>
      </c8y-action-bar-item>
      <c8y-action-bar-item [placement]="'right'">
        <button class="btn btn-link" type="button" (click)="upload('library')">
          <i c8yIcon="upload"></i>
          {{ 'Upload point library' | translate }}
        </button>
      </c8y-action-bar-item>
    }
    <c8y-action-bar-item [placement]="'right'">
      <button class="btn btn-link" type="button" (click)="load()">
        <i c8yIcon="refresh" [class.icon-spin]="loading()"></i>
        {{ 'Reload' | translate }}
      </button>
    </c8y-action-bar-item>

    <div class="card">
      <div class="card-header separator">
        <div class="card-title">{{ 'Connector files' | translate }}</div>
      </div>
      @if (!loading() && connectors().length === 0) {
        <c8y-ui-empty-state
          [icon]="'gears'"
          [title]="'No connector files in the repository.' | translate"
          [subtitle]="canAdd ? ('Add one, or save a snapshot from a gateway.' | translate) : ''"
          [horizontal]="true"
        ></c8y-ui-empty-state>
      } @else {
        <div class="table-responsive">
          <table class="table large-padding">
            <thead>
              <tr>
                <th>{{ 'Name' | translate }}</th>
                <th>{{ 'Description' | translate }}</th>
                <th>{{ 'File' | translate }}</th>
                <th>{{ 'Device type' | translate }}</th>
                <th>{{ 'Last updated' | translate }}</th>
                <th class="text-right">{{ 'Actions' | translate }}</th>
              </tr>
            </thead>
            <tbody>
              @for (entry of connectors(); track entry.id) {
                <tr>
                  <td><a class="interact" (click)="edit(entry)">{{ entry['name'] }}</a></td>
                  <td>{{ entry['description'] }}</td>
                  <td>{{ fileOf(entry) }}</td>
                  <td>{{ entry['deviceType'] }}</td>
                  <td>{{ entry['lastUpdated'] | c8yDate }}</td>
                  <td class="text-right text-nowrap">
                    <ng-container *ngTemplateOutlet="actions; context: { $implicit: entry }"></ng-container>
                  </td>
                </tr>
              }
            </tbody>
          </table>
        </div>
      }
    </div>

    <div class="card">
      <div class="card-header separator">
        <div class="card-title">{{ 'Point libraries' | translate }}</div>
      </div>
      @if (!loading() && libraries().length === 0) {
        <c8y-ui-empty-state
          [icon]="'list'"
          [title]="'No point libraries in the repository.' | translate"
          [subtitle]="canAdd ? ('Add one, or upload a point library.' | translate) : ''"
          [horizontal]="true"
        ></c8y-ui-empty-state>
      } @else {
        <div class="table-responsive">
          <table class="table large-padding">
            <thead>
              <tr>
                <th>{{ 'Name' | translate }}</th>
                <th>{{ 'Description' | translate }}</th>
                <th>{{ 'Protocol' | translate }}</th>
                <th>{{ 'Library' | translate }}</th>
                <th>{{ 'Last updated' | translate }}</th>
                <th class="text-right">{{ 'Actions' | translate }}</th>
              </tr>
            </thead>
            <tbody>
              @for (entry of libraries(); track entry.id) {
                <tr>
                  <td><a class="interact" (click)="edit(entry)">{{ entry['name'] }}</a></td>
                  <td>{{ entry['description'] }}</td>
                  <td>{{ fileOfType(entry['configurationType'])?.protocol }}</td>
                  <td>{{ fileOfType(entry['configurationType'])?.name }}</td>
                  <td>{{ entry['lastUpdated'] | c8yDate }}</td>
                  <td class="text-right text-nowrap">
                    <ng-container *ngTemplateOutlet="actions; context: { $implicit: entry }"></ng-container>
                  </td>
                </tr>
              }
            </tbody>
          </table>
        </div>
      }
    </div>

    <tdot-namespaces-card />

    <ng-template #actions let-entry>
      <button class="btn btn-default btn-xs m-r-4" type="button" (click)="edit(entry)">
        {{ 'Edit' | translate }}
      </button>
      @if (canAdd) {
        <button class="btn btn-default btn-xs m-r-4" type="button" (click)="duplicate(entry)">
          {{ 'Duplicate' | translate }}
        </button>
      }
      <button class="btn btn-default btn-xs m-r-4" type="button" (click)="sendToGateway(entry)">
        {{ 'Send to a gateway' | translate }}
      </button>
      <button class="btn btn-default btn-xs m-r-4" type="button" (click)="sendToGroup(entry)">
        {{ 'Send to a group' | translate }}
      </button>
      <button class="btn btn-danger btn-xs" type="button" (click)="remove(entry)">
        {{ 'Delete' | translate }}
      </button>
    </ng-template>
  `,
})
export class ConnectorsPageComponent implements OnInit {
  private readonly inventory = inject(InventoryService);
  private readonly repository = inject(RepositoryService);
  private readonly ops = inject(ConnectorOperationsService);
  private readonly router = inject(Router);
  private readonly modal = inject(ModalService);
  private readonly bsModal = inject(BsModalService);
  private readonly alert = inject(AlertService);
  private readonly pending = inject(PendingFileService);
  private readonly permissions = inject(Permissions);

  readonly entries = signal<IManagedObject[]>([]);
  readonly connectors = computed(() => entriesOfKind(this.entries(), 'connector'));
  readonly libraries = computed(() => entriesOfKind(this.entries(), 'library'));
  readonly loading = signal(true);
  readonly fileOfType = fileOfType;
  /** Whether the user can add entries; without it, Add, Upload and Duplicate are hidden. */
  readonly canAdd = canAddEntries(roles => this.permissions.hasAnyRole(roles));

  ngOnInit(): void {
    void this.load();
  }

  async load(): Promise<void> {
    this.loading.set(true);
    try {
      const { data } = await this.inventory.list({ query: managedEntriesQuery(), pageSize: 2000 });
      this.entries.set(data);
    } catch (e) {
      this.alert.addServerFailure(e);
    } finally {
      this.loading.set(false);
    }
  }

  fileOf(entry: IManagedObject): string {
    const file = fileOfType(entry['configurationType']);
    return file ? `${file.name}.toml` : '';
  }

  add(kind: FileKind): void {
    void this.router.navigate(['/ot-connectors', kind === 'library' ? 'new-library' : 'new']);
  }

  edit(entry: IManagedObject): void {
    void this.router.navigate(['/ot-connectors', entry.id]);
  }

  /**
   * Opens a copy of the entry in the editor, unsaved: the same content, description and device
   * type, named "<name> copy" with the file name `<file>-copy`. A connector file gets its own
   * service name, so the copy can run next to the original.
   */
  async duplicate(entry: IManagedObject): Promise<void> {
    const file = fileOfType(entry['configurationType']);
    if (!file) {
      return;
    }
    let text: string;
    try {
      text = await this.ops.entryText(entry);
    } catch (e) {
      this.alert.addServerFailure(e);
      return;
    }
    const sameKind = (f: ReturnType<typeof fileOfType>) => !!f && f.kind === file.kind && f.protocol === file.protocol;
    const taken = [
      ...this.entries().map(e => fileOfType(e['configurationType'])).filter(sameKind).map(f => f!.name),
      ...(file.kind === 'library' ? packagedLibraries().filter(l => l.protocol === file.protocol).map(l => l.name) : []),
    ];
    const fileName = copyFileName(file.name, taken);
    this.pending.hand({
      kind: file.kind,
      fileName,
      text: duplicateText(file.kind, text, fileName),
      name: `${entry['name']} copy`,
      description: entry['description'] ?? '',
      deviceType: entry['deviceType'] ?? '',
      copyOf: entry['name'],
    });
    void this.router.navigate(['/ot-connectors', file.kind === 'library' ? 'new-library' : 'new']);
  }

  upload(kind: FileKind): void {
    this.bsModal.show(UploadFileComponent, {
      initialState: { kind, onFinished: () => void this.load() },
      class: 'modal-lg',
      ignoreBackdropClick: true,
    });
  }

  sendToGateway(entry: IManagedObject): void {
    this.bsModal.show(SendToGatewayComponent, { initialState: { entry }, class: 'modal-lg', ignoreBackdropClick: true });
  }

  sendToGroup(entry: IManagedObject): void {
    this.bsModal.show(SendToGroupComponent, { initialState: { entry }, class: 'modal-lg', ignoreBackdropClick: true });
  }

  async remove(entry: IManagedObject): Promise<void> {
    const file = fileOfType(entry['configurationType']);
    const isLibrary = file?.kind === 'library';
    let message = `${entry['name']}: the repository entry and its file are deleted. Gateways keep their copy.`;
    if (isLibrary && file?.protocol) {
      try {
        const referencing = await this.referencingAfterDelete(entry, file.protocol, file.name);
        if (referencing.length) {
          message +=
            ` These connector entries reference the library: ${referencing.map(e => e['name']).join(', ')}.` +
            ' They cannot be saved again until the reference is removed.';
        }
      } catch (e) {
        this.alert.addServerFailure(e);
        return;
      }
    }
    const confirmed = await this.modal
      .confirm(isLibrary ? gettext('Delete point library') : gettext('Delete connector file'), message, Status.DANGER, {
        ok: gettext('Delete'),
        cancel: gettext('Cancel'),
      })
      .catch(() => false);
    if (!confirmed) {
      return;
    }
    try {
      await this.repository.delete(entry);
      this.alert.success(isLibrary ? gettext('Point library deleted.') : gettext('Connector file deleted.'));
      await this.load();
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  /**
   * The connector entries that would reference a library no longer known once `entry` is gone:
   * none while another entry of the same type or a packaged library of that name remains.
   */
  private async referencingAfterDelete(entry: IManagedObject, protocol: string, name: string): Promise<IManagedObject[]> {
    const others = this.libraries().filter(e => e.id !== entry.id && e['configurationType'] === entry['configurationType']);
    const packaged = packagedLibraries().some(l => l.protocol === protocol && l.name === name);
    if (others.length || packaged) {
      return [];
    }
    return this.ops.connectorsReferencing(protocol, name);
  }
}
