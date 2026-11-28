import { Component, Injectable, computed, inject, input, output, signal } from '@angular/core';
import { CoreModule } from '@c8y/ngx-components';
import { CollapseDirective } from 'ngx-bootstrap/collapse';
import { PopoverDirective } from 'ngx-bootstrap/popover';
import { TooltipDirective } from 'ngx-bootstrap/tooltip';

import { PROTOCOL_IDS, SCHEMA, coerce, getAt, protocolInfo, setAt, type Config } from '../generated/config-builder';
import { AGGREGATES, OUTPUTS, POLICIES, REPORT_FIELDS } from '../generated/form-options';
import { HELP, helpForPath } from '../generated/help';
import LIBRARY_SCHEMA from '../generated/point-library.schema.json';
import { type LibraryCatalog, type ResolvedDevice, resolveDevice } from '../shared/libraries';
import {
  addDevice,
  addPoint,
  displayValue,
  extraKeys,
  labelOf,
  pointSummary,
  removeDevice,
  removePoint,
  shapeFields,
  splitList,
  switchProtocol,
  type FieldKind,
} from '../shared/form-model';
import { type FieldProblem } from '../shared/validation';

// The form view of a connector file: the sections and fields of the docs site's config builder
// (site/src/components/tools/ConfigBuilder.tsx), on the same model, options and help texts, in
// the Codex patterns of the Web SDK (c8y-form-group, c8y-messages, collapse, c8y-switch).

const POINT = SCHEMA.$defs.point.properties;
const DEVICE = SCHEMA.$defs.device.properties;
const CONNECTOR = SCHEMA.properties.connector.properties;
const LIBRARY = LIBRARY_SCHEMA.properties.library.properties as Record<string, { description?: string }>;
const DEPRECATED_OUTPUT: Record<string, string> = { samples: 'single', measurements: 'group' };
/** The grid columns of one field. */
const FIELD_COLUMNS = 'col-xs-12 col-sm-6 col-md-3';

/** The keys each level of the form has fields for; others are kept and listed. */
const KNOWN = {
  file: ['connector', 'mqtt', 'connection', 'device'],
  connector: ['protocol', 'service_name', 'poll_interval', 'sampling_interval', 'log_level', 'output', 'max_inflight', 'max_inflight_total', 'report', 'measurement', 'grouping', 'batch'],
  device: ['name', 'poll_interval', 'type', 'default_mode', 'max_inflight', 'points_from', 'protocol_address', 'point'],
  point: ['id', 'datatype', 'access', 'unit', 'poll_interval', 'sampling_interval', 'subscribe', 'address', 'report', 'grouping', 'batch'],
};

interface FormSource {
  config: () => Config;
  problems: () => readonly FieldProblem[];
  apply: (next: Config) => void;
}

/** The file being edited, shared by the form's parts. The form binds it before its parts exist. */
@Injectable()
export class FormState {
  private source: FormSource = { config: () => ({}), problems: () => [], apply: () => undefined };
  readonly config = computed<Config>(() => this.source.config());
  readonly errors = computed<ReadonlyMap<string, string>>(() => new Map(this.source.problems().map(p => [p.path, p.message])));
  /** The protocol of a connector file, or of a point library. */
  readonly protocol = computed<string>(() => this.config()['connector']?.protocol ?? this.config()['library']?.protocol ?? '');
  /** Sections the user opened or closed; the others follow their default. */
  private readonly toggled = signal<ReadonlyMap<string, boolean>>(new Map());

  bind(source: FormSource): void {
    this.source = source;
  }

  value(path: string): any {
    return getAt(this.config(), path);
  }

  error(path: string): string | undefined {
    return this.errors().get(path);
  }

  hasErrorUnder(prefix: string): boolean {
    return [...this.errors().keys()].some(path => path.startsWith(prefix));
  }

  help(path: string): string | undefined {
    const library = /^\/library\/([a-z_]+)$/.exec(path);
    return library ? LIBRARY[library[1]]?.description : helpForPath(path, this.protocol());
  }

  isOpen(key: string, byDefault: boolean): boolean {
    return this.toggled().get(key) ?? byDefault;
  }

  toggle(key: string, byDefault: boolean): void {
    const next = new Map(this.toggled());
    next.set(key, !this.isOpen(key, byDefault));
    this.toggled.set(next);
  }

  set(path: string, value: unknown): void {
    this.edit(config => setAt(config, path, value));
  }

  edit(change: (config: Config) => Config): void {
    this.source.apply(change(this.config()));
  }
}

/** The help button of a field or section: its text opens on focus, titled with the TOML key. */
@Component({
  selector: 'tdot-help',
  imports: [CoreModule, PopoverDirective],
  template: `
    @if (text()) {
      <button
        type="button"
        class="btn-help"
        [attr.aria-label]="('Help' | translate) + ': ' + label()"
        [popover]="text()! | translate"
        [popoverTitle]="title() ?? ''"
        placement="right"
        triggers="focus"
        container="body"
      ></button>
    }
  `,
})
export class FormHelpComponent {
  readonly text = input<string | undefined>();
  readonly label = input('');
  /** Usually the TOML key, so the help says what the field is called in the file. */
  readonly title = input<string>();
}

export type FormFieldKind = FieldKind | 'toggle' | 'list';

/** One field: text, number, choice, switch, switch with a default, or a comma-separated list. */
@Component({
  selector: 'tdot-field',
  imports: [CoreModule, FormHelpComponent],
  host: { '[class]': 'columns()' },
  template: `
    <c8y-form-group class="form-group-sm" [hasError]="!!error()">
      <label [attr.for]="isSwitch() ? null : id()" [title]="key()">
        {{ name() | translate }}
        <tdot-help [text]="state.help(path())" [label]="name() | translate" [title]="key()" />
      </label>
      @switch (kind()) {
        @case ('check') {
          <div>
            <label class="c8y-switch" [title]="name() | translate">
              <input type="checkbox" [id]="id()" [checked]="value() === true" (change)="setChecked($event)" />
              <span></span>
              <span>{{ (value() === true ? 'On' : 'Off') | translate }}</span>
            </label>
          </div>
        }
        @case ('toggle') {
          <div>
            <label class="c8y-switch" [title]="name() | translate">
              <input type="checkbox" [id]="id()" [checked]="value() ?? fallback()" (change)="setToggle($event)" />
              <span></span>
              <span>{{ ((value() ?? fallback()) ? 'On' : 'Off') | translate }}</span>
            </label>
          </div>
        }
        @case ('choice') {
          <div class="c8y-select-wrapper">
            <select class="form-control" [id]="id()" [attr.aria-describedby]="id() + '-msg'" (change)="setChoice($event)">
              <option value="" [selected]="value() === undefined">{{ 'Not set' | translate }}</option>
              @for (o of choices(); track o) {
                <option [value]="o" [selected]="o === shownChoice()">{{ o }}</option>
              }
            </select>
          </div>
        }
        @default {
          <input
            class="form-control"
            type="text"
            spellcheck="false"
            [id]="id()"
            [attr.aria-describedby]="id() + '-msg'"
            [attr.inputmode]="kind() === 'integer' || kind() === 'number' ? 'decimal' : null"
            [value]="shown()"
            [placeholder]="example() ? ('e.g.' | translate) + ' ' + example() : ''"
            (input)="setText($event)"
          />
        }
      }
      <c8y-messages [attr.id]="id() + '-msg'" [helpMessage]="(hint() ?? '') | translate">
        @if (error(); as e) {
          <c8y-message>{{ e }}</c8y-message>
        }
      </c8y-messages>
    </c8y-form-group>
  `,
})
export class FormFieldComponent {
  readonly state = inject(FormState);
  readonly path = input.required<string>();
  readonly label = input<string>();
  readonly kind = input<FormFieldKind>('text');
  readonly options = input<readonly unknown[]>([]);
  /** An example of a valid value, shown as the placeholder. */
  readonly example = input<string>();
  /** A short note under the field, such as what an empty field means. */
  readonly hint = input<string>();
  /** For a toggle: the value an absent key means. */
  readonly fallback = input(false);
  readonly columns = input(FIELD_COLUMNS);

  readonly key = computed(() => this.path().split('/').pop() ?? '');
  readonly name = computed(() => this.label() ?? labelOf(this.key()));
  readonly id = computed(() => `tdot${this.path().replace(/[^a-z0-9]+/gi, '-')}`);
  readonly isSwitch = computed(() => this.kind() === 'check' || this.kind() === 'toggle');
  readonly value = computed(() => this.state.value(this.path()));
  readonly error = computed(() => this.state.error(this.path()));
  /** The options, plus the current value when the file holds one the form does not offer. */
  readonly choices = computed(() => {
    const options = this.options().map(String);
    const current = this.value();
    return current !== undefined && !options.includes(String(current)) ? [...options, String(current)] : options;
  });
  readonly shownChoice = computed(() => (this.value() === undefined ? '' : String(this.value())));

  /** What is being typed, kept while it still means the stored value ("1." while typing 1.5). */
  private readonly draft = signal<string | undefined>(undefined);
  readonly shown = computed(() => {
    const draft = this.draft();
    const value = this.value();
    if (draft !== undefined && JSON.stringify(this.parse(draft)) === JSON.stringify(value)) {
      return draft;
    }
    return this.kind() === 'list' && Array.isArray(value) ? value.join(', ') : displayValue(value);
  });

  setText(event: Event): void {
    const text = (event.target as HTMLInputElement).value;
    this.draft.set(text);
    this.state.set(this.path(), this.parse(text));
  }

  setChoice(event: Event): void {
    const text = (event.target as HTMLSelectElement).value;
    const option = this.options().find(o => String(o) === text);
    this.state.set(this.path(), text === '' ? undefined : (option ?? text));
  }

  setChecked(event: Event): void {
    this.state.set(this.path(), (event.target as HTMLInputElement).checked || undefined);
  }

  setToggle(event: Event): void {
    const on = (event.target as HTMLInputElement).checked;
    this.state.set(this.path(), on === this.fallback() ? undefined : on);
  }

  private parse(text: string): unknown {
    if (this.kind() === 'list') {
      const items = splitList(text);
      return items.length ? items : undefined;
    }
    return coerce(text, this.kind());
  }
}

/** A collapsible part of the form, in the Codex collapse pattern: a legend-style toggle. */
@Component({
  selector: 'tdot-section',
  imports: [CoreModule, CollapseDirective, FormHelpComponent],
  template: `
    <div class="d-flex a-i-center">
      <button
        type="button"
        class="legend form-block btn-clean text-left flex-grow"
        [attr.aria-expanded]="open()"
        [attr.aria-controls]="regionId()"
        (click)="state.toggle(key(), defaultOpen())"
      >
        {{ title() | translate }}
        @if (detail(); as d) {
          <span class="text-muted m-l-4">{{ d | translate }}</span>
        }
        <i class="m-l-4" [c8yIcon]="open() ? 'chevron-up' : 'chevron-down'"></i>
      </button>
      <tdot-help [text]="help()" [label]="title() | translate" [title]="helpTitle()" />
    </div>
    <div class="collapse" role="region" [id]="regionId()" [collapse]="!open()" [isAnimated]="true">
      <ng-content />
    </div>
  `,
})
export class FormSectionComponent {
  readonly state = inject(FormState);
  /** What the open state is kept under: the path of the part. */
  readonly key = input.required<string>();
  readonly title = input.required<string>();
  readonly detail = input<string>();
  readonly help = input<string>();
  readonly helpTitle = input<string>();
  readonly defaultOpen = input(false);
  readonly open = computed(() => this.state.isOpen(this.key(), this.defaultOpen()));
  readonly regionId = computed(() => `tdot-section${this.key().replace(/[^a-z0-9]+/gi, '-')}`);
}

/** The fields of a protocol table, from the builder's protocol shape. */
@Component({
  selector: 'tdot-shape',
  imports: [CoreModule, FormFieldComponent],
  template: `
    <div class="legend form-block">{{ legend() | translate }}</div>
    @if (state.error(base()); as e) {
      <div class="alert alert-danger" role="alert">{{ legend() | translate }}: {{ e }}</div>
    }
    <div class="row d-flex flex-wrap">
      @for (f of fields(); track f.key) {
        <tdot-field [path]="base() + '/' + f.key" [kind]="f.kind" [options]="f.options ?? []" [example]="f.placeholder" />
      } @empty {
        <p class="text-muted col-xs-12">{{ 'The form has no fields for this table on this protocol. Set them in the TOML view.' | translate }}</p>
      }
    </div>
    @if (extra().length) {
      <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra(); track k) {<code>{{ k }}</code> }</p>
    }
  `,
})
export class FormShapeComponent {
  readonly state = inject(FormState);
  readonly base = input.required<string>();
  readonly schema = input<any>();
  readonly legend = input('');
  readonly fields = computed(() => shapeFields(this.schema()));
  readonly extra = computed(() => extraKeys(this.state.value(this.base()), this.fields().map(f => f.key)));
}

/** A `report` table: the reporting policy of the connector or of one point. */
@Component({
  selector: 'tdot-report',
  imports: [CoreModule, FormFieldComponent, FormSectionComponent],
  template: `
    <tdot-section
      [key]="base()"
      title="Reporting"
      [detail]="level() === 'point' ? 'this point, over the connector setting' : ''"
      [help]="help()"
      helpTitle="report"
      [defaultOpen]="state.value(base()) !== undefined"
    >
      <div class="row d-flex flex-wrap">
        <tdot-field [path]="base() + '/on_change'" kind="check" />
        @for (f of fields; track f.key) {
          <tdot-field [path]="base() + '/' + f.key" [kind]="f.key === 'deadband' ? 'number' : 'text'" [hint]="f.hint" />
        }
      </div>
    </tdot-section>
  `,
})
export class FormReportComponent {
  readonly state = inject(FormState);
  readonly base = input.required<string>();
  readonly level = input<'connector' | 'point'>('connector');
  readonly fields = REPORT_FIELDS;
  readonly help = computed(() => HELP[`${this.level()}.report`]);
}

/** `[connector] measurement`: how grouped output names its measurements. */
@Component({
  selector: 'tdot-measurement',
  imports: [CoreModule, FormFieldComponent, FormSectionComponent],
  template: `
    <tdot-section [key]="base()" title="Measurement names" [help]="help" helpTitle="measurement" [defaultOpen]="state.value(base()) !== undefined">
      <div class="row d-flex flex-wrap">
        <tdot-field [path]="base() + '/type'" label="Type" hint="Empty: the protocol" />
        <tdot-field [path]="base() + '/fragment'" hint="Empty: the type" />
        <tdot-field [path]="base() + '/point_separator'" hint="Empty: off" />
      </div>
      @if (state.value(base() + '/group') !== undefined) {
        <div class="alert alert-warning" role="status">
          {{ '"group" is deprecated: it sets the type and the fragment. Set the type, and the fragment if needed, instead.' | translate }}
        </div>
      }
    </tdot-section>
  `,
})
export class FormMeasurementComponent {
  readonly state = inject(FormState);
  readonly base = input.required<string>();
  readonly help = HELP['connector.measurement'];
}

/** The `grouping` table of `[connector]` or a point; `owner` is the table that holds it. */
@Component({
  selector: 'tdot-grouping',
  imports: [CoreModule, FormFieldComponent, FormHelpComponent, FormSectionComponent],
  template: `
    <tdot-section
      [key]="base()"
      title="Grouping"
      [detail]="level() === 'point' ? 'this point, over the connector setting' : ''"
      [help]="help()"
      helpTitle="grouping"
      [defaultOpen]="state.value(base()) !== undefined || ownerTable()['batch'] !== undefined"
    >
      @if (ownerTable()['batch'] !== undefined) {
        <div class="alert alert-warning" role="status">
          {{ '"batch" is the deprecated name of "grouping".' | translate }}
          @if (ownerTable()['grouping'] === undefined) {
            <button type="button" class="btn btn-default btn-sm m-l-8" (click)="renameBatch()">{{ 'Rename it to grouping' | translate }}</button>
          }
        </div>
      }
      <div class="row d-flex flex-wrap">
        <tdot-field [path]="base() + '/by'" label="Group by" kind="choice" [options]="policies" />
        @if (by() === 'frame') {
          <tdot-field [path]="base() + '/window'" example="500ms" />
        }
        @if (sample()) {
          <tdot-field [path]="base() + '/every'" example="1s" />
          <div [class]="columns">
            <c8y-form-group class="form-group-sm">
              <label [for]="aggregateId()" title="aggregate">
                {{ 'Aggregate' | translate }}
                <tdot-help [text]="aggregateHelp" [label]="'Aggregate' | translate" title="aggregate" />
              </label>
              <div class="c8y-select-wrapper">
                <select class="form-control" [id]="aggregateId()" (change)="setAggregate($event)">
                  @for (a of aggregates; track a.id) {
                    <option [value]="a.id" [selected]="a.id === aggregate()">{{ a.label }}</option>
                  }
                </select>
              </div>
              <c8y-messages></c8y-messages>
            </c8y-form-group>
          </div>
        }
      </div>
    </tdot-section>
  `,
})
export class FormGroupingComponent {
  readonly state = inject(FormState);
  /** The path of the table that holds `grouping`: `/connector` or a point. */
  readonly owner = input.required<string>();
  readonly level = input<'connector' | 'point'>('connector');
  readonly policies = POLICIES.map(p => p.id);
  readonly aggregates = AGGREGATES;
  readonly aggregateHelp = HELP['grouping.aggregate'];
  readonly columns = FIELD_COLUMNS;

  readonly base = computed(() => `${this.owner()}/grouping`);
  readonly ownerTable = computed<Config>(() => this.state.value(this.owner()) ?? {});
  readonly help = computed(() => HELP[`${this.level()}.grouping`]);
  readonly by = computed(() => this.state.value(`${this.base()}/by`));
  readonly sample = computed(() => this.by() === 'sample' || (this.by() === undefined && this.state.value(`${this.base()}/aggregate`) !== undefined));
  readonly aggregate = computed(() => {
    const value = JSON.stringify(this.state.value(`${this.base()}/aggregate`) ?? null);
    return AGGREGATES.find(a => JSON.stringify(a.value) === value)?.id ?? 'none';
  });
  readonly aggregateId = computed(() => `tdot${this.base().replace(/[^a-z0-9]+/gi, '-')}-aggregate`);

  setAggregate(event: Event): void {
    const id = (event.target as HTMLSelectElement).value;
    this.state.set(`${this.base()}/aggregate`, AGGREGATES.find(a => a.id === id)?.value ?? undefined);
  }

  /** A deprecated `batch` table: the same policy, with `group_by` named `by`. */
  renameBatch(): void {
    const owner = this.ownerTable();
    const { group_by, ...rest } = owner['batch'] ?? {};
    const grouping = group_by === undefined ? rest : { ...rest, by: group_by };
    const { batch: _, ...others } = owner;
    this.state.set(this.owner(), { ...others, grouping });
  }
}

/**
 * One point, collapsed to its summary: the point fields of a device in a connector file and of a
 * point library are the same (contract §3.4).
 */
@Component({
  selector: 'tdot-point',
  imports: [
    CoreModule,
    CollapseDirective,
    TooltipDirective,
    FormFieldComponent,
    FormShapeComponent,
    FormReportComponent,
    FormGroupingComponent,
  ],
  template: `
    <div class="border-left p-l-16 m-b-8">
      <div class="d-flex a-i-center">
        <button
          type="button"
          class="btn-clean text-left flex-grow p-t-4 p-b-4"
          [attr.aria-expanded]="open()"
          [attr.aria-controls]="regionId()"
          (click)="state.toggle(base(), byDefault())"
        >
          <i class="m-r-4" [c8yIcon]="open() ? 'chevron-up' : 'chevron-down'"></i>
          <strong>{{ point()['id'] || ('Point' | translate) + ' ' + (index() + 1) }}</strong>
          <span class="text-muted m-l-8">{{ summary() }}</span>
          @if (overrides(); as library) {
            <span class="tag tag--info m-l-8">{{ 'overrides' | translate }} {{ library }}</span>
          }
        </button>
        <button
          type="button"
          class="btn btn-dot btn-dot--danger"
          [attr.aria-label]="('Remove point' | translate) + ' ' + (point()['id'] || index() + 1)"
          [tooltip]="'Remove point' | translate"
          [delay]="500"
          (click)="remove.emit()"
        >
          <i c8yIcon="minus-circle"></i>
        </button>
      </div>
      <div class="collapse" role="region" [id]="regionId()" [collapse]="!open()" [isAnimated]="true">
        <div class="row d-flex flex-wrap">
          <tdot-field [path]="base() + '/id'" />
          <tdot-field [path]="base() + '/datatype'" kind="choice" [options]="datatypes" />
          <tdot-field [path]="base() + '/access'" kind="choice" [options]="accesses" />
          <tdot-field [path]="base() + '/unit'" />
          <tdot-field [path]="base() + '/poll_interval'" hint="Empty: the device setting" />
          @if (state.protocol() === 'opcua') {
            <tdot-field [path]="base() + '/sampling_interval'" hint="Empty: the device setting" />
            <tdot-field [path]="base() + '/subscribe'" kind="toggle" [fallback]="true" />
          }
        </div>
        <tdot-shape [base]="base() + '/address'" [schema]="info().address" legend="Address" />
        <tdot-report [base]="base() + '/report'" level="point" />
        @if (grouped()) {
          <tdot-grouping [owner]="base()" level="point" />
        }
        @if (extra().length) {
          <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra(); track k) {<code>{{ k }}</code> }</p>
        }
      </div>
    </div>
  `,
})
export class FormPointComponent {
  readonly state = inject(FormState);
  /** The point's path, such as /device/0/point/2 or, in a library, /point/2. */
  readonly base = input.required<string>();
  readonly index = input(0);
  readonly summary = input('');
  /** Whether the point's grouping settings are shown. */
  readonly grouped = input(false);
  /** The library whose point this one overrides, if it does. */
  readonly overrides = input<string>();
  readonly remove = output<void>();

  readonly datatypes = POINT.datatype.enum as string[];
  readonly accesses = POINT.access.enum as string[];
  readonly point = computed<Config>(() => this.state.value(this.base()) ?? {});
  readonly info = computed(() => protocolInfo(this.state.protocol()));
  /** A point opens by itself when it has an error. */
  readonly byDefault = computed(() => this.state.hasErrorUnder(`${this.base()}/`));
  readonly open = computed(() => this.state.isOpen(this.base(), this.byDefault()));
  readonly regionId = computed(() => `tdot-point${this.base().replace(/[^a-z0-9]+/gi, '-')}`);
  readonly extra = computed(() => extraKeys(this.point(), KNOWN.point));
}

/**
 * A device's point libraries (`points_from`): an ordered list of names, added from the libraries
 * that exist for the file's protocol, so a reference cannot name one that does not.
 */
@Component({
  selector: 'tdot-libraries',
  imports: [CoreModule, TooltipDirective, FormHelpComponent],
  host: { class: 'col-xs-12 col-md-6' },
  template: `
    <c8y-form-group class="form-group-sm" [hasError]="!!error()">
      <label [for]="selectId()" title="points_from">
        {{ 'Point libraries' | translate }}
        <tdot-help [text]="state.help(path())" [label]="'Point libraries' | translate" title="points_from" />
      </label>
      @if (refs().length) {
        <ul class="list-group m-b-8">
          @for (ref of refs(); track $index; let i = $index; let first = $first; let last = $last) {
            <li class="list-group-item d-flex a-i-center p-t-4 p-b-4">
              <span class="flex-grow">
                <span class="text-muted m-r-4">{{ i + 1 }}.</span>
                <strong>{{ ref }}</strong>
                @if (sourceOf(ref); as source) {
                  <span class="text-muted small m-l-8">{{ source | translate }}</span>
                } @else {
                  <span class="text-danger small m-l-8">{{ 'not found' | translate }}</span>
                }
              </span>
              <button
                type="button"
                class="btn btn-clean btn-sm"
                [disabled]="first"
                [attr.aria-label]="('Move up' | translate) + ' ' + ref"
                [tooltip]="'Move up' | translate"
                [delay]="500"
                (click)="move(i, -1)"
              >
                <i c8yIcon="chevron-up"></i>
              </button>
              <button
                type="button"
                class="btn btn-clean btn-sm"
                [disabled]="last"
                [attr.aria-label]="('Move down' | translate) + ' ' + ref"
                [tooltip]="'Move down' | translate"
                [delay]="500"
                (click)="move(i, 1)"
              >
                <i c8yIcon="chevron-down"></i>
              </button>
              <button
                type="button"
                class="btn btn-dot btn-dot--danger"
                [attr.aria-label]="('Remove point library' | translate) + ' ' + ref"
                [tooltip]="'Remove point library' | translate"
                [delay]="500"
                (click)="removeAt(i)"
              >
                <i c8yIcon="minus-circle"></i>
              </button>
            </li>
          }
        </ul>
      }
      <div class="c8y-select-wrapper">
        <select class="form-control" [id]="selectId()" [disabled]="!available().length" (change)="pick($event)">
          <option value="" selected>
            {{ (available().length ? 'Add a point library' : refs().length ? 'No other library for this protocol' : 'No library for this protocol') | translate }}
          </option>
          @for (name of available(); track name) {
            <option [value]="name">{{ name }}</option>
          }
        </select>
      </div>
      <c8y-messages [helpMessage]="'Applied in this order, then the points of the device itself. A point with an ID that exists patches it.' | translate">
        @if (error(); as e) {
          <c8y-message>{{ e }}</c8y-message>
        }
      </c8y-messages>
    </c8y-form-group>
  `,
})
export class FormLibrariesComponent {
  readonly state = inject(FormState);
  /** The device's path, such as /device/0. */
  readonly device = input.required<string>();
  readonly catalog = input<LibraryCatalog>();

  readonly path = computed(() => `${this.device()}/points_from`);
  readonly selectId = computed(() => `tdot${this.path().replace(/[^a-z0-9]+/gi, '-')}`);
  readonly refs = computed<string[]>(() => {
    const value = this.state.value(this.path());
    return Array.isArray(value) ? value.map(String) : [];
  });
  readonly error = computed(() => this.state.error(this.path()));
  readonly available = computed(() => (this.catalog()?.names(this.state.protocol()) ?? []).filter(n => !this.refs().includes(n)));

  sourceOf(ref: string): string | undefined {
    const known = this.catalog()?.get(this.state.protocol(), ref);
    return known ? (known.source === 'packaged' ? 'installed with tedge-dot' : 'repository') : undefined;
  }

  pick(event: Event): void {
    const select = event.target as HTMLSelectElement;
    const name = select.value;
    select.value = '';
    if (name) {
      this.write([...this.refs(), name]);
    }
  }

  move(i: number, by: -1 | 1): void {
    const refs = [...this.refs()];
    [refs[i], refs[i + by]] = [refs[i + by], refs[i]];
    this.write(refs);
  }

  removeAt(i: number): void {
    this.write(this.refs().filter((_, j) => j !== i));
  }

  private write(refs: string[]): void {
    this.state.set(this.path(), refs.length ? refs : undefined);
  }
}

/**
 * The form view of a connector file. It shows `config` and emits each edit as a whole new
 * configuration; the editor writes that into the file's text.
 */
@Component({
  selector: 'tdot-connector-form',
  imports: [
    CoreModule,
    TooltipDirective,
    FormFieldComponent,
    FormHelpComponent,
    FormSectionComponent,
    FormShapeComponent,
    FormReportComponent,
    FormMeasurementComponent,
    FormGroupingComponent,
    FormPointComponent,
    FormLibrariesComponent,
  ],
  providers: [FormState],
  template: `
    <div class="row d-flex flex-wrap">
      <div [class]="columns">
        <c8y-form-group class="form-group-sm">
          <label for="tdot-protocol" title="protocol">
            {{ 'Protocol' | translate }}
            <tdot-help [text]="protocolHelp" [label]="'Protocol' | translate" title="protocol" />
          </label>
          <div class="c8y-select-wrapper">
            <select class="form-control" id="tdot-protocol" (change)="switchTo($event)">
              @for (id of protocols; track id) {
                <option [value]="id" [selected]="id === protocol()">{{ label(id) }}</option>
              }
              @if (protocol() && !protocols.includes($any(protocol()))) {
                <option [value]="protocol()" selected>{{ protocol() }} ({{ 'not known to the form' | translate }})</option>
              }
            </select>
          </div>
          <c8y-messages [helpMessage]="'Switching gives devices and points new addresses. Default names such as opc1 and temperature change with it; names you chose stay.' | translate"></c8y-messages>
        </c8y-form-group>
      </div>
    </div>

    <fieldset class="c8y-fieldset">
      <legend>{{ 'Connector' | translate }} <span class="text-muted text-code m-l-4">[connector]</span></legend>
      <div class="row d-flex flex-wrap">
        <tdot-field path="/connector/service_name" />
        <tdot-field path="/connector/poll_interval" example="2s" />
        @if (protocol() === 'opcua') {
          <tdot-field path="/connector/sampling_interval" hint="Empty: the poll interval" />
        }
        <tdot-field path="/connector/log_level" kind="choice" [options]="logLevels" />
        <tdot-field path="/connector/output" kind="choice" [options]="outputs()" />
        <tdot-field path="/connector/max_inflight" kind="integer" hint="Empty: 4" />
        <tdot-field path="/connector/max_inflight_total" kind="integer" hint="Empty: 32" />
      </div>
      @if (deprecatedOutput(); as replacement) {
        <div class="alert alert-warning" role="status">
          output = "{{ connector()['output'] }}" {{ 'is deprecated. It means' | translate }} "{{ replacement }}".
          <button type="button" class="btn btn-default btn-sm m-l-8" (click)="state.set('/connector/output', replacement)">
            {{ 'Use' | translate }} "{{ replacement }}"
          </button>
        </div>
      }
      <tdot-report base="/connector/report" level="connector" />
      @if (grouped()) {
        <tdot-measurement base="/connector/measurement" />
      }
      @if (grouped() || connector()['grouping'] || connector()['batch']) {
        <tdot-grouping owner="/connector" level="connector" />
      }
      @if (extra('/connector', known.connector).length) {
        <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra('/connector', known.connector); track k) {<code>{{ k }}</code> }</p>
      }
    </fieldset>

    <fieldset class="c8y-fieldset">
      <legend>MQTT <span class="text-muted text-code m-l-4">[mqtt]</span></legend>
      <div class="row d-flex flex-wrap">
        <tdot-field path="/mqtt/host" hint="Empty: 127.0.0.1" />
        <tdot-field path="/mqtt/port" kind="integer" hint="Empty: 1883" />
      </div>
    </fieldset>

    @if (hasConnection()) {
      <fieldset class="c8y-fieldset">
        <legend>{{ 'Connection' | translate }} <span class="text-muted text-code m-l-4">[connection]</span></legend>
        <tdot-shape base="/connection" [schema]="info().connection" legend="Shared connection settings" />
      </fieldset>
    }

    @for (device of devices(); track $index; let d = $index) {
      <fieldset class="c8y-fieldset">
        <legend>
          {{ 'Device' | translate }} {{ device['name'] || '#' + (d + 1) }}
          <span class="text-muted text-code m-l-4">[[device]]</span>
          <button
            type="button"
            class="btn btn-dot btn-dot--danger m-l-8"
            [attr.aria-label]="('Remove device' | translate) + ' ' + (device['name'] || d + 1)"
            [tooltip]="'Remove device' | translate"
            [delay]="500"
            (click)="state.edit(removeDeviceAt(d))"
          >
            <i c8yIcon="minus-circle"></i>
          </button>
        </legend>
        <div class="row d-flex flex-wrap">
          <tdot-field [path]="'/device/' + d + '/name'" />
          <tdot-field [path]="'/device/' + d + '/poll_interval'" hint="Empty: the connector setting" />
          <tdot-libraries [device]="'/device/' + d" [catalog]="catalog()" />
        </div>
        <tdot-section
          [key]="'/device/' + d + '/settings'"
          title="Device settings"
          detail="type, default mode, requests in flight, protocol address"
          [defaultOpen]="hasDeviceError(d)"
        >
          <div class="row d-flex flex-wrap">
            <tdot-field [path]="'/device/' + d + '/type'" [hint]="typeHint(d)" />
            <tdot-field [path]="'/device/' + d + '/default_mode'" kind="choice" [options]="defaultModes" />
            <tdot-field [path]="'/device/' + d + '/max_inflight'" kind="integer" hint="Empty: the connector setting" />
          </div>
          <tdot-shape [base]="'/device/' + d + '/protocol_address'" [schema]="info().protocol_address" legend="Protocol address" />
        </tdot-section>

        <div class="legend form-block">{{ 'Points' | translate }}</div>
        @if (resolved()[d]; as r) {
          @if (r.inherited.length) {
            <tdot-section [key]="'/device/' + d + '/inherited'" title="From point libraries" [detail]="inheritedDetail(r)">
              <div class="table-responsive">
                <table class="table table-condensed m-b-8">
                  <thead>
                    <tr>
                      <th>{{ 'ID' | translate }}</th>
                      <th>{{ 'Data type' | translate }}</th>
                      <th>{{ 'Unit' | translate }}</th>
                      <th>{{ 'Address' | translate }}</th>
                      <th>{{ 'Library' | translate }}</th>
                      <th class="text-right"><span class="sr-only">{{ 'Actions' | translate }}</span></th>
                    </tr>
                  </thead>
                  <tbody>
                    @for (ip of r.inherited; track $index) {
                      <tr>
                        <td>{{ ip.point['id'] }}</td>
                        <td>{{ ip.point['datatype'] }}</td>
                        <td>{{ ip.point['unit'] }}</td>
                        <td class="text-code small">{{ show(ip.point['address']) }}</td>
                        <td>{{ ip.library }}</td>
                        <td class="text-right">
                          <button type="button" class="btn btn-link btn-xs" (click)="state.edit(overridePoint(d, ip.point['id']))">
                            {{ 'Override' | translate }}
                          </button>
                        </td>
                      </tr>
                    }
                  </tbody>
                </table>
              </div>
              <p class="text-muted small">
                {{ 'Read-only here: edit the library to change them for every device. Override adds a point with the same ID to this device, which patches the point of the library.' | translate }}
              </p>
            </tdot-section>
          }
        }
        @for (point of points(device); track $index; let p = $index) {
          <tdot-point
            [base]="pointKey(d, p)"
            [index]="p"
            [summary]="summary(device, point)"
            [grouped]="grouped()"
            [overrides]="resolved()[d]?.overrides?.get(p)"
            (remove)="state.edit(removePointAt(d, p))"
          />
        }
        @if (extra('/device/' + d, known.device).length) {
          <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra('/device/' + d, known.device); track k) {<code>{{ k }}</code> }</p>
        }
        <button type="button" class="btn btn-link" (click)="state.edit(addPointTo(d))">
          <i c8yIcon="plus-circle"></i> {{ 'Add point' | translate }}
        </button>
        @if (protocol() === 'opcua') {
          <button
            type="button"
            class="btn btn-link"
            [tooltip]="'Pick nodes from a scan of this device\\'s server; the points are added to this device' | translate"
            [delay]="500"
            (click)="pointsFromNamespace.emit(d)"
          >
            <i c8yIcon="sitemap"></i> {{ 'Add points from namespace' | translate }}
          </button>
        }
      </fieldset>
    }
    <button type="button" class="btn btn-link m-t-8" (click)="state.edit(addDeviceFn)">
      <i c8yIcon="plus-circle"></i> {{ 'Add device' | translate }}
    </button>
    @if (extra('', known.file).length) {
      <p class="text-muted small">{{ 'Also set, kept as is:' | translate }} @for (k of extra('', known.file); track k) {<code>{{ k }}</code> }</p>
    }
  `,
})
export class ConnectorFormComponent {
  readonly state = inject(FormState);
  readonly config = input.required<Config>();
  readonly problems = input<readonly FieldProblem[]>([], { alias: 'errors' });
  /** The point libraries devices may reference; without it, the form shows no inherited points. */
  readonly catalog = input<LibraryCatalog>();
  readonly configChange = output<Config>();
  /** "Add points from namespace" on device `d` (OPC UA). */
  readonly pointsFromNamespace = output<number>();

  readonly protocols = PROTOCOL_IDS;
  readonly protocolHelp = HELP['connector.protocol'];
  readonly logLevels = CONNECTOR.log_level.enum as string[];
  readonly defaultModes = DEVICE.default_mode.enum as string[];
  readonly known = KNOWN;
  readonly columns = FIELD_COLUMNS;

  readonly protocol = this.state.protocol;
  readonly connector = computed<Config>(() => this.config()['connector'] ?? {});
  readonly info = computed(() => protocolInfo(this.protocol()));
  readonly devices = computed<Config[]>(() => (Array.isArray(this.config()['device']) ? this.config()['device'] : []));
  readonly grouped = computed(() => ['group', 'measurements'].includes(this.connector()['output']));
  readonly deprecatedOutput = computed(() => DEPRECATED_OUTPUT[this.connector()['output']]);
  readonly outputs = computed(() => (this.deprecatedOutput() ? [...OUTPUTS, this.connector()['output']] : OUTPUTS));
  readonly hasConnection = computed(
    () => Object.keys(this.info()?.connection?.properties ?? {}).length > 0 || this.config()['connection'] !== undefined
  );

  /** What each device inherits from its point libraries. */
  readonly resolved = computed<(ResolvedDevice | undefined)[]>(() => {
    const catalog = this.catalog();
    return this.devices().map(device => (catalog ? resolveDevice(device, this.protocol(), catalog) : undefined));
  });

  readonly addDeviceFn = (config: Config) => addDevice(config);
  readonly show = displayValue;

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

  points(device: Config): Config[] {
    return Array.isArray(device['point']) ? device['point'] : [];
  }

  pointKey(d: number, p: number): string {
    return `/device/${d}/point/${p}`;
  }

  inheritedDetail(resolved: ResolvedDevice): string {
    const n = resolved.inherited.length;
    return `${n} ${n === 1 ? 'point' : 'points'}`;
  }

  /** The hint of a device's type: the type it inherits when the field is empty. */
  typeHint(d: number): string | undefined {
    const type = this.resolved()[d]?.type;
    return type ? `Empty: ${type.name}, from ${type.library}` : undefined;
  }

  /** Adds a point with an inherited point's ID to device `d`: it patches the library's point. */
  overridePoint(d: number, id: string) {
    return (config: Config) => setAt(config, `/device/${d}/point/${this.points(getAt(config, `/device/${d}`) ?? {}).length}`, { id });
  }

  summary(device: Config, point: Config): string {
    return pointSummary(this.config(), device, point);
  }

  extra(path: string, known: readonly string[]): string[] {
    return extraKeys(path ? getAt(this.config(), path) : this.config(), known);
  }

  hasDeviceError(d: number): boolean {
    return [...this.state.errors().keys()].some(path => path.startsWith(`/device/${d}/`) && !path.includes('/point/'));
  }

  switchTo(event: Event): void {
    const protocol = (event.target as HTMLSelectElement).value;
    this.state.edit(config => switchProtocol(config, protocol));
  }

  addPointTo(d: number) {
    return (config: Config) => addPoint(config, d);
  }

  removePointAt(d: number, p: number) {
    return (config: Config) => removePoint(config, d, p);
  }

  removeDeviceAt(d: number) {
    return (config: Config) => removeDevice(config, d);
  }
}
