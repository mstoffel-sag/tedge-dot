// Simulation behind the playgrounds: generate a point's readings (polled, or pushed by the
// source on change), run each through the SDK's reporting policy (src/lib/report.ts), and keep
// what it publishes. The batching playground (src/lib/batching.ts) takes those samples from here.

import { changed, parseDuration, policyFromTable, ReportState, type Obs, type Quality, type ReportTable } from './report';

export type SignalId = 'noisy' | 'sine' | 'step' | 'flat' | 'flapping' | 'spike' | 'counter';

export const SIGNALS: { id: SignalId; label: string; unit: string; name: string }[] = [
  { id: 'noisy', label: 'Noisy temperature', unit: '°C', name: 'temperature' },
  { id: 'sine', label: 'Slow sine', unit: '°C', name: 'outdoor' },
  { id: 'step', label: 'Setpoint steps', unit: '°C', name: 'setpoint' },
  { id: 'flat', label: 'Flat (idle sensor)', unit: '°C', name: 'ambient' },
  { id: 'flapping', label: 'Flapping switch', unit: '', name: 'switch' },
  { id: 'spike', label: 'Steady, with short spikes', unit: 'bar', name: 'pressure' },
  { id: 'counter', label: 'Counter', unit: 'pcs', name: 'count' },
];

export type Source = 'polled' | 'pushed';
export type Outage = 'none' | 'short' | 'long';

export const INTERVALS = ['100ms', '250ms', '500ms', '1s', '2s', '5s', '10s'];
export const DURATIONS: { label: string; ms: number }[] = [
  { label: '1 minute', ms: 60_000 },
  { label: '2 minutes', ms: 120_000 },
  { label: '10 minutes', ms: 600_000 },
];
export const OUTAGES: { id: Outage; label: string }[] = [
  { id: 'none', label: 'none' },
  { id: 'short', label: 'short (5 % of the run)' },
  { id: 'long', label: 'long (25 % of the run)' },
];

/** One point of the playground's device. */
export interface SignalSpec {
  /** The point id, which is also its series name. */
  id: string;
  shape: SignalId;
  source: Source;
  /** `poll_interval` of a polled point, `sampling_interval` of a pushed one. */
  interval: string;
  /** A period in which every read fails (quality "bad"). */
  outage: Outage;
  report: ReportTable;
}

/** A small deterministic PRNG, so a preset always draws the same signal. */
export function mulberry32(seed: number) {
  return () => {
    seed |= 0;
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** The signal's value at `t` ms, for a run of `duration` ms. */
export function signalAt(id: SignalId, t: number, duration: number, rand: () => number): number {
  const s = t / 1000;
  const f = t / duration;
  const round = (v: number, places = 2) => Math.round(v * 10 ** places) / 10 ** places;
  switch (id) {
    case 'sine':
      return round(21 + 4 * Math.sin((2 * Math.PI * t) / (duration / 2.5)));
    case 'step':
      return f < 0.3 ? 20 : f < 0.65 ? 24 : 22;
    case 'noisy': {
      // Gaussian-ish noise (sum of uniforms) on a slow drift.
      const noise = (rand() + rand() + rand() - 1.5) * 0.4;
      return round(20 + s / 200 + noise);
    }
    case 'flat':
      return 21.5;
    case 'flapping': {
      // A switch that turns on twice, bouncing for a few seconds each time.
      const edges = [0.25, 0.45, 0.65, 0.85].map((e) => e * duration);
      const state = edges.filter((e) => t >= e).length % 2;
      const near = edges.some((e) => t >= e && t - e < 4000);
      return near && rand() < 0.5 ? 1 - state : state;
    }
    case 'spike':
      return (f >= 0.3 && f < 0.31) || (f >= 0.72 && f < 0.725) ? 5.2 : 2;
    case 'counter':
      return Math.floor(s / 7);
  }
}

/** The outage of a run, in ms, or null. */
export function outageWindow(outage: Outage, duration: number): { from: number; to: number } | null {
  if (outage === 'short') return { from: duration * 0.45, to: duration * 0.5 };
  if (outage === 'long') return { from: duration * 0.4, to: duration * 0.65 };
  return null;
}

export interface Reading {
  /** The reading's (source) time, ms from the start. */
  t: number;
  value: number;
  quality: Quality;
}

export interface Published {
  /** The reading's index (and so its timestamp). */
  index: number;
  /** When it was published: later than the reading when it was held back. */
  at: number;
  /** Published although unchanged, because the heartbeat (`max_interval`) was due. */
  heartbeat: boolean;
}

export interface Run {
  readings: Reading[];
  published: Published[];
}

/** A pushed point's source times are a few ms off the sampling grid, differently per point. */
export function jitterOf(id: string): number {
  let h = 0;
  for (const c of id) h = (h * 31 + c.charCodeAt(0)) | 0;
  return 1 + (Math.abs(h) % 9);
}

function seedOf(spec: SignalSpec): number {
  return 7 + jitterOf(spec.id) * 101 + SIGNALS.findIndex((s) => s.id === spec.shape);
}

/**
 * Read one point for `durationMs` and run every reading through its reporting policy.
 * A polled point is read every `interval` from 0. A pushed point is sampled by the source every
 * `interval` and delivered only when its value or quality changed (an OPC UA monitored item),
 * with a source time a few ms after the sampling time; its heartbeat is an on-demand read.
 */
export function simulateSignal(spec: SignalSpec, durationMs: number, tickMs = 100): Run {
  const rand = mulberry32(seedOf(spec));
  const interval = parseDuration(spec.interval) ?? 1000;
  const policy = policyFromTable(spec.report);
  const state = new ReportState<number>(policy, 0);
  const pushed = spec.source === 'pushed';
  const outage = outageWindow(spec.outage, durationMs);
  const jitter = pushed ? jitterOf(spec.id) : 0;
  const readings: Reading[] = [];
  const published: Published[] = [];
  let last: { obs: Obs; at: number } | null = null;
  let delivered: { value: number; quality: Quality } | null = null;

  const obsOf = (r: Reading): Obs => (r.quality === 'good' ? { kind: 'number', value: r.value } : { kind: 'other', value: r.quality });
  const record = (index: number, at: number, viaOffer: boolean) => {
    const obs = obsOf(readings[index]);
    const heartbeat =
      viaOffer && last !== null && policy.maxInterval !== null && at - last.at >= policy.maxInterval && !changed(policy, obs, last.obs);
    published.push({ index, at, heartbeat });
    last = { obs, at };
  };
  const read = (t: number): Reading => {
    const value = signalAt(spec.shape, t, durationMs, rand);
    const bad = outage !== null && t >= outage.from && t < outage.to;
    return { t: t + jitter, value, quality: bad ? 'bad' : 'good' };
  };
  const offer = (r: Reading) => {
    readings.push(r);
    const index = readings.length - 1;
    const out = state.offer(index, obsOf(r), r.quality, r.t);
    if (out !== null) record(out, r.t, true);
  };

  // Events in time order: sampling times and report ticks.
  const step = Math.min(tickMs, interval);
  for (let t = 0; t <= durationMs; t += step) {
    if (t % interval === 0) {
      const r = read(t);
      if (!pushed) offer(r);
      else if (!delivered || delivered.quality !== r.quality || (r.quality === 'good' && delivered.value !== r.value)) {
        delivered = { value: r.value, quality: r.quality };
        offer(r);
      }
    }
    const now = t + jitter;
    const due = state.due(now, pushed);
    if (due.publish !== null) record(due.publish, now, false);
    if (due.read) offer(read(t));
  }
  return { readings, published };
}

/** One polled point read every `pollMs`, through the policy (the single-signal form). */
export function simulate(
  signal: SignalId,
  table: ReportTable,
  { durationMs = 600_000, pollMs = 1000, tickMs = 100 } = {},
): Run {
  return simulateSignal({ id: 'p', shape: signal, source: 'polled', interval: `${pollMs}ms`, outage: 'none', report: table }, durationMs, tickMs);
}

/** The `report` keys after `on_change`, as the config builder offers them (a test holds them to the schema). */
export const REPORT_FIELDS = [
  { key: 'deadband', hint: 'A number, or a percentage like "2%"' },
  { key: 'min_interval', hint: 'At most once per interval, e.g. "30s"' },
  { key: 'max_interval', hint: 'Heartbeat, e.g. "30m"' },
  { key: 'debounce', hint: 'Publish once settled, e.g. "5s"' },
] as const;

/** The `report` table as a line of TOML, with only the keys that are set. */
export function tableToToml(table: ReportTable): string {
  const parts: string[] = [];
  if (table.on_change) parts.push('on_change = true');
  if (table.deadband !== undefined && table.deadband !== '') {
    parts.push(`deadband = ${typeof table.deadband === 'number' ? table.deadband : JSON.stringify(table.deadband)}`);
  }
  for (const key of ['min_interval', 'max_interval', 'debounce'] as const) {
    if (table[key]) parts.push(`${key} = ${JSON.stringify(table[key])}`);
  }
  return `report = { ${parts.join(', ')} }`.replace('{  }', '{}');
}
