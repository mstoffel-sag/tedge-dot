import { Component, computed, effect, inject, signal, viewChild } from '@angular/core';
import { takeUntilDestroyed } from '@angular/core/rxjs-interop';
import { NgModel } from '@angular/forms';
import { ActivatedRoute, Router } from '@angular/router';
import { IManagedObject, InventoryService } from '@c8y/client';
import { AlertService, CoreModule, ModalService, Status } from '@c8y/ngx-components';
import { gettext } from '@c8y/ngx-components/gettext';
import { EditorComponent, loadMonacoEditor } from '@c8y/ngx-components/editor';
import { RepositoryService, RepositoryType, SharedRepositoryModule } from '@c8y/ngx-components/repository/shared';
import type * as Monaco from 'monaco-editor';

import { PROTOCOL_IDS, emitToml, newConfig, type Config, type ProtocolId } from '../generated/config-builder';
import { ConnectorOperationsService } from '../shared/connector-operations.service';
import { commentLines, newLibrary } from '../shared/form-model';
import { type LibraryCatalog, checkReferences } from '../shared/libraries';
import {
  type FileKind,
  fileOfType,
  isValidConnectorName,
  isValidName,
  isValidProtocol,
  libraryPathOf,
  libraryTypeOf,
  normalizeName,
  pathOf,
  typeOf,
} from '../shared/naming';
import { PendingFileService } from '../shared/pending-file.service';
import { PickerContextService } from '../shared/picker-context.service';
import { emitLibraryToml, writeKeepingLayout } from '../shared/toml-write';
import { TOML_LANGUAGE_ID, tomlLanguage } from '../shared/toml-language';
import { type ValidationResult, describePointer, isValid, validateConnectorFile, validateLibraryFile } from '../shared/validation';
import { ConnectorFormComponent } from './connector-form.component';
import { LibraryFormComponent } from './library-form.component';
import { ValidIfDirective } from './valid-if.directive';

const MARKER_OWNER = 'tedge-dot';

/**
 * Adds or edits one connector file or point library in the configuration repository. A connector
 * file may only reference point libraries that exist; a library's protocol and name are fixed
 * once it is saved, because its type and its path on gateways name them.
 */
@Component({
  selector: 'tdot-connector-editor',
  imports: [CoreModule, SharedRepositoryModule, EditorComponent, ConnectorFormComponent, LibraryFormComponent, ValidIfDirective],
  providers: [ConnectorOperationsService],
  template: `
    <!-- c8y-title sets the browser tab title only when it is created: a new one per heading
         keeps the tab title right when the router reuses this editor for another file. -->
    @for (h of [heading()]; track h) {
      <c8y-title>{{ h | translate }}</c8y-title>
    }

    <div class="card card--fullpage">
      <div class="card-header separator">
        <div class="card-title">
          {{ isNew() ? ((isLibrary() ? 'New point library' : 'New connector file') | translate) : entry()?.['name'] }}
        </div>
      </div>
      <div class="inner-scroll">
        <div class="card-block">
          @if (copyNote(); as note) {
            <div class="alert alert-info" role="status">{{ note }}</div>
          }
          @if (handedNote(); as note) {
            <div class="alert alert-info" role="status">{{ note }}</div>
          }
          <div class="row">
            <div class="col-md-3">
              <c8y-form-group>
                <label for="tdot-name">{{ 'Name' | translate }}</label>
                <input
                  id="tdot-name"
                  name="name"
                  class="form-control"
                  required
                  [placeholder]="('e.g.' | translate) + ' Line 3 pumps'"
                  #nameModel="ngModel"
                  [(ngModel)]="name"
                  [tdotValidIf]="nameCheck"
                  (blur)="nameTouched.set(true)"
                />
                <!-- The messages are our own: c8y-messages fills in its own ones after a delay that,
                     without zone.js, is not drawn until something else changes. -->
                <c8y-messages [defaults]="noDefaultMessages">
                  @if (nameError()) {
                    <c8y-message>{{ nameRule | translate }}</c8y-message>
                  }
                </c8y-messages>
              </c8y-form-group>
            </div>
            <div class="col-md-3">
              <c8y-form-group>
                <label for="tdot-file">{{ (isLibrary() ? 'Library name' : 'File name') | translate }}</label>
                <div class="input-group">
                  <input
                    id="tdot-file"
                    name="fileName"
                    class="form-control"
                    required
                    aria-describedby="tdot-file-msg"
                    [placeholder]="('e.g.' | translate) + (isLibrary() ? ' acme-meter' : ' opcua-pump')"
                    #fileModel="ngModel"
                    [(ngModel)]="fileName"
                    [tdotValidIf]="fileNameCheck()"
                    [disabled]="!isNew()"
                    (blur)="fileTouched.set(true)"
                  />
                  <span class="input-group-addon">.toml</span>
                </div>
                <c8y-messages id="tdot-file-msg" [defaults]="noDefaultMessages" [helpMessage]="fileError() ? '' : gatewayPath()">
                  @if (fileError()) {
                    <c8y-message>{{ fileNameRule() | translate }}</c8y-message>
                  }
                </c8y-messages>
              </c8y-form-group>
            </div>
            <div class="col-md-3">
              <c8y-form-group>
                <label for="tdot-device-type">{{ 'Device type' | translate }}</label>
                <input id="tdot-device-type" name="deviceType" class="form-control" [(ngModel)]="deviceType" [placeholder]="('e.g.' | translate) + ' thin-edge.io'" />
                <c8y-messages [helpMessage]="'Empty: offered to every device type' | translate"></c8y-messages>
              </c8y-form-group>
            </div>
            @if (isNew()) {
              <div class="col-md-3">
                <c8y-form-group>
                  <label for="tdot-start">{{ 'Start from' | translate }}</label>
                  <div class="c8y-select-wrapper">
                    <select id="tdot-start" name="startFrom" class="form-control" [(ngModel)]="startFrom" (ngModelChange)="useTemplate($event)">
                      @for (p of protocols; track p) {
                        <option [ngValue]="p">{{ p }}</option>
                      }
                    </select>
                  </div>
                  <c8y-messages [helpMessage]="'Replaces the content with a template' | translate"></c8y-messages>
                </c8y-form-group>
              </div>
            }
            <div class="col-md-12">
              <c8y-form-group>
                <label for="tdot-description">{{ 'Description' | translate }}</label>
                <input id="tdot-description" name="description" class="form-control" [(ngModel)]="description" />
                <c8y-messages></c8y-messages>
              </c8y-form-group>
            </div>
          </div>

          <div class="btn-group btn-group-sm m-b-16" role="group" [attr.aria-label]="'View' | translate">
            <button type="button" class="btn btn-default" [class.active]="view() === 'form'" [attr.aria-pressed]="view() === 'form'" (click)="showForm()">
              {{ 'Form' | translate }}
            </button>
            <button type="button" class="btn btn-default" [class.active]="view() === 'toml'" [attr.aria-pressed]="view() === 'toml'" (click)="showToml()">
              TOML
            </button>
          </div>

          @if (view() === 'form') {
            @if (result(); as r) {
              @if (r.syntax) {
                <div class="alert alert-danger" role="alert">
                  {{ 'The form cannot show a file with a syntax error. Fix it in the TOML view.' | translate }}
                  ({{ 'line' | translate }} {{ r.syntax.line }})
                  <button type="button" class="btn btn-default btn-sm m-l-8" (click)="showToml()">{{ 'Open the TOML view' | translate }}</button>
                </div>
              } @else if (isLibrary()) {
                @if (r.config?.['library'] === undefined) {
                  <div class="alert alert-info" role="status">
                    {{ 'This file has no [library] table, so the form cannot show it. Edit it in the TOML view.' | translate }}
                  </div>
                } @else {
                  <tdot-library-form [config]="r.config!" [errors]="r.fields" [protocolFixed]="!isNew()" (configChange)="onFormChange($event)" (pointsFromNamespace)="fromNamespace()" />
                }
              } @else if (!r.config?.['connector']) {
                <div class="alert alert-info" role="status">
                  {{ 'This file has no [connector] table, so the form cannot show it. Edit it in the TOML view.' | translate }}
                </div>
              } @else {
                <tdot-connector-form [config]="r.config!" [errors]="r.fields" [catalog]="catalog()" (configChange)="onFormChange($event)" (pointsFromNamespace)="fromNamespace($event)" />
              }
            }
          } @else {
            <c8y-form-group class="tdot-editor">
              <label class="sr-only" for="tdot-toml">{{ 'TOML' | translate }}</label>
              <c8y-editor
                id="tdot-toml"
                name="content"
                [(ngModel)]="content"
                [editorOptions]="editorOptions"
                (editorInit)="onEditorInit($event)"
              ></c8y-editor>
            </c8y-form-group>
          }

          <div class="p-t-8" role="status" aria-live="polite">
            @if (result(); as r) {
              @if (r.syntax) {
                <p class="text-danger">
                  <i c8yIcon="exclamation-circle"></i>
                  {{ 'Line' | translate }} {{ r.syntax.line }}, {{ 'column' | translate }} {{ r.syntax.column }}:
                  {{ r.syntax.message }}
                </p>
              } @else if (r.fields.length) {
                <ul class="list-unstyled text-danger m-b-0">
                  @for (f of r.fields; track f.path + f.message) {
                    <li><i c8yIcon="exclamation-circle"></i> {{ f.label }}: {{ f.message }}</li>
                  }
                </ul>
              } @else {
                <p class="text-success m-b-0">
                  <i c8yIcon="check-circle"></i>
                  {{ (isLibrary()
                    ? 'Matches the point-library schema.'
                    : 'Matches the contract schema, and every point library it references exists.') | translate }}
                </p>
              }
            }
          </div>
        </div>
      </div>
      <div class="card-footer separator d-flex a-i-center">
        @if (!canSave() && !saving()) {
          <span class="text-muted small">
            {{ (isLibrary()
              ? 'To save, give the entry a name and a valid library name, and fix the problems listed above.'
              : 'To save, give the entry a name and a valid file name, and fix the problems listed above.') | translate }}
          </span>
        }
        <div class="m-l-auto">
          <button class="btn btn-default" type="button" [title]="'Cancel' | translate" (click)="cancel()">{{ 'Cancel' | translate }}</button>
          <button
            class="btn btn-primary"
            type="button"
            [class.btn-pending]="saving()"
            [attr.aria-busy]="saving()"
            [title]="'Save' | translate"
            [disabled]="!canSave() || saving()"
            (click)="save()"
          >
            {{ 'Save' | translate }}
          </button>
        </div>
      </div>
    </div>
  `,
  styles: [
    `
      .tdot-editor c8y-editor {
        display: block;
        height: max(360px, 55vh);
        border: 1px solid var(--c8y-form-control-border-color-default, #ccc);
      }
    `,
  ],
})
export class ConnectorEditorComponent {
  private readonly route = inject(ActivatedRoute);
  private readonly router = inject(Router);
  private readonly inventory = inject(InventoryService);
  private readonly repository = inject(RepositoryService);
  private readonly alert = inject(AlertService);
  private readonly modal = inject(ModalService);
  private readonly pending = inject(PendingFileService);
  private readonly pickerContext = inject(PickerContextService);
  private readonly ops = inject(ConnectorOperationsService);

  readonly protocols = PROTOCOL_IDS;
  readonly editorOptions: Monaco.editor.IStandaloneEditorConstructionOptions = {
    language: TOML_LANGUAGE_ID,
    minimap: { enabled: false },
    automaticLayout: true,
    scrollBeyondLastLine: false,
    tabSize: 2,
  };

  readonly kind = signal<FileKind>('connector');
  readonly isLibrary = computed(() => this.kind() === 'library');
  /** The point libraries a connector file may reference; undefined until they are loaded. */
  readonly catalog = signal<LibraryCatalog | undefined>(undefined);
  /** For a saved library: the protocol its type names, which the file must keep. */
  readonly savedProtocol = signal<string | undefined>(undefined);
  readonly isNew = signal(true);
  readonly entry = signal<IManagedObject | undefined>(undefined);
  readonly name = signal('');
  readonly description = signal('');
  readonly fileName = signal('');
  readonly deviceType = signal('');
  readonly startFrom = signal<ProtocolId>('opcua');
  readonly content = signal('');
  readonly nameTouched = signal(false);
  /** Also set for an uploaded file, whose name is checked before the user touches the field. */
  readonly fileTouched = signal(false);
  readonly saving = signal(false);
  /** For a copy: the name of the entry it was made from. */
  readonly copyOf = signal<string | undefined>(undefined);
  /** What a copy is, until it is saved. */
  readonly copyNote = computed(() => {
    const original = this.copyOf();
    if (!original) {
      return undefined;
    }
    const service = this.isLibrary() ? undefined : this.result().config?.['connector']?.service_name;
    return (
      `A copy of ${original}, not saved yet.` +
      (service ? ` Its service name is ${service}, so it can run next to the original on the same gateway.` : '')
    );
  });
  /** Where handed content came from, e.g. points from a namespace file; until it is saved. */
  readonly handedNote = signal<string | undefined>(undefined);
  readonly view = signal<'form' | 'toml'>('form');

  readonly heading = computed(() =>
    this.isNew()
      ? this.isLibrary()
        ? 'Add point library'
        : 'Add connector file'
      : this.isLibrary()
        ? 'Edit point library'
        : 'Edit connector file'
  );
  readonly result = computed<ValidationResult>(() => {
    const text = this.content() ?? '';
    if (this.isLibrary()) {
      const result = validateLibraryFile(text);
      return result.config ? { ...result, fields: [...result.fields, ...this.protocolProblems(result.config)] } : result;
    }
    const result = validateConnectorFile(text);
    const catalog = this.catalog();
    return result.config && catalog ? { ...result, fields: [...result.fields, ...checkReferences(result.config, catalog)] } : result;
  });
  /** A library's protocol: the one its type names once saved, else the file's. */
  readonly libraryProtocol = computed<string>(() => this.savedProtocol() ?? this.result().config?.['library']?.protocol ?? '');
  /**
   * The checks of the name and file name fields. ValidIfDirective makes a field that fails its
   * check invalid, so c8y-form-group marks it red once it is touched; the messages are our own.
   */
  readonly nameCheck = (value: string) => !!value.trim();
  readonly nameRule = gettext('Enter a name for the repository entry.');
  readonly nameError = computed(() => this.nameTouched() && !this.nameCheck(this.name()));
  /** A library name, or a connector name that does not start with "points.". */
  readonly fileNameCheck = computed(() =>
    this.isLibrary()
      ? (value: string) => isValidName(normalizeName(value))
      : (value: string) => isValidConnectorName(normalizeName(value))
  );
  readonly fileError = computed(() => this.fileTouched() && !this.fileValid());
  /** No built-in texts, such as "This field is required.": the fields above show their own. */
  readonly noDefaultMessages = {};
  readonly fileNameRule = computed(() =>
    this.isLibrary()
      ? gettext('Letters, digits, ".", "_" and "-" only, starting with a letter or digit.')
      : gettext('Letters, digits, ".", "_" and "-" only, starting with a letter or digit. It may not start with "points.", which point libraries use.')
  );
  readonly fileValid = computed(() => this.fileNameCheck()(this.fileName()));
  private readonly nameModel = viewChild('nameModel', { read: NgModel });
  private readonly fileModel = viewChild('fileModel', { read: NgModel });
  /** Where the file lands on a gateway, shown under the file name. */
  readonly gatewayPath = computed(() => {
    const name = this.fileValid() ? normalizeName(this.fileName()) : '<name>';
    if (!this.isLibrary()) {
      return pathOf(name);
    }
    const protocol = this.libraryProtocol();
    return libraryPathOf(isValidProtocol(protocol) ? protocol : '<protocol>', name);
  });
  readonly canSave = computed(
    () =>
      !!this.name().trim() &&
      this.fileValid() &&
      isValid(this.result()) &&
      (!this.isLibrary() || isValidProtocol(this.libraryProtocol()))
  );

  private monaco?: typeof Monaco;
  private editor?: Monaco.editor.IStandaloneCodeEditor;

  constructor() {
    effect(() => this.applyMarkers(this.result()));
    // An uploaded file's name is checked at once: c8y-form-group marks only a touched field.
    effect(() => {
      if (this.fileTouched()) {
        this.fileModel()?.control.markAsTouched();
      }
    });
    void this.loadCatalog();
    // The router keeps this component when only the id changes, e.g. from an entry to "new".
    this.route.paramMap.pipe(takeUntilDestroyed()).subscribe(params => void this.load(params.get('id')));
  }

  private async load(id: string | null): Promise<void> {
    // Another file in the same editor: its fields start untouched, without red marks.
    this.nameModel()?.control.markAsUntouched();
    this.fileModel()?.control.markAsUntouched();
    this.nameTouched.set(false);
    this.fileTouched.set(false);
    if (!id || id === 'new' || id === 'new-library') {
      this.showForm();
      this.isNew.set(true);
      this.kind.set(id === 'new-library' ? 'library' : 'connector');
      this.savedProtocol.set(undefined);
      this.entry.set(undefined);
      this.name.set('');
      this.description.set('');
      this.deviceType.set('');
      this.fileName.set('');
      // An uploaded file that needs fixing, or a copy of an entry, comes here from the page.
      const uploaded = this.pending.take();
      this.copyOf.set(uploaded?.copyOf);
      this.handedNote.set(uploaded?.note);
      if (uploaded) {
        this.kind.set(uploaded.kind);
        this.name.set(uploaded.name ?? uploaded.fileName);
        this.description.set(uploaded.description ?? '');
        this.deviceType.set(uploaded.deviceType ?? '');
        this.fileName.set(uploaded.fileName);
        this.fileTouched.set(true);
        this.content.set(uploaded.text);
        if (!this.result().config) {
          this.showToml();
        }
      } else {
        this.useTemplate(this.startFrom());
      }
      return;
    }
    this.isNew.set(false);
    this.copyOf.set(undefined);
    this.handedNote.set(undefined);
    try {
      const { data } = await this.inventory.detail(id);
      if (this.route.snapshot.paramMap.get('id') !== id) {
        return;
      }
      const file = fileOfType(data['configurationType']);
      this.kind.set(file?.kind ?? 'connector');
      this.savedProtocol.set(file?.protocol);
      this.entry.set(data);
      this.name.set(data['name'] ?? '');
      this.description.set(data['description'] ?? '');
      this.deviceType.set(data['deviceType'] ?? '');
      this.fileName.set(file?.name ?? '');
      this.content.set(data['url'] ? await this.repository.getBinaryText(data['url'], { allowExternal: false }) : '');
      // Changes made elsewhere, such as points from a namespace file, replace the saved text.
      const handed = this.pending.takeFor(id);
      if (handed) {
        this.content.set(handed.text);
        this.handedNote.set(handed.note);
        // Coming back from Create from namespace: the editor's own fields, unsaved edits included.
        if (handed.name !== undefined) this.name.set(handed.name);
        if (handed.description !== undefined) this.description.set(handed.description);
        if (handed.deviceType !== undefined) this.deviceType.set(handed.deviceType);
      }
      // A file the form cannot read opens as TOML, where the problem is marked.
      if (this.result().config) {
        this.showForm();
      } else {
        this.showToml();
      }
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  /**
   * Opens Create from namespace for this file (and, for a connector file, device `d`), with the
   * editor's current text: the picker adds the points to it and comes back here, unsaved edits
   * included.
   */
  fromNamespace(d?: number): void {
    const config = this.result().config;
    const device = d === undefined ? undefined : String(config?.['device']?.[d]?.['name'] ?? '') || undefined;
    this.pickerContext.set({
      kind: this.kind(),
      protocol: String((this.isLibrary() ? config?.['library']?.['protocol'] : config?.['connector']?.['protocol']) ?? ''),
      entryId: this.isNew() ? undefined : String(this.entry()?.id ?? '') || undefined,
      name: this.name(),
      description: this.description(),
      fileName: normalizeName(this.fileName()),
      deviceType: this.deviceType(),
      text: this.content(),
      device,
    });
    void this.router.navigate(['/ot-connectors', 'namespace'], { queryParams: { from: 'editor' } });
  }

  useTemplate(protocol: ProtocolId): void {
    this.content.set(this.isLibrary() ? emitLibraryToml(newLibrary(protocol)) : emitToml(newConfig(protocol)));
  }

  /** The file in the standard layout of its kind. */
  private emit(config: Config): string {
    return this.isLibrary() ? emitLibraryToml(config) : emitToml(config);
  }

  /** A saved library must keep the protocol its type and path name. */
  private protocolProblems(config: Config) {
    const saved = this.savedProtocol();
    const protocol = config['library']?.protocol;
    if (!saved || protocol === undefined || protocol === saved) {
      return [];
    }
    const path = '/library/protocol';
    return [{ path, message: `must stay "${saved}": the library's type and its folder on gateways name it`, label: describePointer(config, path) }];
  }

  private async loadCatalog(): Promise<void> {
    try {
      this.catalog.set(await this.ops.catalog());
    } catch (e) {
      this.alert.addServerFailure(e);
    }
  }

  showForm(): void {
    this.editor = undefined;
    this.view.set('form');
  }

  showToml(): void {
    this.view.set('toml');
  }

  /**
   * A form edit, written into the file's text so its comments and layout stay. When the edit
   * cannot be written that way, the whole file is rewritten in the standard layout, after asking
   * if that drops anything.
   */
  async onFormChange(config: Config): Promise<void> {
    const text = this.content();
    const kept = writeKeepingLayout(text, config);
    if (kept !== undefined) {
      this.content.set(kept);
    } else {
      const current = this.result().config;
      const losesNothing = !!current && this.emit(current) === text;
      if (!losesNothing && !(await this.confirmRewrite(commentLines(text)))) {
        return;
      }
      this.content.set(this.emit(config));
    }
    // "Start from" names the file's protocol, also after the form switched it.
    const protocol = config['connector']?.protocol ?? config['library']?.protocol;
    if ((PROTOCOL_IDS as readonly string[]).includes(protocol)) {
      this.startFrom.set(protocol);
    }
  }

  private async confirmRewrite(comments: number): Promise<boolean> {
    const drops = comments > 0 ? ` and drops its ${comments} comment ${comments === 1 ? 'line' : 'lines'}` : '';
    return this.modal
      .confirm(
        gettext('Rewrite the file?'),
        `This change cannot be written into the file as it is. Applying it rewrites the whole file in the standard layout${drops}. Cancel keeps the file unchanged.`,
        Status.WARNING,
        { ok: gettext('Rewrite the file'), cancel: gettext('Cancel') }
      )
      .then(ok => ok !== false)
      .catch(() => false);
  }

  async onEditorInit(editor: Monaco.editor.IStandaloneCodeEditor): Promise<void> {
    this.editor = editor;
    const monaco = await loadMonacoEditor();
    this.monaco = monaco;
    if (!monaco.languages.getLanguages().some(l => l.id === TOML_LANGUAGE_ID)) {
      monaco.languages.register({ id: TOML_LANGUAGE_ID, extensions: ['.toml'] });
      monaco.languages.setMonarchTokensProvider(TOML_LANGUAGE_ID, tomlLanguage as Monaco.languages.IMonarchLanguage);
    }
    const model = editor.getModel();
    if (model) {
      monaco.editor.setModelLanguage(model, TOML_LANGUAGE_ID);
    }
    this.applyMarkers(this.result());
  }

  private applyMarkers(result: ValidationResult): void {
    const model = this.editor?.getModel();
    if (!this.monaco || !model) {
      return;
    }
    const syntax = result.syntax;
    this.monaco.editor.setModelMarkers(
      model,
      MARKER_OWNER,
      syntax
        ? [
            {
              startLineNumber: syntax.line,
              startColumn: syntax.column,
              endLineNumber: syntax.line,
              endColumn: syntax.column + 1,
              message: syntax.message,
              severity: this.monaco.MarkerSeverity.Error,
            },
          ]
        : []
    );
  }

  cancel(): void {
    void this.router.navigate(['/ot-connectors']);
  }

  async save(): Promise<void> {
    if (!this.canSave()) {
      return;
    }
    this.saving.set(true);
    const name = normalizeName(this.fileName());
    const library = this.isLibrary();
    const type = this.entry()?.['configurationType'] ?? (library ? libraryTypeOf(this.libraryProtocol(), name) : typeOf(name));
    try {
      const file = new File([this.content()], `${name}.toml`, { type: 'text/plain' });
      await this.repository.create(
        {
          version: this.name().trim(),
          description: this.description().trim(),
          deviceType: this.deviceType().trim() || undefined,
          configurationType: type,
          binary: { file },
        },
        RepositoryType.CONFIGURATION,
        this.entry()
      );
      this.alert.success(library ? gettext('Point library saved.') : gettext('Connector file saved.'));
      void this.router.navigate(['/ot-connectors']);
    } catch (e) {
      this.alert.addServerFailure(e);
    } finally {
      this.saving.set(false);
    }
  }
}
