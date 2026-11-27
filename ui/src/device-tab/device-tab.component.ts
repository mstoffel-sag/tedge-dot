import { Component, OnInit, inject, signal, viewChild } from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import { ActivatedRoute, Router } from '@angular/router';
import { IManagedObject, InventoryService } from '@c8y/client';
import { CoreModule, ResizableGridComponent } from '@c8y/ngx-components';
import { gettext } from '@c8y/ngx-components/gettext';
import {
  ConfigurationPreviewComponent,
  ConfigurationRepositoryDeviceTabModule,
  DeviceConfigurationListComponent,
  DeviceConfigurationService,
} from '@c8y/ngx-components/repository/configuration';
import {
  ConfigurationSnapshot,
  DeviceConfigurationOperation,
  RepositoryService,
  SupportedConfigurationItem,
} from '@c8y/ngx-components/repository/shared';
import { BsModalService } from 'ngx-bootstrap/modal';

import { SendToGatewayComponent } from '../connectors/send-to-gateway.component';
import { NamespacesCardComponent } from '../namespaces/namespaces-card.component';
import { connectorTypes, libraryTypes } from '../shared/gateway';

/**
 * The OT connectors tab of a thin-edge.io gateway: Device Management's Configuration tab, limited
 * to connector files and, in a second list, point libraries, plus adding a file from the
 * repository and registering one that is already there.
 */
@Component({
  selector: 'tdot-device-tab',
  imports: [
    CoreModule,
    ConfigurationRepositoryDeviceTabModule,
    DeviceConfigurationListComponent,
    ConfigurationPreviewComponent,
    ResizableGridComponent,
    NamespacesCardComponent,
  ],
  template: `
    <c8y-action-bar-item [placement]="'right'">
      <button class="btn btn-link" type="button" (click)="add()">
        <i c8yIcon="plus-circle"></i>
        {{ 'Add connector file' | translate }}
      </button>
    </c8y-action-bar-item>
    <c8y-action-bar-item [placement]="'right'">
      <button class="btn btn-link" type="button" (click)="register()">
        <i c8yIcon="link"></i>
        {{ 'Register a file on the gateway' | translate }}
      </button>
    </c8y-action-bar-item>
    <c8y-action-bar-item [placement]="'right'">
      <button class="btn btn-link" type="button" (click)="reload()">
        <i c8yIcon="refresh" [class.icon-spin]="reloading()"></i>
        {{ 'Reload' | translate }}
      </button>
    </c8y-action-bar-item>

    <div class="card content-fullpage">
      <div class="card-header separator">
        <div class="card-title">{{ 'Connector files and point libraries' | translate }}</div>
      </div>
      <c8y-resizable-grid
        [orientation]="'vertical'"
        [trackId]="'tdot-device-ot-connectors-split-view'"
        [leftColumnWidth]="'50%'"
        [collapsible]="false"
      >
        <div class="d-flex d-col fit-h min-height-0" left-pane>
          <div class="p-l-16 p-r-16">
            <h5 class="legend form-block"><span>{{ 'On device' | translate }}</span></h5>
          </div>
          <div class="card--grid grid__col--4-8--md m-b-0 flex-grow min-height-0">
            <div class="card--grid__inner-scroll bg-level-1">
              <c8y-device-configuration-list
                #connectorList
                [itemIcon]="'plug'"
                [items]="items()"
                [emptyState]="deviceEmptyState"
                (configSelected)="onTypeSelected($event, 'connector')"
              ></c8y-device-configuration-list>
              @if (libraryItems().length) {
                <p class="text-label-small text-muted p-l-16 p-t-16 m-b-0">{{ 'Point libraries' | translate }}</p>
                <c8y-device-configuration-list
                  #libraryList
                  [itemIcon]="'list'"
                  [items]="libraryItems()"
                  [emptyState]="deviceEmptyState"
                  (configSelected)="onTypeSelected($event, 'library')"
                ></c8y-device-configuration-list>
              }
            </div>
            <div class="card--grid__inner-scroll d-flex d-col flex-grow min-height-0 bg-component">
              <div class="card-block d-flex d-col flex-grow min-height-0">
                @if (configurationType(); as type) {
                  <c8y-device-configuration-preview
                    class="d-flex d-col flex-grow min-height-0"
                    [actionButtonIcon]="'arrow-down'"
                    [device]="device()"
                    [configurationType]="type"
                    [configSnapshot]="configSnapshot()!"
                    [canSaveSnapshot]="true"
                    [operationToTrigger]="uploadConfig"
                    [actionButtonText]="'Get snapshot from device' | translate"
                  ></c8y-device-configuration-preview>
                } @else {
                  <fieldset class="c8y-fieldset p-l-16 p-r-16 p-b-16 flex-grow d-flex d-col min-height-0">
                    <legend>{{ 'Preview' | translate }}</legend>
                    <c8y-ui-empty-state
                      [icon]="'file-text'"
                      [title]="'No file selected.' | translate"
                      [subtitle]="'Select a connector file or point library to preview.' | translate"
                      [horizontal]="true"
                    ></c8y-ui-empty-state>
                  </fieldset>
                }
              </div>
            </div>
          </div>
        </div>

        <div class="d-flex d-col fit-h min-height-0" right-pane>
          <div class="p-l-16 p-r-16">
            <h5 class="legend form-block"><span>{{ 'Available from repository' | translate }}</span></h5>
          </div>
          <div class="card--grid grid__col--4-8--md m-b-0 flex-grow min-height-0">
            <div class="card--grid__inner-scroll bg-level-1">
              @if (!configurationType()) {
                <c8y-ui-empty-state
                  [icon]="'plug'"
                  [title]="'No file selected.' | translate"
                  [subtitle]="'Select a connector file or point library to see its repository entries.' | translate"
                  [horizontal]="true"
                ></c8y-ui-empty-state>
              } @else {
                <div class="p-r-16">
                  <c8y-device-configuration-list
                    [itemIcon]="'file-text'"
                    [items]="repositoryItems()"
                    [emptyState]="repositoryEmptyState"
                    [isFilterEnabled]="true"
                    (configSelected)="onEntrySelected($event)"
                  ></c8y-device-configuration-list>
                </div>
              }
            </div>
            <div class="card--grid__inner-scroll d-flex d-col flex-grow min-height-0 bg-component">
              <div class="card-block d-flex d-col flex-grow min-height-0">
                @if (repositorySnapshot(); as snapshot) {
                  <c8y-device-configuration-preview
                    class="d-flex d-col flex-grow min-height-0"
                    [actionButtonIcon]="'arrow-up'"
                    [device]="device()"
                    [configurationType]="configurationType()!"
                    [configSnapshot]="snapshot"
                    [operationToTrigger]="downloadConfig"
                    [actionButtonText]="'Send configuration to device' | translate"
                    [editableRepositoryMo]="selectedEntry()!"
                    (editInRepository)="editEntry($event)"
                  ></c8y-device-configuration-preview>
                } @else {
                  <fieldset class="c8y-fieldset p-l-16 p-r-16 p-b-16 flex-grow d-flex d-col min-height-0">
                    <legend>{{ 'Preview' | translate }}</legend>
                    <c8y-ui-empty-state
                      [icon]="'file-text'"
                      [title]="'No repository entry selected.' | translate"
                      [subtitle]="'Select an entry to preview.' | translate"
                      [horizontal]="true"
                    ></c8y-ui-empty-state>
                  </fieldset>
                }
              </div>
            </div>
          </div>
        </div>
      </c8y-resizable-grid>
    </div>
    @if (device().id) {
      <tdot-namespaces-card [gateway]="device()" />
    }
  `,
})
export class DeviceTabComponent implements OnInit {
  private readonly route = inject(ActivatedRoute);
  private readonly router = inject(Router);
  private readonly repository = inject(RepositoryService);
  private readonly inventory = inject(InventoryService);
  private readonly bsModal = inject(BsModalService);
  private readonly configurations = inject(DeviceConfigurationService);

  readonly uploadConfig = DeviceConfigurationOperation.UPLOAD_CONFIG;
  readonly downloadConfig = DeviceConfigurationOperation.DOWNLOAD_CONFIG;

  readonly device = signal<IManagedObject>({} as IManagedObject);
  readonly items = signal<SupportedConfigurationItem[]>([]);
  readonly libraryItems = signal<SupportedConfigurationItem[]>([]);
  private readonly connectorList = viewChild<DeviceConfigurationListComponent>('connectorList');
  private readonly libraryList = viewChild<DeviceConfigurationListComponent>('libraryList');
  readonly configurationType = signal<string | undefined>(undefined);
  readonly configSnapshot = signal<ConfigurationSnapshot | undefined>(undefined);
  readonly repositoryItems = signal<SupportedConfigurationItem[]>([]);
  readonly selectedEntry = signal<IManagedObject | undefined>(undefined);
  readonly repositorySnapshot = signal<ConfigurationSnapshot | undefined>(undefined);
  readonly reloading = signal(false);
  private repositoryEntries: IManagedObject[] = [];

  readonly deviceEmptyState = {
    icon: 'plug',
    title: gettext('No connector files on this gateway.'),
    text: gettext('Add a connector file, or register one that is already on the gateway.'),
  };
  readonly repositoryEmptyState = {
    icon: 'plug',
    title: gettext('No repository entries for this file.'),
    text: gettext('Add one under Management > OT connectors, or save a snapshot to the repository.'),
  };

  constructor() {
    // The previews report a finished snapshot upload, and "Save to repository" (repository entries
    // only), the way they do in Device Management's Configuration tab.
    this.configurations.configurationsUpdated
      .pipe(takeUntilDestroyed())
      .subscribe((repositoryOnly?: boolean) => void this.updateSnapshots(repositoryOnly));
  }

  ngOnInit(): void {
    const device: IManagedObject = this.route.snapshot.parent?.data['contextData'];
    this.device.set(device);
    this.showTypes(device);
  }

  /** The list emits the selected item, though its output is typed as a string. */
  async onTypeSelected(item: unknown, list: 'connector' | 'library'): Promise<void> {
    // One selection across both lists.
    const other = list === 'connector' ? this.libraryList() : this.connectorList();
    if (other) {
      other.selectedConfig = undefined;
    }
    this.configurationType.set(typeof item === 'string' ? item : (item as SupportedConfigurationItem).name);
    this.configSnapshot.set(undefined);
    await this.updateSnapshots();
  }

  async onEntrySelected(item: unknown): Promise<void> {
    const id = typeof item === 'string' ? undefined : (item as IManagedObject).id;
    const entry = this.repositoryEntries.find(e => e.id === id || e['name'] === item);
    if (!entry) {
      return;
    }
    this.selectedEntry.set(entry);
    const snapshot: ConfigurationSnapshot = {
      id: String(entry.id),
      time: entry['creationTime'],
      name: entry['name'],
      binaryUrl: entry['url'],
    };
    this.repositorySnapshot.set(snapshot);
    if (entry['url']) {
      try {
        const text = await this.repository.getBinaryText(entry['url'], { allowExternal: false });
        if (this.selectedEntry()?.id === entry.id) {
          this.repositorySnapshot.set({ ...snapshot, binary: text });
        }
      } catch {
        // The preview shows the entry without content.
      }
    }
  }

  editEntry(entry: IManagedObject): void {
    void this.router.navigate(['/ot-connectors', entry.id]);
  }

  async reload(): Promise<void> {
    this.reloading.set(true);
    try {
      const { data } = await this.inventory.detail(this.device().id);
      this.device.set(data);
      this.showTypes(data);
      if (this.configurationType()) {
        await this.updateSnapshots();
      }
    } finally {
      this.reloading.set(false);
    }
  }

  add(): void {
    this.openDialog(false);
  }

  /** The connector file types and the point library types the gateway offers. */
  private showTypes(device: IManagedObject): void {
    this.items.set(connectorTypes(device).map(name => ({ name })));
    this.libraryItems.set(libraryTypes(device).map(name => ({ name })));
  }

  register(): void {
    this.openDialog(true);
  }

  private openDialog(registerOnly: boolean): void {
    this.bsModal.show(SendToGatewayComponent, {
      initialState: { device: this.device(), registerOnly, onFinished: () => void this.reload() },
      class: 'modal-lg',
      ignoreBackdropClick: true,
    });
  }

  private async updateSnapshots(repositoryOnly = false): Promise<void> {
    const type = this.configurationType();
    if (!type) {
      return;
    }
    const device = this.device();
    this.selectedEntry.set(undefined);
    this.repositorySnapshot.set(undefined);
    // The SDK also returns entries without a configuration type, which the Configuration tab
    // offers for any type. Here they would replace a connector file with unrelated content.
    const entries = await this.repository.getSnapshotsFromRepository(device, type);
    if (this.configurationType() !== type) {
      return;
    }
    this.repositoryEntries = entries.filter(e => e['configurationType'] === type);
    this.repositoryItems.set(this.repositoryEntries.map(e => ({ ...e, name: e['name'] ?? '' })));
    if (repositoryOnly) {
      return;
    }
    const snapshot = await this.repository.getConfigSnapshot(device, type);
    if (this.configurationType() === type) {
      this.configSnapshot.set(snapshot);
    }
  }
}
