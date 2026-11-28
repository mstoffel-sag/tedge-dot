import { Component, computed, inject, input, output } from '@angular/core';
import { CoreModule } from '@c8y/ngx-components';

import { PROTOCOL_IDS, getAt, protocolInfo, type Config } from '../generated/config-builder';
import { addLibraryPoint, extraKeys, pointSummary, removeLibraryPoint, switchLibraryProtocol } from '../shared/form-model';
import { type FieldProblem } from '../shared/validation';
import { FormFieldComponent, FormHelpComponent, FormPointComponent, FormState } from './connector-form.component';

/** The keys each level of a library has fields for; others are kept and listed. */
const KNOWN = {
  file: ['library', 'point'],
  library: ['protocol', 'type', 'description', 'version', 'namespaces'],
};

/**
 * The form view of a point library (contract §3.4): its [library] settings and its points, with
 * the point fields of a connector file's devices. Like the connector form, it emits each edit as
 * a new configuration, and the editor writes that into the file's text.
 */
@Component({
  selector: 'tdot-library-form',
  imports: [CoreModule, FormFieldComponent, FormHelpComponent, FormPointComponent],
  providers: [FormState],
  template: `
    <div class="row d-flex flex-wrap">
      <div class="col-xs-12 col-sm-6 col-md-3">
        <c8y-form-group class="form-group-sm" [hasError]="!!state.error('/library/protocol')">
          <label for="tdot-library-protocol" title="protocol">
            {{ 'Protocol' | translate }}
            <tdot-help [text]="state.help('/library/protocol')" [label]="'Protocol' | translate" title="protocol" />
          </label>
          <div class="c8y-select-wrapper">
            <select class="form-control" id="tdot-library-protocol" [disabled]="protocolFixed()" (change)="switchTo($event)">
              @if (!protocol()) {
                <option value="" selected>{{ 'Not set' | translate }}</option>
              }
              @for (id of protocols; track id) {
                <option [value]="id" [selected]="id === protocol()">{{ label(id) }}</option>
              }
              @if (protocol() && !protocols.includes($any(protocol()))) {
                <option [value]="protocol()" selected>{{ protocol() }} ({{ 'not known to the form' | translate }})</option>
              }
            </select>
          </div>
          <c8y-messages
            [helpMessage]="(protocolFixed()
              ? 'Fixed once saved: the library type and its folder on the gateway name it.'
              : 'Switching gives points new addresses. A default ID such as temperature changes with it; IDs you chose stay.') | translate"
          >
            @if (state.error('/library/protocol'); as e) {
              <c8y-message>{{ e }}</c8y-message>
            }
          </c8y-messages>
        </c8y-form-group>
      </div>
    </div>

    <fieldset class="c8y-fieldset">
      <legend>{{ 'Library' | translate }} <span class="text-muted text-code m-l-4">[library]</span></legend>
      <div class="row d-flex flex-wrap">
        <tdot-field path="/library/type" label="Device type" hint="Inherited by devices that set none" />
        <tdot-field path="/library/version" />
        @if (protocol() === 'opcua' || library()['namespaces'] !== undefined) {
          <tdot-field path="/library/namespaces" kind="list" columns="col-xs-12 col-md-6" hint="Comma-separated, in index order" />
        }
        <tdot-field path="/library/description" columns="col-xs-12" />
      </div>
      @if (extra('/library', known.library).length) {
        <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra('/library', known.library); track k) {<code>{{ k }}</code> }</p>
      }
    </fieldset>

    <fieldset class="c8y-fieldset">
      <legend>{{ 'Points' | translate }} <span class="text-muted text-code m-l-4">[[point]]</span></legend>
      @for (point of points(); track $index; let p = $index) {
        <tdot-point
          [base]="'/point/' + p"
          [index]="p"
          [summary]="summary(point)"
          [grouped]="point['grouping'] !== undefined || point['batch'] !== undefined"
          (remove)="state.edit(removeAt(p))"
        />
      } @empty {
        <p class="text-muted">{{ 'A library needs at least one point.' | translate }}</p>
      }
      <button type="button" class="btn btn-link" (click)="state.edit(addPointFn)">
        <i class="m-r-4" c8yIcon="plus-circle"></i>{{ 'Add point' | translate }}
      </button>
      @if (protocol() === 'opcua') {
        <button type="button" class="btn btn-link" (click)="pointsFromNamespace.emit()">
          <i class="m-r-4" c8yIcon="sitemap"></i>{{ 'Add points from namespace' | translate }}
        </button>
      }
    </fieldset>
    @if (extra('', known.file).length) {
      <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra('', known.file); track k) {<code>{{ k }}</code> }</p>
    }
  `,
})
export class LibraryFormComponent {
  readonly state = inject(FormState);
  readonly config = input.required<Config>();
  readonly problems = input<readonly FieldProblem[]>([], { alias: 'errors' });
  /** Whether the protocol can no longer change: once the library is saved, its type names it. */
  readonly protocolFixed = input(false);
  readonly configChange = output<Config>();
  /** "Add points from namespace" (OPC UA libraries). */
  readonly pointsFromNamespace = output<void>();

  readonly protocols = PROTOCOL_IDS;
  readonly known = KNOWN;
  readonly protocol = this.state.protocol;
  readonly library = computed<Config>(() => this.config()['library'] ?? {});
  readonly points = computed<Config[]>(() => (Array.isArray(this.config()['point']) ? this.config()['point'] : []));

  readonly addPointFn = (config: Config) => addLibraryPoint(config);

  constructor() {
    this.state.bind({
      config: () => this.config(),
      problems: () => this.problems(),
      apply: next => this.configChange.emit(next),
    });
  }

  label(id: string): string {
    const info = protocolInfo(id);
    return `${info.label}${info.preview ? ' (preview)' : ''}`;
  }

  summary(point: Config): string {
    return pointSummary(this.config(), {}, point);
  }

  extra(path: string, known: readonly string[]): string[] {
    return extraKeys(path ? getAt(this.config(), path) : this.config(), known);
  }

  switchTo(event: Event): void {
    const protocol = (event.target as HTMLSelectElement).value;
    if (protocol) {
      this.state.edit(config => switchLibraryProtocol(config, protocol));
    }
  }

  removeAt(p: number) {
    return (config: Config) => removeLibraryPoint(config, p);
  }
}
