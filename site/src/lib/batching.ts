// Simulation behind the grouping playground: take what each signal's reporting policy published
// (src/lib/playground.ts), feed it to the grouped-output engine (src/lib/batch.ts) with the
// connector configuration, and trace which reading went into which measurement and which signals
// each measurement is missing.

import { Batcher, groupByOf, groupingOf, levelNaming, mergeGroupingLevel, resolveName, type Name, type Message } from './batch';
import { parseDuration } from './report';
import { simulateSignal, type Run, type SignalSpec } from './playground';

/** The `output` values the tools offer; "samples" and "measurements" are their deprecated names. */
export const OUTPUTS = ['single', 'group'];

/** Every `grouping.by`, as the playground offers them (a test holds them to the schema). */
export const POLICIES: { id: 'time' | 'frame' | 'sample'; text: string }[] = [
  { id: 'time', text: 'Readings with the same timestamp share a measurement. Nothing is lost, no time is moved. A signal that published nothing at that time is not in the measurement.' },
  { id: 'frame', text: 'Readings within a window share a measurement stamped with its first time. Nothing is lost; a time moves by less than the window. A signal that published nothing in the window is not in it.' },
  { id: 'sample', text: 'Every period, the value each signal holds, stamped with the sample time, whether it changed or not. Readings in between are not sent. A signal with no good value (never read, or in an outage) is left out.' },
];

/** The `grouping.aggregate` choices under "sample" (a test checks they cover every aggregate). */
export const AGGREGATES: { id: string; label: string; value: string | string[] | null }[] = [
  { id: 'none', label: 'none (sample and hold)', value: null },
  { id: 'mean', label: '"mean"', value: 'mean' },
  { id: 'mean_min_max', label: '["mean", "min", "max"]', value: ['mean', 'min', 'max'] },
  { id: 'all', label: '["mean", "min", "max", "last"]', value: ['mean', 'min', 'max', 'last'] },
];

export const WINDOWS = ['100ms', '250ms', '500ms', '1s', '2s', '5s'];
export const EVERY = ['500ms', '1s', '5s', '10s', '30s', '1m'];

export interface GroupingForm {
  by: (typeof POLICIES)[number]['id'];
  window: string;
  every: string;
  /** An `AGGREGATES` id. */
  aggregate: string;
}

export function groupingTable(form: GroupingForm): Record<string, unknown> {
  if (form.by === 'frame') return { by: 'frame', window: form.window };
  if (form.by === 'sample') {
    const aggregate = AGGREGATES.find((a) => a.id === form.aggregate)?.value ?? null;
    return aggregate === null ? { by: 'sample', every: form.every } : { aggregate, every: form.every };
  }
  return { by: 'time' };
}

const inline = (t: Record<string, unknown>) =>
  Object.entries(t)
    .map(([k, v]) => `${k} = ${Array.isArray(v) ? `[${v.map((x) => JSON.stringify(x)).join(', ')}]` : JSON.stringify(v)}`)
    .join(', ');

export function groupingToml(form: GroupingForm): string {
  return `grouping = { ${inline(groupingTable(form))} }`;
}

/**
 * How the playground lays out the fragments of its measurements (contract §5.5 naming):
 * - `type`: one fragment named after the type, holding every series;
 * - `signal`: a fragment per signal (`meta.measurement.fragment = "<id>"`, series `value`), all
 *   of one type, so a measurement holds several fragments;
 * - `custom`: the points name their fragments otherwise; the playground runs them as they are.
 */
export type FragmentLayout = 'type' | 'signal' | 'custom';

export const FRAGMENT_LAYOUTS: { id: Exclude<FragmentLayout, 'custom'>; label: string; text: string }[] = [
  {
    id: 'type',
    label: 'one fragment',
    text: 'Every series in one fragment named after the type. In Streaming Lake Ingestion: one table, and one write per measurement.',
  },
  {
    id: 'signal',
    label: 'a fragment per signal',
    text: 'Each signal in a fragment of its own, all of one type, so a measurement holds several fragments. In Streaming Lake Ingestion: a table per signal, and one write per fragment of each measurement.',
  },
];

/** The series name a point gets in the `signal` layout. */
export const SIGNAL_SERIES = 'value';

/** The fragment layout of a configuration's first device. */
export function layoutOf(config: Record<string, any>): FragmentLayout {
  const points = ((config.device?.[0]?.point ?? []) as any[]);
  const names = points.map((p) => p.meta?.measurement);
  if (names.every((m) => m === undefined)) return 'type';
  if (points.every((p) => p.meta?.measurement?.fragment === p.id && p.meta.measurement.series === SIGNAL_SERIES && Object.keys(p.meta.measurement).length === 2)) {
    return 'signal';
  }
  return 'custom';
}

/** The configuration with its first device's points laid out as `layout` says. */
export function withLayout(config: Record<string, any>, layout: Exclude<FragmentLayout, 'custom'>): Record<string, any> {
  const device = config.device?.[0];
  if (!device) return config;
  const point = ((device.point ?? []) as any[]).map((p) => {
    const { measurement: _, ...meta } = p.meta ?? {};
    const next = layout === 'signal' ? { ...meta, measurement: { fragment: p.id, series: SIGNAL_SERIES } } : meta;
    const { meta: __, ...rest } = p;
    return Object.keys(next).length ? { ...rest, meta: next } : rest;
  });
  return { ...config, device: [{ ...device, point }, ...(config.device ?? []).slice(1)] };
}

/** The `[connector]` lines and a point's `meta` the playground's settings stand for. */
export function connectorToml(form: GroupingForm, type: string, layout: FragmentLayout): string {
  const lines = ['[connector]', 'output = "group"', `measurement = { type = ${JSON.stringify(type)} }`, groupingToml(form)];
  if (layout === 'signal') {
    lines.push('', '[[device.point]]', 'id = "temperature"', `meta = { measurement = { fragment = "temperature", series = "${SIGNAL_SERIES}" } }`);
  }
  return lines.join('\n');
}

/** One sample the reporting policy published: an input of the batcher. */
export interface Input {
  /** Index into the signals. */
  signal: number;
  /** The reading's time, ms from the start. */
  ts: number;
  /** When it was published (later than `ts` when `min_interval` held it). */
  at: number;
  value: number;
  quality: 'good' | 'bad' | 'stale';
}

export interface Measurement {
  /** The measurement's `time`, ms from the start. */
  time: number;
  /** When the connector published it, ms from the start. */
  publishedAt: number;
  /** Its type: the topic's last segment and the Cumulocity `type`. */
  type: string;
  /** Fragment → series name → value, in the order the connector wrote them. */
  fragments: Record<string, Record<string, number>>;
  /** For each signal: whether it has a series in this measurement. */
  has: boolean[];
  /** Indexes into the inputs that went into it (for an aggregate: every input of the period). */
  from: number[];
}

export interface GroupRun {
  signals: SignalSpec[];
  /** Each signal's readings and what its reporting policy published. */
  reports: Run[];
  inputs: Input[];
  measurements: Measurement[];
  /** Frame windows (frame), for the chart. */
  windows: { from: number; to: number }[];
  /** Sample times (sample). */
  samples: number[];
  /** Good inputs that reached no measurement. */
  dropped: number[];
  /** Measurement × signal cells without a series of that signal. */
  missing: number;
  /** The largest distance between a reading's time and its measurement's time. */
  maxShift: number;
  /** `<fragment>.<series>` → unit, from the units metadata. */
  units: Record<string, string>;
  /** Each signal's type, fragment and series. */
  names: Name[];
  /** The device the measurements are for, and the topic and type of the first signal's. */
  device: string;
  topic: string;
  type: string;
}

const BASE = Date.parse('2026-01-01T08:00:00.000Z');
export const isoAt = (t: number) => new Date(BASE + t).toISOString();

/** Whether `series` is the series `name` or one of its aggregates, `<name>_<aggregate>`. */
export const isSeriesOf = (name: string, series: string) => series === name || series.startsWith(`${name}_`);

/** The signal a series of a fragment belongs to. */
function signalOfSeries(names: Name[], type: string, fragment: string, series: string): number {
  return names.findIndex((n) => n.type === type && n.fragment === fragment && isSeriesOf(n.series, series));
}

/** The series and values of `m` that belong to `name`. */
export function seriesOf(m: Measurement, name: Name): [string, number][] {
  if (m.type !== name.type) return [];
  return Object.entries(m.fragments[name.fragment] ?? {}).filter(([s]) => isSeriesOf(name.series, s));
}

/**
 * Run every signal through its reporting policy, then through the batcher configured by
 * `config` (a connector configuration as TOML parses it). The signals are the points of its
 * first device, by id.
 */
export function simulate(signals: SignalSpec[], durationMs: number, config: Record<string, any>, reports?: Run[]): GroupRun {
  reports ??= signals.map((s) => simulateSignal(s, durationMs));
  const inputs: Input[] = reports
    .flatMap((run, signal) =>
      run.published.map((p) => {
        const r = run.readings[p.index];
        return { signal, ts: r.t, at: p.at, value: r.value, quality: r.quality };
      }),
    )
    .sort((a, b) => a.at - b.at || a.signal - b.signal);

  const connector = config.connector ?? {};
  const device = config.device?.[0] ?? {};
  const deviceName: string = device.name ?? '';
  const points = signals.map((s) => ((device.point ?? []) as any[]).find((p) => p.id === s.id) ?? { id: s.id });
  const m = connector.measurement ?? {};
  const naming = { connector: levelNaming(m), separator: typeof m.point_separator === 'string' ? m.point_separator : '' };
  const names = points.map((p) => resolveName(naming, connector.protocol ?? '', device, p));
  const type = names[0]?.type ?? connector.protocol ?? '';
  // The playground's chart marks follow the first point's grouping policy.
  const mergedOf = (point: any) => [connector, device, point].map(groupingOf).reduce(mergeGroupingLevel, {});
  const policy = groupByOf(mergedOf(points[0])) ?? { kind: 'time' };

  const batcher = new Batcher(config, 0);
  const out: Message[] = [];
  const published: { message: Message; at: number }[] = [];
  const windows: { from: number; to: number }[] = [];
  const window = policy.kind === 'frame' ? policy.window : 0;
  const every = policy.kind === 'sample' ? policy.every : 0;
  // Pushed source times run a few ms past the sampling grid.
  const limit = durationMs + window + 10;
  let units: Record<string, string> = {};
  const drain = (at: number) => {
    for (const message of out.splice(0)) {
      if (message.topic.endsWith('/meta')) {
        const meta = message.payload ? JSON.parse(message.payload) : {};
        units = { ...units, ...Object.fromEntries(Object.entries(meta).map(([k, v]: [string, any]) => [k, v.unit])) };
      } else published.push({ message, at });
    }
  };

  // Event by event: the inputs published at one time are one runtime pass.
  let next = 0;
  for (;;) {
    const t = Math.min(next < inputs.length ? inputs[next].at : Infinity, batcher.nextDeadline() ?? Infinity);
    if (t > limit) break;
    if (next < inputs.length && inputs[next].at === t) {
      while (next < inputs.length && inputs[next].at === t) {
        const r = inputs[next];
        if (window && r.quality === 'good' && (!windows.length || windows.at(-1)!.to <= t)) windows.push({ from: t, to: t + window });
        batcher.offer({ device: deviceName, point: signals[r.signal].id, value: r.value, ts: BASE + r.ts, quality: r.quality }, t, out);
        next++;
      }
      batcher.endOfPass(out);
      drain(t);
    }
    batcher.due(t, BASE + t, out);
    drain(t);
  }
  batcher.flushAll(out);
  drain(limit);
  const samples: number[] = [];
  for (let t = every; every && t <= durationMs; t += every) samples.push(t);

  const measurements: Measurement[] = published.map(({ message, at }) => {
    const body = JSON.parse(message.payload);
    const mtype = message.topic.split('/').at(-1)!;
    const fragments: Record<string, Record<string, number>> = Object.fromEntries(Object.entries(body).filter(([k]) => k !== 'time')) as any;
    const has = signals.map(() => false);
    for (const [fragment, series] of Object.entries(fragments)) {
      for (const s of Object.keys(series)) {
        const i = signalOfSeries(names, mtype, fragment, s);
        if (i >= 0) has[i] = true;
      }
    }
    return { time: Date.parse(body.time) - BASE, publishedAt: at, type: mtype, fragments, has, from: [] };
  });

  // Trace each input to its measurement.
  if (policy.kind === 'sample') {
    const aggregated = mergedOf(points[0]).aggregate !== undefined;
    const bySignal = signals.map((_, s) => inputs.map((x, i) => (x.signal === s ? i : -1)).filter((i) => i >= 0));
    const lastAt = (s: number, time: number) => {
      const list = bySignal[s];
      let found = -1;
      for (const i of list) {
        if (inputs[i].at > time) break;
        found = i;
      }
      return found;
    };
    let prev = 0;
    for (const m of measurements) {
      signals.forEach((_, s) => {
        if (!m.has[s]) return;
        if (!aggregated) {
          const i = lastAt(s, m.time);
          if (i >= 0) m.from.push(i);
          return;
        }
        const held = lastAt(s, prev);
        for (const i of bySignal[s]) {
          const x = inputs[i];
          if (x.quality === 'good' && ((x.at > prev && x.at <= m.time) || i === held)) m.from.push(i);
        }
      });
      prev = m.time;
    }
  } else {
    // Under time and frame a signal's good values reach the measurements in the order they
    // were published, at most one per measurement.
    const queues = signals.map((_, s) => inputs.map((x, i) => (x.signal === s && x.quality === 'good' ? i : -1)).filter((i) => i >= 0));
    const heads = signals.map(() => 0);
    for (const m of measurements) {
      signals.forEach((_, s) => {
        if (m.has[s]) m.from.push(queues[s][heads[s]++]);
      });
    }
  }
  const used = new Set(measurements.flatMap((m) => m.from));
  const dropped = inputs.map((x, i) => (x.quality === 'good' && !used.has(i) ? i : -1)).filter((i) => i >= 0);
  const missing = measurements.reduce((n, m) => n + m.has.filter((h) => !h).length, 0);
  let maxShift = 0;
  if (policy.kind !== 'sample') {
    for (const m of measurements) for (const i of m.from) maxShift = Math.max(maxShift, Math.abs(inputs[i].ts - m.time));
  }
  const topic = `te/device/${deviceName}///m/${type}`;
  return { signals, reports, inputs, measurements, windows, samples, dropped, missing, maxShift, units, names, device: deviceName, topic, type };
}

/** The measurement as the connector publishes it on its topic. */
export function tedgeMeasurement(m: Measurement) {
  return { time: isoAt(m.time), ...m.fragments };
}

/** The measurement as the thin-edge.io Cumulocity mapper sends it (units from the meta topic). */
export function c8yMeasurement(m: Measurement, run: Pick<GroupRun, 'units'>) {
  const { units } = run;
  const out: Record<string, any> = { type: m.type, time: isoAt(m.time) };
  for (const [fragment, series] of Object.entries(m.fragments)) {
    out[fragment] = Object.fromEntries(
      Object.entries(series).map(([s, value]) => {
        const unit = units[`${fragment}.${s}`];
        return [s, unit ? { value, unit } : { value }];
      }),
    );
  }
  return out;
}

/** How many fragments the measurements of a run hold: a Streaming Lake Ingestion write each. */
export const fragmentWrites = (run: Pick<GroupRun, 'measurements'>) =>
  run.measurements.reduce((n, m) => n + Object.keys(m.fragments).length, 0);

/** Example devices for the grouping playground: they replace the signals. */
export const EXAMPLES: { label: string; protocol: 'modbus' | 'opcua'; durationMs: number; signals: SignalSpec[] }[] = [
  {
    label: 'Three polled points, filtered',
    protocol: 'modbus',
    durationMs: 120_000,
    signals: [
      { id: 'temperature', shape: 'noisy', source: 'polled', interval: '1s', outage: 'none', report: { deadband: 0.3 } },
      { id: 'setpoint', shape: 'step', source: 'polled', interval: '1s', outage: 'none', report: { on_change: true } },
      { id: 'outdoor', shape: 'sine', source: 'polled', interval: '5s', outage: 'none', report: {} },
    ],
  },
  {
    label: 'Pushed points, times ms apart',
    protocol: 'opcua',
    durationMs: 60_000,
    signals: [
      { id: 'temperature', shape: 'noisy', source: 'pushed', interval: '500ms', outage: 'none', report: {} },
      { id: 'outdoor', shape: 'sine', source: 'pushed', interval: '1s', outage: 'none', report: {} },
      { id: 'count', shape: 'counter', source: 'pushed', interval: '500ms', outage: 'none', report: {} },
    ],
  },
  {
    label: 'A signal drops out',
    protocol: 'modbus',
    durationMs: 120_000,
    signals: [
      { id: 'temperature', shape: 'noisy', source: 'polled', interval: '1s', outage: 'none', report: {} },
      { id: 'outlet', shape: 'noisy', source: 'polled', interval: '1s', outage: 'long', report: {} },
    ],
  },
  {
    label: 'A spike between samples',
    protocol: 'opcua',
    durationMs: 120_000,
    signals: [
      { id: 'pressure', shape: 'spike', source: 'pushed', interval: '100ms', outage: 'none', report: {} },
      { id: 'temperature', shape: 'noisy', source: 'polled', interval: '2s', outage: 'none', report: { deadband: 0.3 } },
    ],
  },
];
