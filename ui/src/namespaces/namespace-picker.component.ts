import { ScrollingModule } from '@angular/cdk/scrolling';
import { Component, computed, inject, signal } from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import { ActivatedRoute, Router } from '@angular/router';
import { IManagedObject, InventoryService } from '@c8y/client';
import { AlertService, CoreModule } from '@c8y/ngx-components';
import { SharedRepositoryModule } from '@c8y/ngx-components/repository/shared';

import { type Config, parseToml, protocolInfo } from '../generated/config-builder';
import { FormHelpComponent } from '../connectors/connector-form.component';
import { HELP } from '../generated/help';
import { ConnectorOperationsService } from '../shared/connector-operations.service';
import { packagedLibraries } from '../shared/libraries';
import { NODE_CLASSES, type NamespaceIndex, type NodeClass, type TreeFilter, indexNamespace, treeRows, visibleNodes, whyNoPoints } from '../shared/namespace-file';
import {
  type PointChange,
  type Selection,
  type TargetMatch,
  type TargetPoints,
  changePoints,
  checkState,
  checkedPoints,
  clashingIds,
  deviceTarget,
  initialSelection,
  isMethodPoint,
  libraryTarget,
  matchTarget,
  newLibraryText,
  resultPoints,
  toggleNode,
  writeDevice,
  writeLibrary,
} from '../shared/namespace-points';
import { NamespaceService } from '../shared/namespace.service';
import { entriesOfKind, fileOfType, isValidName, normalizeName } from '../shared/naming';
import { PendingFileService } from '../shared/pending-file.service';
import { type PickerContext, type StoredNamespace, scansFor, targetForScan } from '../shared/namespace-context';
import { METADATA_FRAGMENT, type NamespaceMetadata } from '../shared/namespace-file';
import { PickerContextService } from '../shared/picker-context.service';
import { ScanDialogComponent } from './scan-dialog.component';
import { UploadNamespaceComponent } from './upload-namespace.component';
import { BsModalService } from 'ngx-bootstrap/modal';
import { managedEntriesQuery } from '../shared/queries';

type TargetKind = 'new-library' | 'library' | 'device' | 'editor';

interface ConnectorChoice {
  entry: IManagedObject;
  text: string;
  devices: string[];
}

/**
 * Create from namespace: a namespace file as a tree. Users check nodes or subtrees, change the
 * proposed points, and choose where they go: a new point library, an existing one, or a device
 * of a connector file. The result opens in the editor, unsaved, where it is checked as usual.
 */
@Component({
  selector: 'tdot-namespace-picker',
  imports: [CoreModule, SharedRepositoryModule, ScrollingModule, FormHelpComponent],
  providers: [ConnectorOperationsService],
  styles: `
    .tdot-tree, .tdot-points { height: 60vh; }
    .tdot-row { height: 30px; display: flex; align-items: center; white-space: nowrap; overflow: hidden; cursor: default; }
    .tdot-row.active { background: var(--c8y-root-component-background-active, rgba(0, 0, 0, 0.06)); }
    .tdot-chevron { width: 20px; display: inline-block; text-align: center; cursor: pointer; }
    .tdot-point { height: 40px; display: flex; align-items: center; gap: 4px; }
    .tdot-point input, .tdot-point select { height: 28px; padding: 2px 6px; }
    .tdot-clash input.tdot-id { border-color: var(--c8y-palette-status-danger, #d00); }
    .tdot-point-head { height: 28px; border-bottom: 1px solid var(--c8y-root-component-border-color, #e0e0e0); }
    .tdot-col-mark { flex: 0 0 24px; }
    .tdot-col-id { flex: 0 0 30%; }
    .tdot-col-name { flex: 0 0 25%; }
    .tdot-col-unit { flex: 0 0 10%; }
    .tdot-col-info { flex: 1 1 auto; min-width: 0; }
    .tdot-col-x { flex: 0 0 28px; }
    .tdot-bulk-field { display: flex; flex-direction: column; }
    .tdot-bulk-label { display: flex; align-items: center; gap: 2px; margin-bottom: 2px; }
    .tdot-bulk-label label { margin: 0; font-size: 12px; }
    .tdot-bulk-field select, .tdot-bulk-field input { width: 9em; }
  `,
  template: `
    <c8y-title>{{ 'Create from namespace' | translate }}</c8y-title>
    <c8y-breadcrumb>
      <c8y-breadcrumb-item [icon]="'plug'" [label]="'OT connectors' | translate" [path]="'/ot-connectors'"></c8y-breadcrumb-item>
      <c8y-breadcrumb-item [label]="'Create from namespace' | translate"></c8y-breadcrumb-item>
    </c8y-breadcrumb>

    @if (loadError(); as e) {
      <div class="alert alert-danger" role="alert">{{ e }}</div>
    }
    @if (context(); as ctx) {
      <div class="card">
        <div class="card-block d-flex a-i-center gap-16 flex-wrap">
          <span class="flex-grow">
            {{ 'Adding points to' | translate }} <strong>{{ ctx.name || ctx.fileName }}</strong>&nbsp;<span class="text-muted">({{ ctx.fileName }}.toml{{ ctx.device ? ', device ' + ctx.device : '' }})</span>.
            {{ 'Your unsaved changes in the editor are kept.' | translate }}
          </span>
          @if (scans().length) {
            <label for="tdot-ns-scan" class="m-0">{{ 'Namespace file' | translate }}</label>
            <select id="tdot-ns-scan" class="form-control" style="max-width: 24em" [ngModel]="id" (ngModelChange)="openScan($event)">
              @for (s of scans(); track s.id) {
                <option [value]="s.id">{{ scanLabel(s) }}</option>
              }
            </select>
          }
          <button class="btn btn-default btn-sm" type="button" (click)="scan()"><i c8yIcon="search"></i> {{ 'Scan address space' | translate }}</button>
          <button class="btn btn-default btn-sm" type="button" (click)="upload()"><i c8yIcon="upload"></i> {{ 'Upload namespace file' | translate }}</button>
        </div>
      </div>
      @if (noScan()) {
        <div class="card">
          <c8y-ui-empty-state
            [icon]="'sitemap'"
            [title]="(ctx.device ? 'No namespace file of this device yet.' : 'No namespace files yet.') | translate"
            [subtitle]="'Scan the device\\'s server from its gateway, or upload a file from opcua-scan.' | translate"
            [horizontal]="true"
          ></c8y-ui-empty-state>
          <div class="card-block">
            <button class="btn btn-default" type="button" (click)="cancel()">{{ 'Back to the editor' | translate }}</button>
          </div>
        </div>
      }
    }
    @if (index(); as ix) {
      <div class="card">
        <div class="card-block">
          <p class="m-b-0">
            <strong>{{ ix.file.scan.device ?? ix.file.server.endpoint }}</strong>
            · {{ ix.file.server.endpoint }}
            @if (ix.file.server.product_name) {
              · {{ ix.file.server.product_name }}
            }
            · {{ ix.file.nodes.length }} {{ 'nodes' | translate }} · {{ 'scanned' | translate }} {{ ix.file.scan.time | c8yDate }}
          </p>
        </div>
      </div>

      <div class="card">
        <div class="card-header separator"><div class="card-title">{{ 'Target' | translate }}</div></div>
        <div class="card-block">
          @if (context(); as ctx) {
            <p>
              {{ 'The file you are editing' | translate }}: <strong>{{ ctx.name || ctx.fileName }}</strong>&nbsp;<span class="text-muted">({{ ctx.fileName }}.toml)</span>
            </p>
            @if (ctx.kind === 'connector') {
              <c8y-form-group>
                <label for="tdot-ns-ctx-device">{{ 'Device' | translate }}</label>
                <select id="tdot-ns-ctx-device" class="form-control" style="max-width: 24em" [ngModel]="targetDevice()" (ngModelChange)="chooseContextDevice(+$event)">
                  @for (d of contextDevices(); track $index) {
                    <option [value]="$index">{{ d }}</option>
                  }
                </select>
              </c8y-form-group>
            }
          } @else {
          <div class="form-group">
            <label class="c8y-radio radio-inline">
              <input type="radio" name="tdot-target" [checked]="targetKind() === 'new-library'" (change)="setTargetKind('new-library')" />
              <span></span><span>{{ 'New point library' | translate }}</span>
            </label>
            <label class="c8y-radio radio-inline">
              <input type="radio" name="tdot-target" [checked]="targetKind() === 'library'" (change)="setTargetKind('library')" />
              <span></span><span>{{ 'Existing point library' | translate }}</span>
            </label>
            <label class="c8y-radio radio-inline">
              <input type="radio" name="tdot-target" [checked]="targetKind() === 'device'" (change)="setTargetKind('device')" />
              <span></span><span>{{ 'Device in a connector file' | translate }}</span>
            </label>
          </div>
          @switch (targetKind()) {
            @case ('new-library') {
              <div class="row">
                <div class="col-sm-4">
                  <c8y-form-group [hasError]="!!libraryNameProblem()">
                    <label for="tdot-ns-lib">{{ 'Library name' | translate }}</label>
                    <input id="tdot-ns-lib" class="form-control" [ngModel]="libraryName()" (ngModelChange)="libraryName.set($event)" placeholder="acme-line" />
                    <c8y-messages [helpMessage]="libraryNameProblem() ?? ('Installs as points.d/opcua/<name>.toml' | translate)"></c8y-messages>
                  </c8y-form-group>
                </div>
                <div class="col-sm-4">
                  <c8y-form-group>
                    <label for="tdot-ns-type">{{ 'Device type' | translate }}</label>
                    <input id="tdot-ns-type" class="form-control" [ngModel]="libraryType()" (ngModelChange)="libraryType.set($event)" placeholder="acme-line-v1" />
                    <c8y-messages [helpMessage]="'[library] type: devices that use the library get it' | translate"></c8y-messages>
                  </c8y-form-group>
                </div>
              </div>
            }
            @case ('library') {
              <c8y-form-group>
                <label for="tdot-ns-target-lib">{{ 'Point library' | translate }}</label>
                <select id="tdot-ns-target-lib" class="form-control" [ngModel]="targetEntryId()" (ngModelChange)="chooseLibrary($event)">
                  <option value="">{{ ('Choose a library of this protocol' | translate) + ' (' + protocolLabel() + ')' }}</option>
                  @for (e of libraryEntries(); track e.id) {
                    <option [value]="e.id">{{ e['name'] }} ({{ fileOfType(e['configurationType'])?.name }})</option>
                  }
                </select>
              </c8y-form-group>
            }
            @case ('device') {
              <div class="row">
                <div class="col-sm-6">
                  <c8y-form-group>
                    <label for="tdot-ns-target-file">{{ 'Connector file' | translate }}</label>
                    <select id="tdot-ns-target-file" class="form-control" [ngModel]="targetEntryId()" (ngModelChange)="chooseConnector($event)">
                      <option value="">{{ ('Choose a connector file of this protocol' | translate) + ' (' + protocolLabel() + ')' }}</option>
                      @for (e of connectorEntries(); track e.id) {
                        <option [value]="e.id">{{ e['name'] }} ({{ fileOfType(e['configurationType'])?.name }}.toml)</option>
                      }
                    </select>
                  </c8y-form-group>
                </div>
                <div class="col-sm-6">
                  <c8y-form-group>
                    <label for="tdot-ns-target-device">{{ 'Device' | translate }}</label>
                    <select id="tdot-ns-target-device" class="form-control" [disabled]="!connector()" [ngModel]="targetDevice()" (ngModelChange)="chooseDevice(+$event)">
                      @for (d of connector()?.devices ?? []; track $index) {
                        <option [value]="$index">{{ d }}</option>
                      }
                    </select>
                  </c8y-form-group>
                </div>
              </div>
            }
          }
          }
          @if (outsideCount()) {
            <p class="text-muted m-b-0">
              {{ outsideCount() }} {{ (outsideCount() === 1 ? 'point of the target is not in this scan. It stays as it is.' : 'points of the target are not in this scan. They stay as they are.') | translate }}
            </p>
          }
        </div>
      </div>

      <div class="row">
        <div class="col-md-6">
          <div class="card">
            <div class="card-header separator"><div class="card-title">{{ 'Address space' | translate }}</div></div>
            <div class="card-block p-b-0">
              <div class="d-flex gap-8 m-b-8">
                <input class="form-control" [placeholder]="'Search name or path' | translate" [ngModel]="filterText()" (ngModelChange)="filterText.set($event)" />
                <select class="form-control" style="max-width: 10em" [ngModel]="filterClass()" (ngModelChange)="filterClass.set($event)">
                  <option value="">{{ 'All classes' | translate }}</option>
                  @for (c of nodeClasses; track c) {
                    <option [value]="c">{{ c }}</option>
                  }
                </select>
              </div>
              <div class="d-flex gap-16 m-b-8">
                <label class="c8y-checkbox checkbox-inline">
                  <input type="checkbox" [checked]="filterPoints()" (change)="filterPoints.set(!filterPoints())" /><span></span>
                  <span>{{ 'With points only' | translate }}</span>
                </label>
                <label class="c8y-checkbox checkbox-inline">
                  <input type="checkbox" [checked]="filterWritable()" (change)="filterWritable.set(!filterWritable())" /><span></span>
                  <span>{{ 'Writable' | translate }}</span>
                </label>
                <input class="form-control input-sm" style="max-width: 10em" [placeholder]="'Data type' | translate" [ngModel]="filterType()" (ngModelChange)="filterType.set($event)" />
              </div>
            </div>
            <cdk-virtual-scroll-viewport class="tdot-tree" itemSize="30">
              <div *cdkVirtualFor="let row of rows(); trackBy: trackRow" class="tdot-row p-l-8" [class.active]="row.id === focused()" (click)="focused.set(row.id)">
                <span [style.width.px]="row.depth * 16"></span>
                <span class="tdot-chevron" (click)="toggleExpand(row.id, row.expanded); $event.stopPropagation()">
                  @if (row.hasChildren) {
                    <i [c8yIcon]="row.expanded ? 'chevron-down' : 'chevron-right'"></i>
                  }
                </span>
                @if (pointCount(row.id); as n) {
                  <label class="c8y-checkbox m-0" (click)="$event.stopPropagation()">
                    <input type="checkbox" [checked]="state(row.id) === 'all'" [indeterminate]="state(row.id) === 'some'" (change)="toggle(row.id)" />
                    <span></span>
                  </label>
                } @else {
                  <span style="width: 20px; display: inline-block"></span>
                }
                <span class="m-l-4">{{ node(row.id)?.display_name }}</span>
                <span class="text-muted small m-l-8">{{ node(row.id)?.node_class }}@if (node(row.id)?.datatype) { · {{ node(row.id)?.datatype }} }</span>
                @if (node(row.id)?.truncated) {
                  <span class="tag tag--warning m-l-8" [title]="'Not every child was scanned: ' + node(row.id)?.truncated">{{ 'truncated' | translate }}</span>
                }
                @if (noPoints(row.id); as why) {
                  <i c8yIcon="ban" class="text-muted m-l-8" [title]="why"></i>
                }
              </div>
            </cdk-virtual-scroll-viewport>
            @if (focusedNode(); as n) {
              <div class="card-block small separator-top">
                <div><strong>{{ n.display_name }}</strong> <span class="text-muted">{{ n.id }}</span></div>
                <div class="text-muted">{{ ix.path.get(n.id) }}</div>
                @if (n.description) {
                  <div>{{ n.description }}</div>
                }
                <div>
                  {{ n.node_class }}
                  @if (n.datatype) { · {{ n.datatype }} }
                  @if (n.unit) { · {{ n.unit }} }
                  @if (n.access_level !== undefined) { · {{ 'access level' | translate }} {{ n.access_level }} }
                  @if (n.subscribable === false) { · {{ 'not subscribable' | translate }} }
                  @if (n.value !== undefined) { · {{ 'value' | translate }} {{ n.value }} }
                </div>
                @if (noPoints(n.id); as why) {
                  <div class="text-warning">{{ 'No point:' | translate }} {{ why }}</div>
                }
              </div>
            }
          </div>
        </div>

        <div class="col-md-6">
          <div class="card">
            <div class="card-header separator d-flex a-i-center">
              <div class="card-title flex-grow">{{ checked().length }} {{ (checked().length === 1 ? 'point checked' : 'points checked') | translate }}</div>
              <label class="c8y-checkbox checkbox-inline m-0">
                <input type="checkbox" [checked]="allMarked()" (change)="markAll()" /><span></span>
                <span>{{ 'Mark all' | translate }}</span>
              </label>
            </div>
            @if (marked().size) {
              <div class="card-block p-b-0">
                <p class="small m-b-4">
                  {{ 'Change the' | translate }} {{ marked().size }} {{ (marked().size === 1 ? 'marked point' : 'marked points') | translate }}.
                  {{ 'Fields left empty stay as they are.' | translate }}
                  @if (markedMethods(); as n) {
                    <br />{{ n }} {{ (n === 1 ? 'of them is a method: it takes no value settings and stays as it is.' : 'of them are methods: they take no value settings and stay as they are.') | translate }}
                  }
                </p>
                <div class="d-flex gap-8 m-b-8 flex-wrap a-i-end">
                  <div class="tdot-bulk-field">
                    <div class="tdot-bulk-label"><label for="tdot-bulk-mode">{{ 'Mode' | translate }}</label><tdot-help [text]="help.mode" [label]="'Mode' | translate" title="subscribe" /></div>
                    <select id="tdot-bulk-mode" class="form-control input-sm" [(ngModel)]="bulk.mode">
                      <option value="">{{ 'unchanged' | translate }}</option>
                      <option value="subscribe">{{ 'Subscribe' | translate }}</option>
                      <option value="poll">{{ 'Poll' | translate }}</option>
                    </select>
                  </div>
                  <div class="tdot-bulk-field">
                    <div class="tdot-bulk-label"><label for="tdot-bulk-interval">{{ 'Interval' | translate }}</label><tdot-help [text]="help.interval" [label]="'Interval' | translate" title="sampling_interval / poll_interval" /></div>
                    <input id="tdot-bulk-interval" class="form-control input-sm" [placeholder]="('e.g.' | translate) + ' 5s'" [(ngModel)]="bulk.interval" />
                  </div>
                  <div class="tdot-bulk-field">
                    <div class="tdot-bulk-label"><label for="tdot-bulk-access">{{ 'Access' | translate }}</label><tdot-help [text]="help.access" [label]="'Access' | translate" title="access" /></div>
                    <select id="tdot-bulk-access" class="form-control input-sm" [(ngModel)]="bulk.access">
                      <option value="">{{ 'unchanged' | translate }}</option>
                      <option value="read">read</option>
                      <option value="read_write">read_write</option>
                      <option value="write">write</option>
                    </select>
                  </div>
                  <div class="tdot-bulk-field">
                    <div class="tdot-bulk-label"><label for="tdot-bulk-type">{{ 'Measurement type' | translate }}</label><tdot-help [text]="help.measurementType" [label]="'Measurement type' | translate" title="meta.measurement.type" /></div>
                    <input id="tdot-bulk-type" class="form-control input-sm" [placeholder]="('e.g.' | translate) + ' Pump'" [(ngModel)]="bulk.measurementType" />
                  </div>
                  <div class="tdot-bulk-field">
                    <div class="tdot-bulk-label"><label for="tdot-bulk-fragment">{{ 'Fragment' | translate }}</label><tdot-help [text]="help.fragment" [label]="'Fragment' | translate" title="meta.measurement.fragment" /></div>
                    <input id="tdot-bulk-fragment" class="form-control input-sm" [placeholder]="('e.g.' | translate) + ' c8y_Pump'" [(ngModel)]="bulk.measurementFragment" />
                  </div>
                  <button class="btn btn-default btn-sm" type="button" (click)="applyBulk()">{{ 'Apply' | translate }}</button>
                </div>
              </div>
            }
            @if (checked().length) {
              <div class="tdot-point tdot-point-head p-l-8 p-r-8 small text-muted">
                <span class="tdot-col-mark" [title]="'Mark points to change them together' | translate"></span>
                <span class="tdot-col-id">{{ 'Point id' | translate }} <tdot-help [text]="help.id" [label]="'Point id' | translate" title="id" /></span>
                <span class="tdot-col-name">{{ 'Name' | translate }} <tdot-help [text]="help.name" [label]="'Name' | translate" title="name" /></span>
                <span class="tdot-col-unit">{{ 'Unit' | translate }} <tdot-help [text]="help.unit" [label]="'Unit' | translate" title="unit" /></span>
                <span class="tdot-col-info">{{ 'Type and settings' | translate }} <tdot-help [text]="help.summary" [label]="'Type and settings' | translate" /></span>
                <span class="tdot-col-x"></span>
              </div>
            } @else {
              <p class="text-muted p-16 m-0">{{ 'Check nodes in the address space to take their points.' | translate }}</p>
            }
            <cdk-virtual-scroll-viewport class="tdot-points" itemSize="40">
              <div *cdkVirtualFor="let p of checked(); trackBy: trackPoint" class="tdot-point p-l-8 p-r-8" [class.tdot-clash]="clashes().has(p.point['id'])">
                <label class="c8y-checkbox m-0 tdot-col-mark" [title]="'Mark for a change of several points' | translate">
                  <input type="checkbox" [checked]="marked().has(p.key)" (change)="mark(p.key)" [attr.aria-label]="('Mark' | translate) + ' ' + p.point['id']" /><span></span>
                </label>
                <input class="form-control tdot-id tdot-col-id" [attr.aria-label]="'Point id' | translate" [title]="clashes().has(p.point['id']) ? ('This id is used twice' | translate) : ''" [ngModel]="p.point['id']" (change)="edit(p.key, { id: $any($event.target).value })" />
                <input class="form-control tdot-col-name" [attr.aria-label]="'Name' | translate" [ngModel]="p.point['name']" (change)="edit(p.key, { name: $any($event.target).value })" />
                <input class="form-control tdot-col-unit" [attr.aria-label]="'Unit' | translate" placeholder="—" [disabled]="isMethod(p.point)" [title]="isMethod(p.point) ? ('A method has no unit' | translate) : ''" [ngModel]="p.point['unit']" (change)="edit(p.key, { unit: $any($event.target).value })" />
                <span class="small text-muted text-truncate tdot-col-info" [title]="summary(p.point)">{{ summary(p.point) }}</span>
                <button class="btn btn-link btn-xs tdot-col-x" type="button" [title]="'Uncheck' | translate" [attr.aria-label]="('Uncheck' | translate) + ' ' + p.point['id']" (click)="uncheck(p.key)"><i c8yIcon="times"></i></button>
              </div>
            </cdk-virtual-scroll-viewport>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-block d-flex a-i-center gap-16">
          <span class="flex-grow">
            @if (clashes().size) {
              <span class="text-danger">{{ 'Rename the points whose id is used twice:' | translate }} {{ [...clashes()].join(', ') }}</span>
            } @else if (problem(); as p) {
              <span class="text-warning">{{ p }}</span>
            } @else {
              {{ summaryLine() }}
            }
          </span>
          <button class="btn btn-default" type="button" (click)="cancel()">{{ (context() ? 'Back to the editor' : 'Cancel') | translate }}</button>
          <button class="btn btn-primary" type="button" [disabled]="!!clashes().size || !!problem()" (click)="create()">{{ 'Open in editor' | translate }}</button>
        </div>
      </div>
    } @else if (!loadError() && !noScan()) {
      <p><i c8yIcon="spinner" class="icon-spin"></i> {{ 'Loading the namespace file' | translate }}…</p>
    }
  `,
})
export class NamespacePickerComponent {
  private readonly route = inject(ActivatedRoute);
  private readonly router = inject(Router);
  private readonly inventory = inject(InventoryService);
  private readonly namespaces = inject(NamespaceService);
  private readonly ops = inject(ConnectorOperationsService);
  private readonly pending = inject(PendingFileService);
  private readonly alert = inject(AlertService);
  private readonly pickerContext = inject(PickerContextService);
  private readonly bsModal = inject(BsModalService);

  /** Opened from the editor (`?from=editor`): the file and device the points go to. */
  readonly context = signal<PickerContext | undefined>(undefined);
  /** In the editor's context: the namespace files that fit, best first. */
  readonly scans = signal<StoredNamespace[]>([]);
  /** In the editor's context: no namespace file fits yet. */
  readonly noScan = signal(false);
  /** The devices of the editor's connector file. */
  readonly contextDevices = signal<string[]>([]);

  readonly nodeClasses = NODE_CLASSES;
  readonly fileOfType = fileOfType;
  private namespaceId = '';

  readonly index = signal<NamespaceIndex | undefined>(undefined);
  readonly loadError = signal<string | undefined>(undefined);

  // --- tree --------------------------------------------------------------------------------
  readonly expanded = signal<ReadonlySet<string>>(new Set());
  readonly collapsed = signal<ReadonlySet<string>>(new Set());
  readonly focused = signal<string | undefined>(undefined);
  readonly filterText = signal('');
  readonly filterClass = signal<NodeClass | ''>('');
  readonly filterPoints = signal(false);
  readonly filterWritable = signal(false);
  readonly filterType = signal('');
  readonly filter = computed<TreeFilter>(() => ({
    text: this.filterText(),
    nodeClasses: this.filterClass() ? [this.filterClass() as NodeClass] : [],
    withPoints: this.filterPoints(),
    writable: this.filterWritable(),
    datatype: this.filterType().trim(),
  }));
  readonly visible = computed(() => {
    const index = this.index();
    return index ? visibleNodes(index, this.filter()) : undefined;
  });
  readonly rows = computed(() => {
    const index = this.index();
    return index ? treeRows(index, this.expanded(), this.visible(), this.collapsed()) : [];
  });
  readonly focusedNode = computed(() => {
    const id = this.focused();
    return id ? this.index()?.byId.get(id) : undefined;
  });

  // --- selection and target ----------------------------------------------------------------
  readonly selection = signal<Selection>(new Map());
  readonly marked = signal<ReadonlySet<string>>(new Set());
  readonly targetKind = signal<TargetKind>('new-library');
  readonly libraryName = signal('');
  readonly libraryType = signal('');
  readonly entries = signal<IManagedObject[]>([]);
  /** The scan's protocol: targets must be of the same protocol. */
  readonly protocol = computed(() => this.index()?.file.protocol ?? 'opcua');
  readonly protocolLabel = computed(() => protocolInfo(this.protocol()).label);
  readonly libraryEntries = computed(() => entriesOfKind(this.entries(), 'library').filter(e => fileOfType(e['configurationType'])?.protocol === this.protocol()));
  readonly connectorCandidates = computed(() => entriesOfKind(this.entries(), 'connector'));
  readonly connectorEntries = signal<IManagedObject[]>([]);
  readonly targetEntryId = signal('');
  private targetText = '';
  readonly connector = signal<ConnectorChoice | undefined>(undefined);
  readonly targetDevice = signal(0);
  readonly target = signal<TargetPoints | undefined>(undefined);
  readonly match = computed<TargetMatch | undefined>(() => {
    const index = this.index();
    const target = this.target();
    return index && target ? matchTarget(index, target) : undefined;
  });
  readonly outsideCount = computed(() => this.match()?.outside.length ?? 0);

  readonly checked = computed(() => {
    const index = this.index();
    return index ? checkedPoints(index, this.selection(), this.target(), this.match()) : [];
  });
  readonly clashes = computed(() => {
    const index = this.index();
    const target = this.target() ?? { points: [] };
    if (!index) return new Set<string>();
    const match = this.match() ?? matchTarget(index, target);
    return clashingIds(resultPoints(index, this.selection(), target, match).points);
  });
  /** How many of the marked points are methods, which a change of several points leaves alone. */
  readonly markedMethods = computed(() => this.checked().filter(p => this.marked().has(p.key) && isMethodPoint(p.point)).length);
  readonly isMethod = isMethodPoint;
  readonly allMarked = computed(() => this.checked().length > 0 && this.checked().every(p => this.marked().has(p.key)));
  readonly libraryNameProblem = computed(() => {
    const name = normalizeName(this.libraryName());
    if (!name) return undefined;
    if (!isValidName(name)) return 'Letters, digits, ".", "_" and "-" only, starting with a letter or digit.';
    const taken = [
      ...this.libraryEntries().map(e => fileOfType(e['configurationType'])?.name),
      ...packagedLibraries().filter(l => l.protocol === this.protocol()).map(l => l.name),
    ];
    return taken.includes(name) ? 'A library of that name exists; choose "Existing point library" to add to it.' : undefined;
  });
  readonly problem = computed<string | undefined>(() => {
    switch (this.targetKind()) {
      case 'new-library':
        if (!this.checked().length) return 'Check the nodes whose points the library should have.';
        if (!normalizeName(this.libraryName())) return 'Name the new library.';
        return this.libraryNameProblem();
      case 'library':
        return this.target() ? undefined : 'Choose the library to change.';
      case 'device':
        return this.target() ? undefined : 'Choose the connector file and the device.';
      case 'editor':
        return this.target() ? undefined : 'The file in the editor has no device to add points to.';
    }
  });
  readonly summaryLine = computed(() => {
    const index = this.index();
    if (!index) return '';
    const target = this.target() ?? { points: [] };
    const r = resultPoints(index, this.selection(), target, this.match() ?? matchTarget(index, target));
    return `${plural(r.added, 'new point')}, ${r.points.length - r.added - r.outside} kept, ${r.removed} removed.`;
  });

  /** The change of several points; empty fields stay as they are. */
  bulk: {
    mode: '' | 'subscribe' | 'poll';
    interval?: string;
    access: '' | 'read' | 'write' | 'read_write';
    measurementType?: string;
    measurementFragment?: string;
  } = { mode: '', access: '' };

  /** Help for the point list and the change of several points, from the form's texts where they exist. */
  readonly help = {
    id: 'The point id: the series name in Cumulocity and the MQTT topic segment. It must be unique in the file.',
    name: 'A readable name for the point. The series and the MQTT topic use the id.',
    unit: HELP['point.unit'],
    summary: 'The data type and the settings that differ from the defaults: as opcua-scan proposed them, or as you changed them.',
    mode: 'Subscribe: the server pushes the value when it changes. Poll: the connector reads it every interval (subscribe = false).',
    interval: `With Subscribe, the sampling interval: ${HELP['point.sampling_interval']} With Poll, the poll interval: ${HELP['point.poll_interval']} A duration such as 500ms, 5s or 1m.`,
    access: HELP['point.access'],
    measurementType: HELP['measurement.type'],
    fragment: HELP['measurement.fragment'],
  };

  constructor() {
    this.route.paramMap.pipe(takeUntilDestroyed()).subscribe(params => void this.load(params.get('id') ?? ''));
  }

  private async load(id: string): Promise<void> {
    this.namespaceId = id;
    this.index.set(undefined);
    this.loadError.set(undefined);
    this.noScan.set(false);
    // The editor's context counts only when the editor sent us here; a stale one never hijacks
    // a picker opened from the Namespaces list.
    const context = this.route.snapshot.queryParamMap.get('from') === 'editor' ? this.pickerContext.get() : undefined;
    this.context.set(context);
    try {
      if (context) {
        const stored = (await this.namespaces.list()).map(e => ({ id: String(e.mo.id), meta: e.meta, lastUpdated: e.mo['lastUpdated'] }));
        const fitting = scansFor(stored, context);
        this.scans.set(fitting);
        if (!id) {
          if (fitting.length) {
            this.openScan(fitting[0].id);
          } else {
            this.noScan.set(true);
          }
          return;
        }
      }
      if (!id) {
        throw new Error('Choose a namespace file in the Namespaces list.');
      }
      const [file, entries, mo] = await Promise.all([
        this.namespaces.read(id),
        this.inventory.list({ query: managedEntriesQuery(), pageSize: 2000 }).then(r => r.data),
        this.inventory.detail(id).then(r => r.data).catch(() => undefined),
      ]);
      this.metadata = mo?.[METADATA_FRAGMENT] as NamespaceMetadata | undefined;
      const index = indexNamespace(file);
      this.index.set(index);
      this.entries.set(entries);
      this.expanded.set(new Set(index.roots));
      this.libraryType.set('');
      this.libraryName.set(file.scan.device ? normalizeName(file.scan.device).replace(/[^A-Za-z0-9._-]/g, '-') : '');
      if (context) {
        this.useContext(context);
      } else {
        void this.loadConnectorEntries().then(() => this.preselectTarget());
      }
    } catch (e) {
      this.loadError.set(e instanceof Error ? e.message : String(e));
    }
  }

  private metadata?: NamespaceMetadata;

  /** Targets the editor's text: its device for a connector file, else the library. */
  private useContext(context: PickerContext): void {
    this.targetKind.set('editor');
    try {
      const config = parseToml(context.text);
      if (context.kind === 'library') {
        this.setTarget(libraryTarget(config), context.text);
        return;
      }
      const devices = (Array.isArray(config['device']) ? config['device'] : []).map((d: Config, i: number) => String(d['name'] ?? `device ${i + 1}`));
      this.contextDevices.set(devices);
      this.chooseContextDevice(Math.max(0, devices.indexOf(context.device ?? '')));
    } catch (e) {
      this.loadError.set(`The file in the editor cannot be read: ${e instanceof Error ? e.message : String(e)}`);
    }
  }

  chooseContextDevice(d: number): void {
    const context = this.context();
    if (!context) return;
    this.targetDevice.set(d);
    const device = parseToml(context.text)['device']?.[d];
    this.setTarget(device ? deviceTarget(device) : undefined, context.text);
  }

  /**
   * A scan whose metadata names a connector file and device that the repository has starts with
   * that device as the target. Only while the user has not chosen anything yet.
   */
  private async preselectTarget(): Promise<void> {
    const meta = { connectorFile: this.metadata?.connectorFile ?? this.index()?.file.scan.connector_file, device: this.metadata?.device ?? this.index()?.file.scan.device };
    const candidates = this.connectorEntries().map(e => ({
      id: String(e.id),
      fileName: fileOfType(e['configurationType'])?.name ?? '',
      devices: this.connectorDevices.get(String(e.id)) ?? [],
    }));
    const found = targetForScan(meta, candidates);
    if (!found || this.targetKind() !== 'new-library' || this.selection().size) return;
    this.targetKind.set('device');
    await this.chooseConnector(found.entryId);
  }

  /** In the editor's context: shows another namespace file, keeping the context. */
  openScan(id: string): void {
    void this.router.navigate(['/ot-connectors', 'namespace', id], { queryParams: { from: 'editor' } });
  }

  scanLabel(s: StoredNamespace): string {
    const m = s.meta;
    return [m?.device, m?.connectorFile ? `${m.connectorFile}.toml` : undefined, m?.gateway, m?.scannedAt].filter(Boolean).join(' · ') || s.id;
  }

  scan(): void {
    this.bsModal.show(ScanDialogComponent, {
      initialState: { preferredDevice: this.context()?.device, onScanned: (id: string) => this.openScan(id), onOpen: (id: string) => this.openScan(id) },
      class: 'modal-lg',
      ignoreBackdropClick: true,
    });
  }

  upload(): void {
    this.bsModal.show(UploadNamespaceComponent, {
      initialState: { onStored: (id: string) => this.openScan(id) },
      ignoreBackdropClick: true,
    });
  }

  /** Device names of the OPC UA connector entries, by entry id. */
  private readonly connectorDevices = new Map<string, string[]>();

  /** The repository's connector files of the scan's protocol. */
  private async loadConnectorEntries(): Promise<void> {
    const opcua: IManagedObject[] = [];
    for (const entry of this.connectorCandidates()) {
      try {
        const config = parseToml(await this.ops.entryText(entry));
        if (config['connector']?.['protocol'] !== this.protocol()) continue;
        opcua.push(entry);
        this.connectorDevices.set(
          String(entry.id),
          (Array.isArray(config['device']) ? config['device'] : []).map((d: Config) => String(d['name'] ?? ''))
        );
      } catch {
        // Not readable: not offered.
      }
    }
    this.connectorEntries.set(opcua);
  }

  node(id: string) {
    return this.index()?.byId.get(id);
  }

  pointCount(id: string): number {
    const index = this.index();
    return index ? index.byId.get(id)?.points?.length || (index.children.get(id)?.length ? 1 : 0) : 0;
  }

  noPoints(id: string): string | undefined {
    const node = this.node(id);
    return node ? whyNoPoints(node) : undefined;
  }

  state(id: string) {
    return checkState(this.index()!, this.selection(), id);
  }

  trackRow = (_: number, row: { id: string }) => row.id;
  trackPoint = (_: number, p: { key: string }) => p.key;

  toggleExpand(id: string, open: boolean): void {
    if (this.visible()) {
      this.collapsed.update(s => toggled(s, id, open));
    } else {
      this.expanded.update(s => toggled(s, id, !open));
    }
  }

  toggle(id: string): void {
    this.selection.set(toggleNode(this.index()!, this.selection(), id));
  }

  uncheck(key: string): void {
    const next = new Map(this.selection());
    next.delete(key);
    this.selection.set(next);
    this.marked.update(s => toggled(s, key, false));
  }

  edit(key: string, change: PointChange): void {
    this.selection.set(changePoints(this.selection(), [key], change));
  }

  mark(key: string): void {
    this.marked.update(s => toggled(s, key, !s.has(key)));
  }

  markAll(): void {
    this.marked.set(this.allMarked() ? new Set() : new Set(this.checked().map(p => p.key)));
  }

  applyBulk(): void {
    const change: PointChange = {};
    if (this.bulk.mode) change.mode = this.bulk.mode;
    if (this.bulk.interval?.trim()) change.interval = this.bulk.interval.trim();
    if (this.bulk.access) change.access = this.bulk.access;
    if (this.bulk.measurementType?.trim()) change.measurementType = this.bulk.measurementType.trim();
    if (this.bulk.measurementFragment?.trim()) change.measurementFragment = this.bulk.measurementFragment.trim();
    this.selection.set(changePoints(this.selection(), this.marked(), change));
  }

  summary(point: Config): string {
    // A method point has no datatype; its access says what it is.
    const parts = [point['datatype'] ?? point['access']];
    if (point['datatype'] && point['access'] && point['access'] !== 'read') parts.push(point['access']);
    if (point['subscribe'] === false) parts.push(`poll ${point['poll_interval'] ?? ''}`.trim());
    else if (point['sampling_interval']) parts.push(`subscribe ${point['sampling_interval']}`);
    const m = point['meta']?.['measurement'];
    if (m?.type || m?.fragment) parts.push([m.type, m.fragment].filter(Boolean).join('/'));
    if (point['address']?.['field']) parts.push(`field ${point['address']['field']}`);
    return parts.filter(Boolean).join(' · ');
  }

  // --- target ------------------------------------------------------------------------------
  setTargetKind(kind: TargetKind): void {
    this.targetKind.set(kind);
    this.targetEntryId.set('');
    this.connector.set(undefined);
    this.setTarget(undefined, '');
  }

  private setTarget(target: TargetPoints | undefined, text: string): void {
    this.targetText = text;
    this.target.set(target);
    const index = this.index();
    // The target's points that the file has start checked; other checks stay.
    const keep = new Map(this.selection());
    if (index && target) {
      for (const [key, change] of initialSelection(matchTarget(index, target))) {
        if (!keep.has(key)) keep.set(key, change);
      }
    }
    this.selection.set(keep);
  }

  async chooseLibrary(id: string): Promise<void> {
    this.targetEntryId.set(id);
    const entry = this.libraryEntries().find(e => e.id === id);
    if (!entry) {
      this.setTarget(undefined, '');
      return;
    }
    try {
      const text = await this.ops.entryText(entry);
      this.setTarget(libraryTarget(parseToml(text)), text);
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  async chooseConnector(id: string): Promise<void> {
    this.targetEntryId.set(id);
    const entry = this.connectorEntries().find(e => e.id === id);
    if (!entry) {
      this.connector.set(undefined);
      this.setTarget(undefined, '');
      return;
    }
    try {
      const text = await this.ops.entryText(entry);
      const config = parseToml(text);
      const devices = (Array.isArray(config['device']) ? config['device'] : []).map((d: Config, i: number) => String(d['name'] ?? `device ${i + 1}`));
      this.connector.set({ entry, text, devices });
      const scanned = this.index()?.file.scan.device;
      this.chooseDevice(Math.max(0, devices.indexOf(scanned ?? '')));
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  chooseDevice(d: number): void {
    const connector = this.connector();
    if (!connector) return;
    this.targetDevice.set(d);
    const device = parseToml(connector.text)['device']?.[d];
    this.setTarget(device ? deviceTarget(device) : undefined, connector.text);
  }

  // --- result ------------------------------------------------------------------------------
  create(): void {
    const index = this.index();
    if (!index || this.problem() || this.clashes().size) return;
    const target = this.target() ?? { points: [] };
    const result = resultPoints(index, this.selection(), target, this.match() ?? matchTarget(index, target));
    const from = `${index.file.scan.device ?? index.file.server.endpoint} (scan of ${index.file.scan.time})`;
    const counts = `${plural(result.added, 'point')} added, ${result.removed} removed`;
    try {
      switch (this.targetKind()) {
        case 'new-library': {
          const fileName = normalizeName(this.libraryName());
          this.pending.hand({
            kind: 'library',
            fileName,
            name: fileName,
            text: newLibraryText(result.points, result.namespaces, { type: this.libraryType().trim() || undefined, protocol: this.protocol() }),
            description: `From the namespace of ${from}`,
            note: `A new library with ${plural(result.points.length, 'point')} from the namespace of ${from}. Not saved yet.`,
          });
          void this.router.navigate(['/ot-connectors', 'new-library']);
          return;
        }
        case 'library': {
          const written = writeLibrary(this.targetText, result);
          this.handChanges('library', written, `${counts} from the namespace of ${from}.`);
          return;
        }
        case 'device': {
          const written = writeDevice(this.targetText, this.targetDevice(), result);
          const device = this.connector()?.devices[this.targetDevice()];
          this.handChanges('connector', written, `${counts} on device ${device}, from the namespace of ${from}.`);
          return;
        }
        case 'editor': {
          const context = this.context()!;
          if (context.kind === 'library') {
            const written = writeLibrary(this.targetText, result);
            this.handBack(written.text, `${counts} from the namespace of ${from}.`, written.keptLayout);
          } else {
            const written = writeDevice(this.targetText, this.targetDevice(), result);
            const device = this.contextDevices()[this.targetDevice()];
            this.handBack(written.text, `${counts} on device ${device}, from the namespace of ${from}.`, written.keptLayout);
          }
          return;
        }
      }
    } catch (e) {
      this.alert.danger(e instanceof Error ? e.message : String(e));
    }
  }

  private handChanges(kind: 'library' | 'connector', written: { text: string; keptLayout: boolean }, what: string): void {
    const entryId = this.targetEntryId();
    const file = fileOfType(this.entries().find(e => e.id === entryId)?.['configurationType']);
    this.pending.hand({
      kind,
      entryId,
      fileName: file?.name ?? '',
      text: written.text,
      note: `${what} Not saved yet.${written.keptLayout ? '' : ' The file could not keep its layout and comments, so it was written anew; check it before saving.'}`,
    });
    void this.router.navigate(['/ot-connectors', entryId]);
  }

  /**
   * Back to the editor with `text`: the saved entry it was editing, or the new file with its
   * fields. Without a note it is the editor's own text, unsaved edits included.
   */
  private handBack(text: string, what?: string, keptLayout = true): void {
    const context = this.context();
    if (!context) return;
    this.pending.hand({
      kind: context.kind,
      entryId: context.entryId,
      fileName: context.fileName,
      name: context.name,
      description: context.description,
      deviceType: context.deviceType,
      text,
      note: what
        ? `${what} Not saved yet.${keptLayout ? '' : ' The file could not keep its layout and comments, so it was written anew; check it before saving.'}`
        : undefined,
    });
    this.pickerContext.clear();
    void this.router.navigate(['/ot-connectors', context.entryId ?? (context.kind === 'library' ? 'new-library' : 'new')]);
  }

  cancel(): void {
    if (this.context()) {
      this.handBack(this.context()!.text);
      return;
    }
    void this.router.navigate(['/ot-connectors']);
  }

  /** For the template: the id of the file being edited. */
  get id(): string {
    return this.namespaceId;
  }
}

function plural(n: number, word: string): string {
  return `${n} ${word}${n === 1 ? '' : 's'}`;
}

function toggled(set: ReadonlySet<string>, key: string, on: boolean): Set<string> {
  const next = new Set(set);
  if (on) next.add(key);
  else next.delete(key);
  return next;
}
