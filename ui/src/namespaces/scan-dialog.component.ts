import { Component, OnInit, computed, inject, signal } from '@angular/core';
import { IManagedObject, InventoryService } from '@c8y/client';
import { CoreModule } from '@c8y/ngx-components';
import { BsModalRef } from 'ngx-bootstrap/modal';

import { type ExistingScan, type ScanScope, canScan, currentScan, scanDevices, scopeProblem } from '../shared/namespace-scan';
import { NamespaceService } from '../shared/namespace.service';
import { scanGatewaysQuery } from '../shared/queries';

type State = 'pick' | 'running' | 'done' | 'error';

const NODE_CLASSES = ['Variable', 'Method', 'Object'];

/**
 * Asks a gateway to scan the address space of one of its OPC UA devices with
 * c8y_ScanAddressSpace, waits for the operation and reports the new namespace file. Opened from
 * the OT connectors page (pick a gateway) or from a gateway's tab (`gateway` set).
 */
@Component({
  selector: 'tdot-scan-dialog',
  imports: [CoreModule],
  template: `
    <div class="modal-header dialog-header">
      <i c8yIcon="search"></i>
      <h4>{{ 'Scan address space' | translate }}</h4>
    </div>
    <div class="modal-body">
      @switch (state()) {
        @case ('pick') {
          @if (!gateway) {
            <c8y-form-group>
              <label for="tdot-scan-gw">{{ 'Gateway' | translate }}</label>
              <input id="tdot-scan-gw" class="form-control" [(ngModel)]="term" (ngModelChange)="search($event)"
                [placeholder]="'Search gateways that can scan, by name' | translate" />
            </c8y-form-group>
            <div class="list-group">
              @for (g of gateways(); track g.id) {
                <button type="button" class="list-group-item" [class.active]="picked()?.id === g.id" (click)="pick(g)">{{ g['name'] }}</button>
              } @empty {
                <p class="text-muted">
                  {{ 'No gateway announces c8y_ScanAddressSpace. It needs tedge-dot with the scan operation and opcua-scan.' | translate }}
                </p>
              }
            </div>
          }
          @if (picked()) {
            <c8y-form-group>
              <label for="tdot-scan-device">{{ 'OPC UA device' | translate }}</label>
              <select id="tdot-scan-device" class="form-control" [ngModel]="device()" (ngModelChange)="device.set($event)">
                @for (d of devices(); track d.id) {
                  <option [value]="d['name']">{{ d['name'] }}</option>
                }
              </select>
              <c8y-messages [helpMessage]="'The scan connects with this device\\'s settings from its connector file on the gateway. A successful scan replaces the device\\'s current namespace file.' | translate"></c8y-messages>
            </c8y-form-group>
            @if (existing(); as e) {
              <div class="alert alert-info d-flex a-i-center gap-16" role="status">
                <span class="flex-grow">
                  {{ device() }} {{ 'already has a namespace file from this gateway' | translate }}:
                  {{ 'scanned' | translate }} {{ (e.meta?.scannedAt ?? e.lastUpdated) | c8yDate }}@if (e.meta?.nodes !== undefined) {, {{ e.meta?.nodes }} {{ 'nodes' | translate }}}@if (e.meta?.points !== undefined) {, {{ e.meta?.points }} {{ 'points' | translate }}}.
                  {{ 'Scanning again replaces it once the new scan succeeds; a failed scan keeps it.' | translate }}
                </span>
                <button type="button" class="btn btn-default btn-sm" (click)="openExisting(e.id)">{{ 'Open it instead' | translate }}</button>
              </div>
            }
            <div class="row">
              <div class="col-sm-6">
                <c8y-form-group>
                  <label for="tdot-scan-root">{{ 'Start at node' | translate }}</label>
                  <input id="tdot-scan-root" class="form-control" placeholder="i=85" [(ngModel)]="scope.root" />
                  <c8y-messages [helpMessage]="'Empty: Objects' | translate"></c8y-messages>
                </c8y-form-group>
              </div>
              <div class="col-sm-3">
                <c8y-form-group>
                  <label for="tdot-scan-depth">{{ 'Depth' | translate }}</label>
                  <input id="tdot-scan-depth" class="form-control" [placeholder]="'unlimited' | translate" [(ngModel)]="scope.depth" />
                </c8y-form-group>
              </div>
              <div class="col-sm-3">
                <c8y-form-group>
                  <label for="tdot-scan-max">{{ 'Children per node' | translate }}</label>
                  <input id="tdot-scan-max" class="form-control" [placeholder]="'no limit' | translate" [(ngModel)]="scope.maxChildren" />
                </c8y-form-group>
              </div>
            </div>
            <div class="row">
              <div class="col-sm-6">
                <c8y-form-group>
                  <label for="tdot-scan-include">{{ 'Include' | translate }}</label>
                  <textarea id="tdot-scan-include" class="form-control" rows="3" placeholder="Objects/Factory/**" [(ngModel)]="scope.include"></textarea>
                  <c8y-messages [helpMessage]="'One browse-path pattern per line; * within a name, ** across levels.' | translate"></c8y-messages>
                </c8y-form-group>
              </div>
              <div class="col-sm-6">
                <c8y-form-group>
                  <label for="tdot-scan-exclude">{{ 'Exclude' | translate }}</label>
                  <textarea id="tdot-scan-exclude" class="form-control" rows="3" placeholder="**/Diagnostics/**" [(ngModel)]="scope.exclude"></textarea>
                </c8y-form-group>
              </div>
            </div>
            <c8y-form-group>
              <label>{{ 'Node classes' | translate }}</label>
              <div>
                @for (c of nodeClasses; track c) {
                  <label class="c8y-checkbox checkbox-inline">
                    <input type="checkbox" [checked]="types().includes(c)" (change)="toggleType(c)" />
                    <span></span>
                    <span>{{ c }}</span>
                  </label>
                }
              </div>
              <c8y-messages [helpMessage]="'The objects above them are always kept.' | translate"></c8y-messages>
            </c8y-form-group>
            @if (problem(); as p) {
              <div class="alert alert-warning" role="alert">{{ p }}</div>
            }
          }
        }
        @case ('running') {
          <p><i c8yIcon="spinner" class="icon-spin"></i> {{ 'Scanning' | translate }} {{ device() }} ({{ status() }})…</p>
          <p class="text-muted">{{ 'Large address spaces take minutes. You can close this dialog; the scan goes on and its file appears in the list.' | translate }}</p>
        }
        @case ('done') {
          <div class="alert alert-success" role="status">{{ 'The namespace file is ready.' | translate }}</div>
        }
        @case ('error') {
          <div class="alert alert-danger" role="alert">{{ error() }}</div>
        }
      }
    </div>
    <div class="modal-footer">
      @if (state() === 'pick') {
        <button type="button" class="btn btn-default" (click)="close()">{{ 'Cancel' | translate }}</button>
        <button type="button" class="btn btn-primary" [disabled]="!canStart()" (click)="start()">{{ (existing() ? 'Scan again' : 'Scan') | translate }}</button>
      } @else if (state() === 'done') {
        <button type="button" class="btn btn-default" (click)="close()">{{ 'Close' | translate }}</button>
        <button type="button" class="btn btn-primary" (click)="open()">{{ 'Create from namespace' | translate }}</button>
      } @else {
        <button type="button" class="btn btn-default" (click)="close()">{{ 'Close' | translate }}</button>
      }
    </div>
  `,
})
export class ScanDialogComponent implements OnInit {
  private readonly inventory = inject(InventoryService);
  private readonly namespaces = inject(NamespaceService);
  private readonly modalRef = inject(BsModalRef);

  /** Set by the opener: the gateway to scan on; else the dialog asks. */
  gateway?: IManagedObject;
  /** Set by the opener: the device to preselect when the gateway has it. */
  preferredDevice?: string;
  /** Set by the opener: called with the new namespace file's id. */
  onScanned?: (id: string) => void;
  /** Set by the opener: opens a namespace file in the picker. */
  onOpen?: (id: string) => void;

  term = '';
  scope: ScanScope = {};
  readonly nodeClasses = NODE_CLASSES;
  readonly types = signal<string[]>(['Variable', 'Method']);
  readonly gateways = signal<IManagedObject[]>([]);
  readonly picked = signal<IManagedObject | undefined>(undefined);
  readonly devices = signal<IManagedObject[]>([]);
  readonly device = signal('');
  readonly state = signal<State>('pick');
  readonly status = signal('PENDING');
  readonly error = signal('');
  readonly resultId = signal<string | undefined>(undefined);
  readonly problem = signal<string | undefined>(undefined);
  readonly canStart = computed(() => !!this.picked() && !!this.device());
  /** The picked gateway's namespace files, to tell when the device already has one. */
  readonly stored = signal<ExistingScan[]>([]);
  /** The namespace file a scan of the selected device would replace. */
  readonly existing = computed(() => {
    const gateway = this.picked();
    return gateway ? currentScan(this.stored(), String(gateway.id), this.device()) : undefined;
  });

  ngOnInit(): void {
    if (this.gateway) {
      void this.pick(this.gateway);
    } else {
      void this.search('');
    }
  }

  async search(term: string): Promise<void> {
    const { data } = await this.inventory.list({ query: scanGatewaysQuery(term), pageSize: 20, withParents: false });
    this.gateways.set(data.filter(canScan));
  }

  async pick(gateway: IManagedObject): Promise<void> {
    this.picked.set(gateway);
    this.stored.set([]);
    void this.namespaces
      .list(String(gateway.id))
      .then(entries => {
        if (this.picked()?.id === gateway.id) {
          this.stored.set(entries.map(e => ({ id: String(e.mo.id), lastUpdated: e.mo['lastUpdated'], meta: e.meta })));
        }
      })
      .catch(() => undefined);
    const { data } = await this.inventory.childDevicesList(gateway.id, { pageSize: 2000 });
    const devices = scanDevices(data as (IManagedObject & { name?: string })[]);
    this.devices.set(devices);
    const preferred = devices.find(d => d['name'] === this.preferredDevice);
    this.device.set((preferred ?? devices[0])?.['name'] ?? '');
  }

  toggleType(c: string): void {
    this.types.update(t => (t.includes(c) ? t.filter(x => x !== c) : [...t, c]));
  }

  async start(): Promise<void> {
    const scope = { ...this.scope, types: this.types() };
    const problem = scopeProblem(scope);
    this.problem.set(problem);
    if (problem || !this.picked()) {
      return;
    }
    this.state.set('running');
    try {
      const id = await this.namespaces.scan(this.picked()!, this.device(), scope, s => this.status.set(s));
      this.resultId.set(id);
      this.state.set('done');
      this.onScanned?.(id);
    } catch (e) {
      this.error.set(e instanceof Error ? e.message : String(e));
      this.state.set('error');
    }
  }

  openExisting(id: string): void {
    this.modalRef.hide();
    this.onOpen?.(id);
  }

  open(): void {
    const id = this.resultId();
    this.modalRef.hide();
    if (id) {
      this.onOpen?.(id);
    }
  }

  close(): void {
    this.modalRef.hide();
  }
}
