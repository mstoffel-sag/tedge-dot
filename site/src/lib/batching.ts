// Simulation behind the batching playground: take what each signal's reporting policy published
// (src/lib/playground.ts), feed it to the measurement-output batcher (src/lib/batch.ts) with the
// connector configuration, and trace which reading went into which measurement and which signals
// each measurement is missing.

import { Batcher, groupByOf, mergeBatchLevel, resolveName, type Message } from './batch';
import { parseDuration } from './report';
import { simulateSignal, type Run, type SignalSpec } from './playground';

/** Every `batch.group_by`, as the playground offers them (a test holds them to the schema). */
export const POLICIES: { id: 'time' | 'frame' | 'sample'; text: string }[] = [
  { id: 'time', text: 'Readings with the same timestamp share a measurement. Nothing is lost, no time is moved. A signal that published nothing at that time is not in the measurement.' },
  { id: 'frame', text: 'Readings within a window share a measurement stamped with its first time. Nothing is lost; a time moves by less than the window. A signal that published nothing in the window is not in it.' },
  { id: 'sample', text: 'Every period, the value each signal holds, stamped with the sample time, whether it changed or not. Readings in between are not sent. A signal with no good value (never read, or in an outage) is left out.' },
];

/** The `batch.aggregate` choices under "sample" (a test checks they cover every aggregate). */
export const AGGREGATES: { id: string; label: string; value: string | string[] | null }[] = [
  { id: 'none', label: 'none (sample and hold)', value: null },
  { id: 'mean', label: '"mean"', value: 'mean' },
  { id: 'mean_min_max', label: '["mean", "min", "max"]', value: ['mean', 'min', 'max'] },
  { id: 'all', label: '["mean", "min", "max", "last"]', value: ['mean', 'min', 'max', 'last'] },
];

export const WINDOWS = ['100ms', '250ms', '500ms', '1s', '2s', '5s'];
export const EVERY = ['500ms', '1s', '5s', '10s', '30s', '1m'];

export interface BatchForm {
  group_by: (typeof POLICIES)[number]['id'];
  window: string;
  every: string;
  /** An `AGGREGATES` id. */
  aggregate: string;
}

export function batchTable(form: BatchForm): Record<string, unknown> {
  if (form.group_by === 'frame') return { group_by: 'frame', window: form.window };
  if (form.group_by === 'sample') {
    const aggregate = AGGREGATES.find((a) => a.id === form.aggregate)?.value ?? null;
    return aggregate === null ? { group_by: 'sample', every: form.every } : { aggregate, every: form.every };
  }
  return { group_by: 'time' };
}

export function batchToml(form: BatchForm): string {
  const t = batchTable(form);
  const parts = Object.entries(t).map(([k, v]) => `${k} = ${Array.isArray(v) ? `[${v.map((x) => JSON.stringify(x)).join(', ')}]` : JSON.stringify(v)}`);
  return `batch = { ${parts.join(', ')} }`;
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
  /** Series name → value. */
  series: Record<string, number>;
  /** For each signal: whether it has a series in this measurement. */
  has: boolean[];
  /** Indexes into the inputs that went into it (for an aggregate: every input of the period). */
  from: number[];
}

export interface BatchRun {
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
  units: Record<string, string>;
  /** The device the measurements are for, and their topic and group. */
  device: string;
  topic: string;
  group: string;
}

const BASE = Date.parse('2026-01-01T08:00:00.000Z');
export const isoAt = (t: number) => new Date(BASE + t).toISOString();

/** The signal a series belongs to: its series name itself, or `<series>_<aggregate>`. */
function signalOfSeries(names: string[], series: string): number {
  return names.findIndex((n) => series === n || series.startsWith(`${n}_`));
}

/**
 * Run every signal through its reporting policy, then through the batcher configured by
 * `config` (a connector configuration as TOML parses it). The signals are the points of its
 * first device, by id.
 */
export function simulate(signals: SignalSpec[], durationMs: number, config: Record<string, any>, reports?: Run[]): BatchRun {
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
  const naming = { group: m.group ?? '', series: m.series ?? '', separator: m.point_separator ?? '' };
  const names = points.map((p) => resolveName(naming, connector.protocol ?? '', p));
  const group = names[0]?.[0] ?? connector.protocol ?? '';
  // The playground's chart marks follow the first point's batch policy.
  const policy = groupByOf([connector.batch, device.batch, points[0]?.batch].reduce(mergeBatchLevel, {})) ?? { kind: 'time' };

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
        units = Object.fromEntries(Object.entries(JSON.parse(message.payload)).map(([k, v]: [string, any]) => [k.split('.').slice(1).join('.'), v.unit]));
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
    const series: Record<string, number> = Object.assign({}, ...Object.entries(body).filter(([k]) => k !== 'time').map(([, v]) => v));
    const has = signals.map(() => false);
    for (const s of Object.keys(series)) {
      const i = signalOfSeries(names.map((n) => n[1]), s);
      if (i >= 0) has[i] = true;
    }
    return { time: Date.parse(body.time) - BASE, publishedAt: at, series, has, from: [] };
  });

  // Trace each input to its measurement.
  if (policy.kind === 'sample') {
    const merged = [connector.batch, device.batch, points[0]?.batch].reduce(mergeBatchLevel, {});
    const aggregated = merged.aggregate !== undefined;
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
  const topic = `te/device/${deviceName}///m/${group}`;
  return { signals, reports, inputs, measurements, windows, samples, dropped, missing, maxShift, units, device: deviceName, topic, group };
}

/** The measurement as the thin-edge.io Cumulocity mapper sends it (units from the meta topic). */
export function c8yMeasurement(m: Measurement, run: Pick<BatchRun, 'units' | 'group'>) {
  const { units, group } = run;
  return {
    type: group,
    time: isoAt(m.time),
    [group]: Object.fromEntries(
      Object.entries(m.series).map(([s, value]) => [s, units[s] ? { value, unit: units[s] } : { value }]),
    ),
  };
}

/** Example devices for the batching playground: they replace the signals. */
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
