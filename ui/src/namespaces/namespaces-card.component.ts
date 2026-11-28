import { Component, OnInit, inject, input, signal } from '@angular/core';
import { Router } from '@angular/router';
import { IManagedObject } from '@c8y/client';
import { AlertService, CoreModule, ModalService, Permissions, Status } from '@c8y/ngx-components';
import { gettext } from '@c8y/ngx-components/gettext';
import { BsModalService } from 'ngx-bootstrap/modal';

import { protocolInfo } from '../generated/config-builder';
import { canScan } from '../shared/namespace-scan';
import { type NamespaceEntry, NamespaceService } from '../shared/namespace.service';
import { canAddEntries } from '../shared/permissions';
import { ScanDialogComponent } from './scan-dialog.component';
import { UploadNamespaceComponent } from './upload-namespace.component';

/**
 * The namespace files: scans of OPC UA address spaces stored in Cumulocity. On the OT connectors
 * page every file; on a gateway's tab (`gateway` set) the files its scans produced.
 */
@Component({
  selector: 'tdot-namespaces-card',
  imports: [CoreModule],
  template: `
    <div class="card">
      <div class="card-header separator d-flex a-i-center">
        <div class="card-title flex-grow">{{ 'Namespaces' | translate }}</div>
        @if (scanAllowed()) {
          <button class="btn btn-link btn-sm" type="button" (click)="scan()">
            <i c8yIcon="search"></i> {{ 'Scan address space' | translate }}
          </button>
        }
        @if (canAdd && !gateway()) {
          <button class="btn btn-link btn-sm" type="button" (click)="upload()">
            <i c8yIcon="upload"></i> {{ 'Upload namespace file' | translate }}
          </button>
        }
      </div>
      @if (!loading() && entries().length === 0) {
        <c8y-ui-empty-state
          [icon]="'sitemap'"
          [title]="'No namespace files.' | translate"
          [subtitle]="emptyText() | translate"
          [horizontal]="true"
        ></c8y-ui-empty-state>
      } @else {
        <div class="table-responsive">
          <table class="table large-padding">
            <thead>
              <tr>
                <th>{{ 'Device' | translate }}</th>
                <th>{{ 'Protocol' | translate }}</th>
                <th>{{ 'Server' | translate }}</th>
                @if (!gateway()) {
                  <th>{{ 'Gateway' | translate }}</th>
                }
                <th class="text-right">{{ 'Nodes' | translate }}</th>
                <th class="text-right">{{ 'Points' | translate }}</th>
                <th>{{ 'Scanned' | translate }}</th>
                <th class="text-right">{{ 'Actions' | translate }}</th>
              </tr>
            </thead>
            <tbody>
              @for (e of entries(); track e.mo.id) {
                <tr>
                  <td>
                    <a class="interact" (click)="open(e)">{{ e.meta?.device ?? e.mo['name'] }}</a>
                    @if (e.meta?.connectorFile) {
                      <span class="text-muted small"> · {{ e.meta?.connectorFile }}.toml</span>
                    }
                  </td>
                  <td>{{ protocolLabel(e.meta?.protocol) }}</td>
                  <td>
                    {{ e.meta?.endpoint }}
                    @if (e.meta?.productName) {
                      <div class="text-muted small">{{ e.meta?.productName }}</div>
                    }
                  </td>
                  @if (!gateway()) {
                    <td>{{ e.meta?.gateway }}</td>
                  }
                  <td class="text-right">
                    {{ e.meta?.nodes }}
                    @if (e.meta?.truncated) {
                      <span class="tag tag--warning m-l-4" [title]="'The scan did not browse every branch.' | translate">{{ 'truncated' | translate }}</span>
                    }
                  </td>
                  <td class="text-right">{{ e.meta?.points }}</td>
                  <td>{{ (e.meta?.scannedAt ?? e.mo['lastUpdated']) | c8yDate }}</td>
                  <td class="text-right text-nowrap">
                    <button class="btn btn-default btn-xs m-r-4" type="button" (click)="open(e)">{{ 'Create from namespace' | translate }}</button>
                    <button class="btn btn-default btn-xs m-r-4" type="button" (click)="download(e)">{{ 'Download' | translate }}</button>
                    <button class="btn btn-danger btn-xs" type="button" (click)="remove(e)">{{ 'Delete' | translate }}</button>
                  </td>
                </tr>
              }
            </tbody>
          </table>
        </div>
      }
    </div>
  `,
})
export class NamespacesCardComponent implements OnInit {
  private readonly namespaces = inject(NamespaceService);
  private readonly router = inject(Router);
  private readonly bsModal = inject(BsModalService);
  private readonly modal = inject(ModalService);
  private readonly alert = inject(AlertService);
  private readonly permissions = inject(Permissions);

  /** On a gateway's tab: the gateway. */
  readonly gateway = input<IManagedObject | undefined>(undefined);

  readonly entries = signal<NamespaceEntry[]>([]);
  readonly loading = signal(true);
  readonly canAdd = canAddEntries(roles => this.permissions.hasAnyRole(roles));
  readonly scanAllowed = () => {
    const gateway = this.gateway();
    return this.permissions.hasAnyRole(['ROLE_DEVICE_CONTROL_ADMIN']) && (!gateway || canScan(gateway));
  };

  /** What to do next, for the context: the page, a gateway that can scan, or one that cannot. */
  readonly emptyText = () => {
    const gateway = this.gateway();
    if (!gateway) {
      return 'Scan an OPC UA device from a gateway, or upload a file from opcua-scan, then pick nodes to build a point library.';
    }
    return canScan(gateway)
      ? 'Scan an OPC UA device of this gateway, then pick nodes to build a point library.'
      : 'This gateway cannot scan yet: it needs a tedge-dot version with the c8y_ScanAddressSpace operation, and opcua-scan.';
  };

  /** The protocol's name as the forms show it, e.g. "OPC UA"; files without metadata are OPC UA scans. */
  protocolLabel(protocol: string | undefined): string {
    return protocolInfo(protocol ?? 'opcua').label;
  }

  ngOnInit(): void {
    void this.load();
  }

  async load(): Promise<void> {
    this.loading.set(true);
    try {
      this.entries.set(await this.namespaces.list(this.gateway()?.id === undefined ? undefined : String(this.gateway()!.id)));
    } catch (e) {
      this.alert.addServerFailure(e);
    } finally {
      this.loading.set(false);
    }
  }

  open(entry: NamespaceEntry | string): void {
    const id = typeof entry === 'string' ? entry : String(entry.mo.id);
    void this.router.navigate(['/ot-connectors', 'namespace', id]);
  }

  scan(): void {
    this.bsModal.show(ScanDialogComponent, {
      initialState: { gateway: this.gateway(), onScanned: () => void this.load(), onOpen: (id: string) => this.open(id) },
      class: 'modal-lg',
      ignoreBackdropClick: true,
    });
  }

  upload(): void {
    this.bsModal.show(UploadNamespaceComponent, {
      initialState: { onFinished: () => void this.load() },
      ignoreBackdropClick: true,
    });
  }

  async download(entry: NamespaceEntry): Promise<void> {
    try {
      const bytes = await this.namespaces.bytes(String(entry.mo.id));
      const url = URL.createObjectURL(new Blob([bytes as BlobPart], { type: 'application/gzip' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = `${(entry.meta?.device ?? 'namespace').replace(/[^A-Za-z0-9._-]/g, '_')}-${String(entry.meta?.scannedAt ?? '').replace(/[:]/g, '')}.json.gz`;
      a.click();
      URL.revokeObjectURL(url);
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  async remove(entry: NamespaceEntry): Promise<void> {
    const confirmed = await this.modal
      .confirm(
        gettext('Delete namespace file'),
        `The scan of ${entry.meta?.device ?? entry.mo['name']} from ${entry.meta?.scannedAt ?? entry.mo['lastUpdated']} is deleted. Point libraries made from it stay.`,
        Status.DANGER,
        { ok: gettext('Delete'), cancel: gettext('Cancel') }
      )
      .catch(() => false);
    if (!confirmed) {
      return;
    }
    try {
      await this.namespaces.delete(String(entry.mo.id));
      this.alert.success(gettext('Namespace file deleted.'));
      await this.load();
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }
}
