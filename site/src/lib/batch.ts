// The measurement-output batcher (contract §5.5), ported from the Rust SDK
// (impl/rust/crates/sdk/src/measure.rs, `Batcher`) for the batching playground. The docs build
// runs it against every case in doc/contract/test-vectors/measurements/vectors.json, the same
// vectors the Rust and C SDKs run (tests/batch.test.ts).
//
// Times are milliseconds: `now` on the device clock, `ts` and sample times in Unix time.

import { parseDuration } from './report';

export type Quality = 'good' | 'bad' | 'stale';

export const SIZE_BUDGET = 15_000;
export const SERIES_OVERHEAD = 12;
const FRAME_BYTES = 9 + 24 + 2 + 2 + 2;
const MAX_LAYERS = 64;

export type GroupBy = { kind: 'time' } | { kind: 'frame'; window: number } | { kind: 'sample'; every: number };
export type Aggregate = 'mean' | 'min' | 'max' | 'last';
const AGGREGATES: Aggregate[] = ['mean', 'min', 'max', 'last'];

export interface Message {
  topic: string;
  payload: string;
  retain: boolean;
}

export interface Routing {
  sample: boolean;
  measured: boolean;
}

export interface Reading {
  device: string;
  point: string;
  /** A number, a boolean or a string; `undefined` for a raw point. */
  value: number | boolean | string | undefined;
  /** Unix milliseconds. */
  ts: number;
  quality: Quality;
}

type Table = Record<string, any>;

/** `group_by` of a merged `batch` table; `null` when the duration it needs is missing. */
export function groupByOf(table: Table): GroupBy | null {
  const duration = (key: string) => (typeof table[key] === 'string' ? parseDuration(table[key]) : null);
  switch (table.group_by ?? 'time') {
    case 'frame': {
      const window = duration('window');
      return window === null ? null : { kind: 'frame', window };
    }
    case 'sample': {
      const every = duration('every');
      return every === null ? null : { kind: 'sample', every };
    }
    default:
      return { kind: 'time' };
  }
}

/** The aggregates of a merged `batch` table and whether they are suffixed to the series name. */
export function aggregatesOf(table: Table): { aggregates: Aggregate[]; suffixed: boolean } | null {
  const a = table.aggregate;
  if (typeof a === 'string') return AGGREGATES.includes(a as Aggregate) ? { aggregates: [a as Aggregate], suffixed: false } : null;
  if (Array.isArray(a)) return a.every((x) => AGGREGATES.includes(x)) ? { aggregates: a, suffixed: true } : null;
  return null;
}

/** Merge one level's `batch` table over `base`; `aggregate` without `group_by` chooses "sample". */
export function mergeBatchLevel(base: Table, level: unknown): Table {
  if (!level || typeof level !== 'object' || Array.isArray(level)) return base;
  const out = { ...base, ...(level as Table) };
  if ('aggregate' in level && !('group_by' in level)) out.group_by = 'sample';
  return out;
}

const sameGroupBy = (a: GroupBy, b: GroupBy) =>
  a.kind === b.kind && (a as any).window === (b as any).window && (a as any).every === (b as any).every;
const keyOf = (topic: number, g: GroupBy) => `${topic}|${g.kind}|${(g as any).window ?? (g as any).every ?? ''}`;

/** `n` as JSON: an integer without a fraction, else the shortest round-trip form laid out as Rust's `ryu`. */
export function formatNumber(n: number): string {
  if (Number.isInteger(n) && Math.abs(n) < 1e15) return String(n);
  const [mantissa, exp] = Math.abs(n).toExponential().split('e');
  const digits = mantissa.replace('.', '');
  const e = Number(exp);
  const kk = e + 1;
  const sign = n < 0 ? '-' : '';
  let text: string;
  if (digits.length <= kk && kk <= 16) text = `${digits}${'0'.repeat(kk - digits.length)}.0`;
  else if (kk > 0 && kk <= 16) text = `${digits.slice(0, kk)}.${digits.slice(kk)}`;
  else if (kk > -5 && kk <= 0) text = `0.${'0'.repeat(-kk)}${digits}`;
  else text = `${digits[0]}${digits.length > 1 ? `.${digits.slice(1)}` : ''}e${e}`;
  return sign + text;
}

const bytes = (s: string) => new TextEncoder().encode(s).length;
const iso = (ms: number) => new Date(ms).toISOString();

function body(groupJson: string, time: number, series: string): string {
  return `{"time":"${iso(time)}",${groupJson}:{${series}}}`;
}

interface PointPlan {
  topic: number;
  seriesKey: string;
  measurable: boolean;
  sample: boolean;
  groupBy: GroupBy;
  aggregates: { aggregate: Aggregate; key: string }[];
}

interface Topic {
  topic: string;
  groupJson: string;
  units: string;
  unitsSettled: boolean;
}

interface Open {
  time: number;
  opened: number;
  body: string;
  n: number;
  points: number[];
}

class Slot {
  value: number | null = null;
  since: number;
  sum = 0;
  goodNs = 0;
  range: [number, number] | null = null;
  constructor(now: number) {
    this.since = now;
  }
  close(now: number) {
    if (this.value !== null) {
      const ns = Math.max(0, now - this.since) * 1e6;
      this.sum += this.value * ns;
      this.goodNs += ns;
    }
    this.since = now;
  }
  hold(value: number | null, now: number) {
    this.close(now);
    this.value = value;
    if (value !== null) this.range = this.range ? [Math.min(this.range[0], value), Math.max(this.range[1], value)] : [value, value];
  }
  restart(now: number) {
    this.since = now;
    this.sum = 0;
    this.goodNs = 0;
    this.range = this.value === null ? null : [this.value, this.value];
  }
}

interface DeviceTable {
  points: PointPlan[];
  index: Map<string, number>;
  topics: Topic[];
  depth: number[];
  open: Map<string, { topic: number; groupBy: GroupBy; layers: Open[] }>;
  lastLayerMs: Map<string, number>;
  slots: Slot[];
  samplers: { every: number; next: number }[];
  stale: boolean;
}

/** Whether a flow reads this point's samples (or `sample` says so). */
export function publishesSample(point: Table): boolean {
  if (typeof point.sample === 'boolean') return point.sample;
  const flagged = (key: string) => point.meta?.[key] !== undefined && point.meta[key] !== null && point.meta[key] !== false;
  return flagged('alarm') || flagged('event') || flagged('parameter') || ['write', 'read_write'].includes(point.access);
}

/**
 * The point tables of a connector's devices in measurement output, and the messages they
 * produce. `config` is the parsed TOML of a connector configuration.
 */
export class Batcher {
  private devices: DeviceTable[] = [];
  private byName = new Map<string, number>();
  private includeBoolean: boolean;
  private publishedUnits = new Map<string, string>();

  constructor(config: Table, now: number) {
    const connector = config.connector ?? {};
    const protocol: string = connector.protocol ?? '';
    const m = connector.measurement ?? {};
    const text = (v: unknown) => (typeof v === 'string' ? v : '');
    const naming = { group: text(m.group), series: text(m.series), separator: text(m.point_separator) };
    this.includeBoolean = typeof m.include_boolean === 'boolean' ? m.include_boolean : true;

    for (const device of (config.device ?? []) as Table[]) {
      const topics: Topic[] = [];
      const units: Map<string, string>[] = [];
      const points: PointPlan[] = [];
      const index = new Map<string, number>();
      const samplers: { every: number; next: number }[] = [];
      for (const point of (device.point ?? []) as Table[]) {
        const [group, series] = resolveName(naming, protocol, point);
        const groupJson = JSON.stringify(group);
        let topic = topics.findIndex((t) => t.groupJson === groupJson);
        if (topic < 0) {
          topics.push({ topic: `te/device/${device.name}///m/${group}`, groupJson, units: '', unitsSettled: false });
          units.push(new Map());
          topic = topics.length - 1;
        }
        const mode = point.mode ?? device.default_mode ?? 'typed';
        const measurable = point.meta?.measurement !== false && mode === 'typed';
        let table: Table = {};
        for (const level of [connector.batch, device.batch, point.batch]) table = mergeBatchLevel(table, level);
        let groupBy = groupByOf(table) ?? { kind: 'time' };
        if (groupBy.kind === 'sample' && point.meta?.event?.every === true) groupBy = { kind: 'time' };
        const aggs = groupBy.kind === 'sample' ? aggregatesOf(table) : null;
        const names: { aggregate: Aggregate | null; name: string }[] = aggs
          ? aggs.aggregates.map((a) => ({ aggregate: a, name: aggs.suffixed ? `${series}_${a}` : series }))
          : [{ aggregate: null, name: series }];
        if (measurable && typeof point.unit === 'string' && point.unit) {
          for (const { name } of names) units[topic].set(`${group}.${name}`, point.unit);
        }
        if (groupBy.kind === 'sample') {
          const every = groupBy.every;
          if (!samplers.some((s) => s.every === every)) samplers.push({ every, next: now + every });
        }
        index.set(point.id, points.length);
        points.push({
          topic,
          seriesKey: `${JSON.stringify(series)}:`,
          measurable,
          sample: publishesSample(point),
          groupBy,
          aggregates: names
            .filter((n) => n.aggregate !== null)
            .map((n) => ({ aggregate: n.aggregate!, key: `${JSON.stringify(n.name)}:` })),
        });
      }
      topics.forEach((t, i) => {
        if (units[i].size) {
          // Sorted, as the Rust SDK's BTreeMap orders them.
          const keys = [...units[i].keys()].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
          t.units = JSON.stringify(Object.fromEntries(keys.map((k) => [k, { unit: units[i].get(k) }])));
        }
      });
      this.byName.set(device.name, this.devices.length);
      this.devices.push({
        points,
        index,
        topics,
        depth: points.map(() => 0),
        open: new Map(),
        lastLayerMs: new Map(),
        slots: points.map(() => new Slot(now)),
        samplers,
        stale: false,
      });
    }
  }

  private seriesValue(plan: PointPlan, r: Reading): number | null {
    if (!plan.measurable || r.quality !== 'good' || r.value === undefined) return null;
    if (typeof r.value === 'number') return Number.isFinite(r.value) ? r.value : null;
    if (typeof r.value === 'boolean') return this.includeBoolean ? (r.value ? 1 : 0) : null;
    return null;
  }

  /** Take one reading the report policy let through; closed messages are appended to `out`. */
  offer(r: Reading, now: number, out: Message[]): Routing {
    const d = this.byName.get(r.device);
    const k = d === undefined ? undefined : this.devices[d].index.get(r.point);
    if (d === undefined || k === undefined) return { sample: true, measured: false };
    const plan = this.devices[d].points[k];
    const value = this.seriesValue(plan, r);
    if (value === null) {
      if (r.quality !== 'good') this.devices[d].slots[k].hold(null, now);
      return { sample: plan.sample, measured: false };
    }
    if (plan.groupBy.kind === 'sample') {
      if (plan.aggregates.length) this.devices[d].slots[k].hold(value, now);
      else this.devices[d].slots[k].value = value;
    } else {
      this.add(d, k, r.ts, value, now, out);
    }
    return { sample: plan.sample, measured: true };
  }

  private add(d: number, k: number, ts: number, value: number, now: number, out: Message[]) {
    const dev = this.devices[d];
    const plan = dev.points[k];
    const key = keyOf(plan.topic, plan.groupBy);
    const depth = dev.depth[k];
    let opened = now;
    const existing = dev.open.get(key);
    if (existing) {
      const first = existing.layers[0];
      let close: boolean;
      if (plan.groupBy.kind === 'frame') {
        if (first.opened + plan.groupBy.window <= now) close = true;
        else if (depth >= MAX_LAYERS) {
          opened = first.opened;
          close = true;
        } else close = false;
      } else close = depth > 0 || first.time !== ts;
      if (close) this.close(d, key, out);
    }
    const layer = dev.depth[k];
    let entry = dev.open.get(key);
    if (!entry) {
      entry = { topic: plan.topic, groupBy: plan.groupBy, layers: [] };
      dev.open.set(key, entry);
    }
    const layers = entry.layers;
    if (layer === layers.length) {
      let time = ts;
      if (plan.groupBy.kind === 'frame') {
        // Layer times strictly increase per key, so a point's values stay in order.
        const last = dev.lastLayerMs.get(key);
        if (last !== undefined && ts <= last) time = last + 1;
        dev.lastLayerMs.set(key, time);
      }
      layers.push({ time, opened: layers[0]?.opened ?? opened, body: '', n: 0, points: [] });
    }
    const topic = dev.topics[plan.topic];
    const piece = plan.seriesKey + formatNumber(value);
    const open = layers[layer];
    const estimate = (b: number, n: number) => FRAME_BYTES + bytes(topic.groupJson) + b + SERIES_OVERHEAD * n;
    if (open.n > 0 && estimate(bytes(open.body) + 1 + bytes(piece), open.n + 1) > SIZE_BUDGET) {
      // Split: the full part goes now; the rest keeps the message's time and window.
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: body(topic.groupJson, open.time, open.body), retain: false });
      open.body = '';
      open.n = 0;
    }
    open.body += (open.body ? ',' : '') + piece;
    open.n += 1;
    open.points.push(k);
    dev.depth[k] += 1;
  }

  private close(d: number, key: string, out: Message[]) {
    const dev = this.devices[d];
    const entry = dev.open.get(key);
    if (!entry) return;
    dev.open.delete(key);
    const topic = dev.topics[entry.topic];
    for (const open of entry.layers) {
      for (const k of open.points) dev.depth[k] = 0;
      if (open.n === 0) continue;
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: body(topic.groupJson, open.time, open.body), retain: false });
    }
  }

  /** The end of a runtime pass: publish every open `time` message. */
  endOfPass(out: Message[]) {
    this.devices.forEach((dev, d) => {
      const keys = [...dev.open.entries()]
        .filter(([, e]) => e.groupBy.kind === 'time')
        .sort(([, a], [, b]) => a.topic - b.topic)
        .map(([key]) => key);
      for (const key of keys) this.close(d, key, out);
    });
  }

  /** Publish every open message, frames included. */
  flushAll(out: Message[]) {
    this.devices.forEach((dev, d) => {
      const keys = [...dev.open.entries()].sort(([, a], [, b]) => a.topic - b.topic).map(([key]) => key);
      for (const key of keys) this.close(d, key, out);
    });
  }

  /** Publish the frames whose window ended and the samples that are due; `wall` stamps samples. */
  due(now: number, wall: number, out: Message[]) {
    this.devices.forEach((dev, d) => {
      const ended = [...dev.open.entries()]
        .filter(([, e]) => e.groupBy.kind === 'frame' && e.layers[0].opened + e.groupBy.window <= now)
        .sort(([, a], [, b]) => a.layers[0].opened - b.layers[0].opened || a.topic - b.topic)
        .map(([key]) => key);
      for (const key of ended) this.close(d, key, out);
      for (const sampler of dev.samplers) {
        if (sampler.next > now) continue;
        this.sample(d, sampler.every, now, wall, out);
        // On the period's grid; a sampler that fell a whole period behind restarts.
        let next = sampler.next + sampler.every;
        if (next <= now) next = now + sampler.every;
        sampler.next = next;
      }
    });
  }

  private sample(d: number, every: number, now: number, wall: number, out: Message[]) {
    const dev = this.devices[d];
    const bodies = dev.topics.map(() => ({ text: '', n: 0 }));
    dev.points.forEach((plan, k) => {
      if (plan.groupBy.kind !== 'sample' || plan.groupBy.every !== every) return;
      const series: [string, number][] = [];
      const slot = dev.slots[k];
      if (!plan.aggregates.length) {
        if (slot.value === null || dev.stale) return;
        series.push([plan.seriesKey, slot.value]);
      } else {
        slot.close(now);
        if (slot.goodNs > 0) {
          for (const { aggregate, key } of plan.aggregates) {
            const value =
              aggregate === 'mean' ? slot.sum / slot.goodNs
              : aggregate === 'min' ? slot.range?.[0]
              : aggregate === 'max' ? slot.range?.[1]
              : slot.value;
            if (value !== undefined && value !== null) series.push([key, value]);
          }
        }
        slot.restart(now);
      }
      for (const [key, value] of series) {
        const piece = key + formatNumber(value);
        const topic = dev.topics[plan.topic];
        const b = bodies[plan.topic];
        const size = FRAME_BYTES + bytes(topic.groupJson) + bytes(b.text) + 1 + bytes(piece) + SERIES_OVERHEAD * (b.n + 1);
        if (b.n > 0 && size > SIZE_BUDGET) {
          this.settleUnits(topic, out);
          out.push({ topic: topic.topic, payload: body(topic.groupJson, wall, b.text), retain: false });
          b.text = '';
          b.n = 0;
        }
        b.text += (b.text ? ',' : '') + piece;
        b.n += 1;
      }
    });
    bodies.forEach((b, t) => {
      if (!b.n) return;
      const topic = dev.topics[t];
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: body(topic.groupJson, wall, b.text), retain: false });
    });
  }

  /** The earliest device-clock time `due` has something to do, if any. */
  nextDeadline(): number | null {
    let min: number | null = null;
    for (const dev of this.devices) {
      for (const e of dev.open.values()) {
        if (e.groupBy.kind === 'frame') min = Math.min(min ?? Infinity, e.layers[0].opened + e.groupBy.window);
      }
      for (const s of dev.samplers) min = Math.min(min ?? Infinity, s.next);
    }
    return min;
  }

  /** Mark a device stale (its link is down) or healthy again. */
  setStale(device: string, stale: boolean, now: number) {
    const d = this.byName.get(device);
    if (d === undefined) return;
    const dev = this.devices[d];
    if (stale && !dev.stale) {
      dev.points.forEach((plan, k) => {
        if (plan.aggregates.length) dev.slots[k].hold(null, now);
      });
    }
    dev.stale = stale;
  }

  private settleUnits(topic: Topic, out: Message[]) {
    if (topic.unitsSettled) return;
    topic.unitsSettled = true;
    const meta = `${topic.topic}/meta`;
    const before = this.publishedUnits.get(meta);
    if (before === topic.units || (before === undefined && !topic.units)) return;
    this.publishedUnits.set(meta, topic.units);
    out.push({ topic: meta, payload: topic.units, retain: true });
  }
}

/** A point's group and series, in `ot-measurement`'s order. */
/** A point's measurement group and series name (contract §5.5 naming). */
export function resolveName(naming: { group: string; series: string; separator: string }, protocol: string, point: Table): [string, string] {
  const named = (key: string) => {
    const v = point.meta?.measurement?.[key];
    return typeof v === 'string' && v ? v : null;
  };
  let group = named('group') ?? naming.group;
  let series = named('series') ?? naming.series;
  const sep = naming.separator;
  const id: string = point.id;
  if (sep && (!group || !series)) {
    const at = id.indexOf(sep);
    if (at > 0 && at + sep.length < id.length) {
      if (!group) group = id.slice(0, at);
      if (!series) series = id.slice(at + sep.length);
    }
  }
  return [group || protocol, series || id];
}
