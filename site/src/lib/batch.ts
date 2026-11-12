// The grouped-output engine (contract §5.5), ported from the Rust SDK
// (impl/rust/crates/sdk/src/measure.rs, `Batcher`) for the grouping playground. The docs build
// runs it against every case in doc/contract/test-vectors/measurements/vectors.json, the same
// vectors the Rust and C SDKs run (tests/batch.test.ts).
//
// Times are milliseconds: `now` on the device clock, `ts` and sample times in Unix time.

import { parseDuration } from './report';

export type Quality = 'good' | 'bad' | 'stale';

export const SIZE_BUDGET = 15_000;
export const SERIES_OVERHEAD = 12;
/** `{"time":"2026-01-01T00:00:00.000Z",` + the closing `}`, around the fragments. */
const FRAME_BYTES = 9 + 24 + 2 + 1;
/** `:{` + `}` around one fragment's series, after its quoted name. */
const FRAGMENT_BYTES = 3;
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

/** `by` of a merged `grouping` table; `null` when the duration it needs is missing. */
export function groupByOf(table: Table): GroupBy | null {
  const duration = (key: string) => (typeof table[key] === 'string' ? parseDuration(table[key]) : null);
  switch (table.by ?? 'time') {
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

/** The aggregates of a merged `grouping` table and whether they are suffixed to the series name. */
export function aggregatesOf(table: Table): { aggregates: Aggregate[]; suffixed: boolean } | null {
  const a = table.aggregate;
  if (typeof a === 'string') return AGGREGATES.includes(a as Aggregate) ? { aggregates: [a as Aggregate], suffixed: false } : null;
  if (Array.isArray(a)) return a.every((x) => AGGREGATES.includes(x)) ? { aggregates: a, suffixed: true } : null;
  return null;
}

/** Merge one level's `grouping` table over `base`; `aggregate` without `by` chooses "sample". */
export function mergeGroupingLevel(base: Table, level: unknown): Table {
  if (!level || typeof level !== 'object' || Array.isArray(level)) return base;
  const out = { ...base, ...(level as Table) };
  if ('aggregate' in level && !('by' in level)) out.by = 'sample';
  return out;
}

/**
 * A level's `grouping` table: `grouping`, else the deprecated `batch` rewritten as the loaders
 * rewrite it (`group_by` is `by`). The loaders refuse a level that sets both.
 */
export function groupingOf(level: Table | undefined): Table | undefined {
  if (!level) return undefined;
  if (level.grouping !== undefined) return level.grouping;
  const batch = level.batch;
  if (!batch || typeof batch !== 'object' || Array.isArray(batch)) return batch;
  const { group_by, ...rest } = batch as Table;
  return group_by === undefined ? rest : { ...rest, by: group_by };
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

/** The series of a message not yet published, per fragment of its topic. */
class Body {
  /** Per fragment, `"a":1,"b":2` without the braces; empty when it holds none. */
  parts: string[] = [];
  /** The byte length of the message text with these series; meaningless while `n` is 0. */
  len = 0;
  /** The fragments holding a series. */
  fragments = 0;
  /** The series. */
  n = 0;

  /** The text length after adding `piece` (bytes) to fragment `f`, whose quoted name is `nameLen` bytes. */
  lenWith(f: number, nameLen: number, piece: number): number {
    const base = this.n === 0 ? FRAME_BYTES : this.len;
    if (!this.parts[f]) return base + (this.fragments > 0 ? 1 : 0) + nameLen + FRAGMENT_BYTES + piece;
    return base + 1 + piece;
  }

  /** The estimated Cumulocity size after adding `piece` to fragment `f` (§5.5). */
  estimateWith(f: number, nameLen: number, piece: number): number {
    return this.lenWith(f, nameLen, piece) + SERIES_OVERHEAD * (this.n + 1);
  }

  push(f: number, nameLen: number, piece: string) {
    this.len = this.lenWith(f, nameLen, bytes(piece));
    while (this.parts.length <= f) this.parts.push('');
    if (!this.parts[f]) this.fragments += 1;
    this.parts[f] += (this.parts[f] ? ',' : '') + piece;
    this.n += 1;
  }

  clear() {
    this.parts = this.parts.map(() => '');
    this.len = 0;
    this.fragments = 0;
    this.n = 0;
  }

  /** `{"time":"<time>","<fragment>":{<series>},…}`, the fragments in the topic's order. */
  render(fragments: string[], time: number): string {
    let out = `{"time":"${iso(time)}"`;
    fragments.forEach((name, f) => {
      if (this.parts[f]) out += `,${name}:{${this.parts[f]}}`;
    });
    return `${out}}`;
  }
}

interface PointPlan {
  topic: number;
  /** Index into the topic's `fragments`. */
  fragment: number;
  seriesKey: string;
  measurable: boolean;
  sample: boolean;
  groupBy: GroupBy;
  aggregates: { aggregate: Aggregate; key: string }[];
}

interface Topic {
  topic: string;
  /** The measurement type the topic ends with. */
  type: string;
  /** `"<fragment>"`, escaped and quoted, in the order their first point is configured. */
  fragments: string[];
  units: string;
  unitsSettled: boolean;
}

interface Open {
  time: number;
  opened: number;
  body: Body;
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
 * The point tables of a connector's devices in grouped output, and the messages they
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
    const naming = { connector: levelNaming(m), separator: typeof m.point_separator === 'string' ? m.point_separator : '' };
    this.includeBoolean = typeof m.include_boolean === 'boolean' ? m.include_boolean : true;

    for (const device of (config.device ?? []) as Table[]) {
      const topics: Topic[] = [];
      const units: Map<string, string>[] = [];
      const points: PointPlan[] = [];
      const index = new Map<string, number>();
      const samplers: { every: number; next: number }[] = [];
      for (const point of (device.point ?? []) as Table[]) {
        const { type, fragment, series } = resolveName(naming, protocol, device, point);
        let topic = topics.findIndex((t) => t.type === type);
        if (topic < 0) {
          topics.push({ topic: `te/device/${device.name}///m/${type}`, type, fragments: [], units: '', unitsSettled: false });
          units.push(new Map());
          topic = topics.length - 1;
        }
        const fragmentJson = JSON.stringify(fragment);
        let fragmentIndex = topics[topic].fragments.indexOf(fragmentJson);
        if (fragmentIndex < 0) fragmentIndex = topics[topic].fragments.push(fragmentJson) - 1;
        const mode = point.mode ?? device.default_mode ?? 'typed';
        const measurable = point.meta?.measurement !== false && mode === 'typed';
        let table: Table = {};
        for (const level of [connector, device, point]) table = mergeGroupingLevel(table, groupingOf(level));
        let groupBy = groupByOf(table) ?? { kind: 'time' };
        if (groupBy.kind === 'sample' && point.meta?.event?.every === true) groupBy = { kind: 'time' };
        const aggs = groupBy.kind === 'sample' ? aggregatesOf(table) : null;
        const names: { aggregate: Aggregate | null; name: string }[] = aggs
          ? aggs.aggregates.map((a) => ({ aggregate: a, name: aggs.suffixed ? `${series}_${a}` : series }))
          : [{ aggregate: null, name: series }];
        if (measurable && typeof point.unit === 'string' && point.unit) {
          for (const { name } of names) units[topic].set(`${fragment}.${name}`, point.unit);
        }
        if (groupBy.kind === 'sample') {
          const every = groupBy.every;
          if (!samplers.some((s) => s.every === every)) samplers.push({ every, next: now + every });
        }
        index.set(point.id, points.length);
        points.push({
          topic,
          fragment: fragmentIndex,
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
      layers.push({ time, opened: layers[0]?.opened ?? opened, body: new Body(), points: [] });
    }
    const topic = dev.topics[plan.topic];
    const f = plan.fragment;
    const nameLen = bytes(topic.fragments[f]);
    const piece = plan.seriesKey + formatNumber(value);
    const open = layers[layer];
    if (open.body.n > 0 && open.body.estimateWith(f, nameLen, bytes(piece)) > SIZE_BUDGET) {
      // Split: the full part goes now; the rest keeps the message's time and window.
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: open.body.render(topic.fragments, open.time), retain: false });
      open.body.clear();
    }
    open.body.push(f, nameLen, piece);
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
      if (open.body.n === 0) continue;
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: open.body.render(topic.fragments, open.time), retain: false });
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
    const bodies = dev.topics.map(() => new Body());
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
        const nameLen = bytes(topic.fragments[plan.fragment]);
        if (b.n > 0 && b.estimateWith(plan.fragment, nameLen, bytes(piece)) > SIZE_BUDGET) {
          this.settleUnits(topic, out);
          out.push({ topic: topic.topic, payload: b.render(topic.fragments, wall), retain: false });
          b.clear();
        }
        b.push(plan.fragment, nameLen, piece);
      }
    });
    bodies.forEach((b, t) => {
      if (!b.n) return;
      const topic = dev.topics[t];
      this.settleUnits(topic, out);
      out.push({ topic: topic.topic, payload: b.render(topic.fragments, wall), retain: false });
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

/** The names one level gives a series: `type`, `fragment` and `series`, empty when unnamed. */
export interface LevelNaming {
  type: string;
  fragment: string;
  series: string;
}

/** A level's names; `group` (deprecated) names the type and the fragment it does not name itself. */
export function levelNaming(table: unknown): LevelNaming {
  const t = table && typeof table === 'object' && !Array.isArray(table) ? (table as Table) : {};
  const text = (v: unknown) => (typeof v === 'string' ? v : '');
  const group = text(t.group);
  return { type: text(t.type) || group, fragment: text(t.fragment) || group, series: text(t.series) };
}

/** A series' resolved names. */
export interface Name {
  type: string;
  fragment: string;
  series: string;
}

/**
 * A point's type, fragment and series (contract §5.5 naming), key by key: the point's
 * `meta.measurement`, the device's `measurement`, `[connector] measurement`, then the point id
 * split at `point_separator` (the prefix names the type and the fragment, the suffix the series),
 * then the protocol for the type, the type for the fragment and the point id for the series.
 */
export function resolveName(
  naming: { connector: LevelNaming; separator: string },
  protocol: string,
  device: Table,
  point: Table,
): Name {
  const levels = [levelNaming(point.meta?.measurement), levelNaming(device.measurement), naming.connector];
  const first = (key: keyof LevelNaming) => levels.map((l) => l[key]).find((v) => v) ?? '';
  const sep = naming.separator;
  const id: string = point.id;
  let prefix = '';
  let suffix = '';
  if (sep) {
    const at = id.indexOf(sep);
    if (at > 0 && at + sep.length < id.length) {
      prefix = id.slice(0, at);
      suffix = id.slice(at + sep.length);
    }
  }
  const type = first('type') || prefix || protocol;
  const fragment = first('fragment') || prefix || type;
  const series = first('series') || suffix || id;
  return { type, fragment, series };
}
