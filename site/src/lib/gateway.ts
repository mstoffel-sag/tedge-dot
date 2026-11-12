// The config workbench's model: the simulated signals, the connector configuration that reads
// them (edited in the config builder), and what the simulation takes from the two. A config point
// reads the signal with the same id. Only subscribed OPC UA points are pushed; every other point
// is polled.
//
// The configuration is the plain object TOML parses to, so the batcher (src/lib/batch.ts) runs
// on exactly the config the builder shows.

import { groupingTable, type GroupingForm } from './batching';
import { groupingOf } from './batch';
import { parseToml, protocolInfo, type Config } from './config-builder';
import { SIGNALS, type Outage, type SignalId, type SignalSpec } from './playground';
import type { ReportTable } from './report';

/** A simulated signal. */
export interface SimSignal {
  /** Its name: the config point with this id reads it. */
  id: string;
  shape: SignalId;
  /** A period in which every read of it fails. */
  outage: Outage;
}

/** Whether the connector subscribes to this point (OPC UA, unless `subscribe = false`). */
export function isPushed(protocol: string, point: Config): boolean {
  return protocol === 'opcua' && point.subscribe !== false;
}

export const unitOfShape = (shape: SignalId) => SIGNALS.find((s) => s.id === shape)?.unit ?? '';

/** A new config point that reads `signal`, with an address of the protocol's shape. */
export function pointFor(protocol: string, signal: SimSignal, index: number, extra: Config = {}): Config {
  const unit = unitOfShape(signal.shape);
  const address =
    protocol === 'modbus'
      ? { table: 'holding', address: index * 2, count: 2 }
      : protocol === 'opcua'
        ? { node_id: `ns=2;s=${signal.id}` }
        : structuredClone(protocolInfo(protocol).newPoint.address ?? {});
  return {
    id: signal.id,
    datatype: protocol === 'modbus' ? 'float32' : 'float64',
    access: 'read',
    ...(unit ? { unit } : {}),
    ...extra,
    address,
  };
}

/** A whole configuration for the signals: one device, a point per signal. */
export function configFor(protocol: string, signals: SignalSpec[], grouping: Record<string, unknown> | null = { by: 'time' }): Config {
  const device = structuredClone(protocolInfo(protocol).newDevice);
  return {
    connector: {
      protocol,
      service_name: `tedge-dot-${protocol}`,
      poll_interval: '1s',
      output: 'group',
      measurement: { type: 'Process' },
      ...(grouping ? { grouping } : {}),
    },
    mqtt: { host: '127.0.0.1', port: 1883 },
    device: [
      {
        ...device,
        name: 'line1',
        point: signals.map((s, i) => {
          const extra: Config = {};
          if (protocol === 'opcua' && s.source === 'pushed') extra.sampling_interval = s.interval;
          else {
            if (s.interval !== '1s') extra.poll_interval = s.interval;
            if (protocol === 'opcua') extra.subscribe = false;
          }
          if (Object.keys(s.report).length) extra.report = s.report;
          return pointFor(protocol, s, i, extra);
        }),
      },
    ],
  };
}

/**
 * The configuration moved to another protocol: the device's address and every point's address
 * are replaced, push settings dropped where the protocol has none; ids, units and policies stay.
 */
export function switchProtocol(config: Config, protocol: string, sims: SimSignal[]): Config {
  const device = config.device?.[0] ?? {};
  const template = structuredClone(protocolInfo(protocol).newDevice);
  const points = ((device.point ?? []) as Config[]).map((p, i) => {
    const { address, subscribe, sampling_interval, datatype, ...rest } = p;
    const signal = sims.find((e) => e.id === p.id) ?? { id: p.id, shape: 'flat' as SignalId, outage: 'none' as Outage };
    const keep: Config = { ...rest };
    if (protocol === 'opcua') {
      if (sampling_interval !== undefined) keep.sampling_interval = sampling_interval;
      else keep.subscribe = false;
    }
    const { address: fresh, ...made } = pointFor(protocol, signal, i);
    return { ...made, ...keep, address: fresh };
  });
  const { sampling_interval: _, ...connector } = config.connector ?? {};
  return {
    ...config,
    connector: { ...connector, protocol, service_name: `tedge-dot-${protocol}`, ...(protocol === 'opcua' && config.connector?.sampling_interval ? { sampling_interval: config.connector.sampling_interval } : {}) },
    device: [{ ...device, protocol_address: template.protocol_address, point: points }, ...(config.device ?? []).slice(1)],
  };
}

/** The configuration's points, edited by `edit`, on its first device. */
function editPoints(config: Config, edit: (points: Config[], protocol: string) => Config[]): Config {
  const device = config.device?.[0] ?? { name: 'line1' };
  const points = edit([...((device.point ?? []) as Config[])], config.connector?.protocol ?? '');
  return { ...config, device: [{ ...device, point: points }, ...(config.device ?? []).slice(1)] };
}

/** A signal was added: a point reads it, as the connector does by default (polled, or subscribed on OPC UA). */
export const addPoint = (config: Config, signal: SimSignal) =>
  editPoints(config, (points, protocol) => (points.some((p) => p.id === signal.id) ? points : [...points, pointFor(protocol, signal, points.length)]));

/** A signal was removed: so is its point. */
export const removePoint = (config: Config, id: string) => editPoints(config, (points) => points.filter((p) => p.id !== id));

/** A signal was renamed: the point that read it follows. */
export const renamePoint = (config: Config, from: string, to: string) =>
  editPoints(config, (points, protocol) =>
    points.map((p) => (p.id === from ? { ...p, id: to, ...(protocol === 'opcua' ? { address: { ...p.address, node_id: `ns=2;s=${to}` } } : {}) } : p)),
  );

/** A signal's shape changed: its point takes the unit of the new one. */
export const reunitPoint = (config: Config, signal: SimSignal) =>
  editPoints(config, (points) =>
    points.map((p) => {
      if (p.id !== signal.id) return p;
      const { unit: _, ...rest } = p;
      const unit = unitOfShape(signal.shape);
      return unit ? { ...rest, unit } : rest;
    }),
  );

// Point libraries the workbench knows (demo/points.d), by "<protocol>/<name>", as `points_from`
// names them (contract §3.4). Set once from the page.
let libraries: Record<string, Config[]> = {};

export function setLibraries(texts: Record<string, string> = {}) {
  libraries = Object.fromEntries(
    Object.entries(texts).map(([key, text]) => {
      try {
        return [key, (parseToml(text).point ?? []) as Config[]];
      } catch {
        return [key, []];
      }
    }),
  );
}

const isTable = (v: unknown): v is Config => v !== null && typeof v === 'object' && !Array.isArray(v);
const deepMerge = (a: Config, b: Config): Config =>
  Object.fromEntries([...new Set([...Object.keys(a), ...Object.keys(b)])].map((k) => [k, isTable(a[k]) && isTable(b[k]) ? deepMerge(a[k], b[k]) : k in b ? b[k] : a[k]]));

/** A later definition of a point patching an earlier one (§3.4): `meta` and `transform` merge, the rest is replaced. */
function patchPoint(base: Config, over: Config): Config {
  const out = { ...base, ...over };
  if (isTable(base.meta) && isTable(over.meta)) out.meta = deepMerge(base.meta, over.meta);
  if (isTable(base.transform) && isTable(over.transform)) out.transform = { ...base.transform, ...over.transform };
  return out;
}

/** `points_from` names the workbench has no library for. */
export function unknownLibraries(config: Config): string[] {
  const protocol = config.connector?.protocol ?? '';
  return ((config.device ?? []) as Config[]).flatMap((d) => ((d.points_from ?? []) as string[]).filter((n) => !libraries[`${protocol}/${n}`]));
}

/** The configuration with every device's library points collected and its inline points applied over them, as the connector loads it. */
export function expandLibraries(config: Config): Config {
  const protocol = config.connector?.protocol ?? '';
  if (!((config.device ?? []) as Config[]).some((d) => d.points_from?.length)) return config;
  return {
    ...config,
    device: ((config.device ?? []) as Config[]).map((d) => {
      if (!d.points_from?.length) return d;
      const { points_from, ...rest } = d;
      const collected: Config[] = [];
      for (const list of [...(points_from as string[]).map((n) => libraries[`${protocol}/${n}`] ?? []), (d.point ?? []) as Config[]]) {
        for (const p of list) {
          const i = collected.findIndex((x) => x.id === p.id);
          if (i < 0) collected.push(p);
          else collected[i] = patchPoint(collected[i], p);
        }
      }
      return { ...rest, point: collected };
    }),
  };
}

const first = (...values: unknown[]) => values.find((v) => typeof v === 'string' && v !== '') as string | undefined;

/** What the simulation runs: one spec per config point that reads a signal. */
export interface Resolved {
  specs: SignalSpec[];
  /** Signals no config point reads. */
  unread: string[];
  /** Config points that read no signal. */
  orphans: string[];
  /** For each spec: where its report policy and its interval come from. */
  origins: { report: string[]; interval: string }[];
}

/** The signals as the configuration reads them (contract §3.4 resolution, §5.3 merging). */
export function resolve(config: Config, sims: SimSignal[]): Resolved {
  config = expandLibraries(config);
  const connector = config.connector ?? {};
  const protocol: string = connector.protocol ?? '';
  const device = config.device?.[0] ?? {};
  const points = ((device.point ?? []) as Config[]).filter((p) => p.enabled !== false);
  const specs: SignalSpec[] = [];
  const origins: Resolved['origins'] = [];
  const orphans: string[] = [];
  for (const p of points) {
    const signal = sims.find((e) => e.id === p.id);
    if (!signal) {
      orphans.push(String(p.id ?? ''));
      continue;
    }
    const pushed = isPushed(protocol, p);
    const polled = first(p.poll_interval, device.poll_interval, connector.poll_interval) ?? '2s';
    let interval = pushed ? first(p.sampling_interval, device.sampling_interval, connector.sampling_interval) ?? polled : polled;
    // "0" asks the server for its fastest rate; the simulation takes 100 ms.
    if (interval === '0') interval = '100ms';
    const report: ReportTable = { ...(connector.report ?? {}), ...(device.report ?? {}), ...(p.report ?? {}) };
    const from = [connector.report && '[connector]', device.report && '[[device]]', p.report && 'point'].filter(Boolean) as string[];
    specs.push({ id: String(p.id), shape: signal.shape, source: pushed ? 'pushed' : 'polled', interval, outage: signal.outage, report });
    origins.push({
      report: from,
      interval: pushed
        ? p.sampling_interval ? 'point sampling_interval' : device.sampling_interval ? 'device sampling_interval' : connector.sampling_interval ? '[connector] sampling_interval' : 'poll_interval, no sampling_interval set'
        : p.poll_interval ? 'point poll_interval' : device.poll_interval ? 'device poll_interval' : connector.poll_interval ? '[connector] poll_interval' : 'the default poll_interval',
    });
  }
  const unread = sims.filter((e) => !points.some((p) => p.id === e.id)).map((e) => e.id);
  return { specs, unread, orphans, origins };
}

/**
 * The configuration with one `grouping` policy for every point (the comparison strip's rows).
 * A deprecated `batch` table anywhere is dropped with the others.
 */
export function withGrouping(config: Config, form: GroupingForm): Config {
  const strip = (o: Config) => {
    const { batch: _, grouping: __, ...rest } = o;
    return rest;
  };
  return {
    ...config,
    connector: { ...strip(config.connector ?? {}), grouping: groupingTable(form) },
    device: ((config.device ?? []) as Config[]).map((d) => ({ ...strip(d), point: ((d.point ?? []) as Config[]).map(strip) })),
  };
}

/**
 * The grouping form a `[connector] grouping` table stands for (or a deprecated `batch`, with
 * `group_by`); `memory` fills durations it lacks.
 */
export function formOfGrouping(connector: Config | undefined, memory: GroupingForm): GroupingForm {
  const t: Config = groupingOf(connector) ?? {};
  const aggregate = t.aggregate;
  const id =
    aggregate === undefined ? 'none' : typeof aggregate === 'string' ? aggregate : aggregate.length === 3 ? 'mean_min_max' : 'all';
  const by = t.by ?? (aggregate !== undefined ? 'sample' : 'time');
  return {
    by: ['time', 'frame', 'sample'].includes(by) ? by : 'time',
    window: typeof t.window === 'string' ? t.window : memory.window,
    every: typeof t.every === 'string' ? t.every : memory.every,
    aggregate: by === 'sample' ? id : memory.aggregate,
  };
}

/** Whether any device or point sets its own `grouping` (or deprecated `batch`) over `[connector]`'s. */
export function hasOwnGrouping(config: Config): boolean {
  config = expandLibraries(config);
  const own = (o: Config) => o.grouping !== undefined || o.batch !== undefined;
  return ((config.device ?? []) as Config[]).some((d) => own(d) || ((d.point ?? []) as Config[]).some(own));
}

/** The package-picker answers a configuration settles: its protocol and the features it uses. */
export function packageNeeds(config: Config): { protocols: string[]; features: string[] } {
  config = expandLibraries(config);
  const protocol: string = config.connector?.protocol ?? '';
  const features = new Set<string>();
  const devices = (config.device ?? []) as Config[];
  const policies = [config.connection?.security_policy, ...devices.map((d) => d.protocol_address?.security_policy)];
  if (protocol === 'opcua') {
    if (policies.some((p) => typeof p === 'string' && /basic128rsa15/i.test(p))) features.add('opcua-basic128rsa15');
  }
  return { protocols: protocol ? [protocol] : [], features: [...features] };
}

/** The signals a loaded configuration reads: one per point of its first device (at most `max`). */
export function signalsFor(config: Config, max: number): { sims: SimSignal[]; notes: string[] } {
  const unknown = unknownLibraries(config);
  config = expandLibraries(config);
  const devices = (config.device ?? []) as Config[];
  const points = ((devices[0]?.point ?? []) as Config[]).filter((p) => typeof p.id === 'string' && p.id && p.enabled !== false);
  const notes: string[] = [];
  if (devices.length > 1) notes.push(`Only the first device, ${devices[0].name ?? ''}, is simulated.`);
  if (unknown.length) notes.push(`No library ${unknown.join(', ')} here: its points are not simulated.`);
  if (points.length > max) notes.push(`Only the first ${max} points get a signal; the others read nothing here.`);
  const shapeFor = (p: Config): SignalId => {
    if (p.datatype === 'bool' || p.datatype === 'boolean') return 'flapping';
    return SIGNALS.find((s) => s.unit && s.unit === p.unit)?.id ?? 'noisy';
  };
  const sims = points.slice(0, max).map((p) => ({ id: p.id, shape: shapeFor(p), outage: 'none' as Outage }));
  return { sims, notes };
}
