// The grouping playground's examples show what its page says they show, on the configuration
// the config builder would show for them.
import { describe, expect, it } from 'vitest';
import {
  c8yMeasurement,
  connectorToml,
  EXAMPLES,
  fragmentWrites,
  groupingToml,
  layoutOf,
  simulate,
  tedgeMeasurement,
  withLayout,
  type GroupingForm,
} from '../src/lib/batching';
import { readFileSync } from 'node:fs';
import { parseToml } from '../src/lib/config-builder';
import { addPoint, configFor, expandLibraries, formOfGrouping, hasOwnGrouping, resolve, setLibraries, signalsFor, switchProtocol, withGrouping } from '../src/lib/gateway';
import type { SignalSpec } from '../src/lib/playground';
import { stateOf } from '../src/lib/signals-store';

const form = (f: Partial<GroupingForm>): GroupingForm => ({ by: 'time', window: '500ms', every: '1s', aggregate: 'none', ...f });
const example = (label: string) => EXAMPLES.find((e) => e.label === label)!;
const run = (label: string, f: Partial<GroupingForm>, layout: 'type' | 'signal' = 'type') => {
  const e = example(label);
  return simulate(e.signals, e.durationMs, withLayout(withGrouping(configFor(e.protocol, e.signals), form(f)), layout));
};
const values = (r: ReturnType<typeof run>, fragment: string, series: string) => r.measurements.map((m) => m.fragments[fragment]?.[series]);

describe('grouping playground', () => {
  it('polled points read in one pass share a measurement; a filtered or slower one is missing from most', () => {
    const r = run('Three polled points, filtered', {});
    expect(r.measurements.length).toBeGreaterThan(0);
    expect(r.measurements.every((m) => m.has.some(Boolean))).toBe(true);
    // outdoor is polled every 5 s, the others every second.
    const outdoor = r.measurements.filter((m) => m.has[2]);
    expect(outdoor.length).toBeLessThan(r.measurements.length / 2);
    expect(outdoor.every((m) => m.time % 5000 === 0)).toBe(true);
    expect(r.maxShift).toBe(0);
    expect(r.dropped).toEqual([]);
  });

  it('pushed times a few ms apart: one measurement each under time, far fewer under frame', () => {
    const time = run('Pushed points, times ms apart', {});
    expect(time.measurements.every((m) => m.has.filter(Boolean).length === 1)).toBe(true);
    const frame = run('Pushed points, times ms apart', { by: 'frame', window: '100ms' });
    expect(frame.measurements.length).toBeLessThan(time.measurements.length);
    expect(frame.maxShift).toBeLessThan(100);
    expect(frame.dropped).toEqual([]);
  });

  it('a signal in an outage is left out of samples and of time measurements', () => {
    const held = run('A signal drops out', { by: 'sample', every: '5s' });
    const gap = held.measurements.filter((m) => m.time > 50_000 && m.time < 75_000);
    expect(gap.length).toBeGreaterThan(0);
    expect(gap.every((m) => m.has[0] && !m.has[1])).toBe(true);
    expect(held.measurements.at(-1)!.has[1]).toBe(true);
    const time = run('A signal drops out', {});
    expect(time.measurements.filter((m) => m.time > 50_000 && m.time < 75_000).every((m) => !m.has[1])).toBe(true);
  });

  it('sample and hold misses the spike; the max aggregate keeps it', () => {
    const held = run('A spike between samples', { by: 'sample', every: '10s' });
    expect(values(held, 'Process', 'pressure')).not.toContain(5.2);
    expect(held.dropped.length).toBeGreaterThan(0);
    const agg = run('A spike between samples', { by: 'sample', every: '10s', aggregate: 'mean_min_max' });
    expect(values(agg, 'Process', 'pressure_max')).toContain(5.2);
    expect(c8yMeasurement(agg.measurements[0], agg).Process.pressure_max).toEqual({ value: expect.any(Number), unit: 'bar' });
    expect(agg.topic).toBe('te/device/line1///m/Process');
  });

  it('a value held back by min_interval keeps its own time', () => {
    const signals: SignalSpec[] = [{ id: 'temperature', shape: 'noisy', source: 'polled', interval: '3s', outage: 'none', report: { min_interval: '10s' } }];
    const r = simulate(signals, 60_000, configFor('modbus', signals));
    expect(r.inputs.some((x) => x.at > x.ts)).toBe(true);
    for (const m of r.measurements) expect(m.time).toBe(r.inputs[m.from[0]].ts);
  });

  it('a point grouping table over [connector] is what the engine runs', () => {
    const e = example('A spike between samples');
    const config = configFor(e.protocol, e.signals, { by: 'time' });
    config.device[0].point[0].grouping = { aggregate: ['mean', 'min', 'max'], every: '10s' };
    const r = simulate(e.signals, e.durationMs, config);
    expect(r.measurements.some((m) => 'pressure_max' in m.fragments.Process && !('temperature' in m.fragments.Process))).toBe(true);
  });

  it('a deprecated batch table runs as grouping, and the form reads it', () => {
    const e = example('A spike between samples');
    const config = configFor(e.protocol, e.signals, null);
    config.device[0].point[0].batch = { group_by: 'sample', every: '10s', aggregate: ['mean', 'min', 'max'] };
    const r = simulate(e.signals, e.durationMs, config);
    expect(r.measurements.some((m) => 'pressure_max' in m.fragments.Process)).toBe(true);
    expect(hasOwnGrouping(config)).toBe(true);
    expect(formOfGrouping({ batch: { group_by: 'frame', window: '2s' } }, form({}))).toMatchObject({ by: 'frame', window: '2s' });
    // The comparison strip drops it with the other tables.
    expect(hasOwnGrouping(withGrouping(config, form({})))).toBe(false);
  });

  it('writes the grouping table and the connector settings', () => {
    expect(groupingToml(form({ by: 'frame', window: '100ms' }))).toBe('grouping = { by = "frame", window = "100ms" }');
    expect(groupingToml(form({ by: 'sample', aggregate: 'mean_min_max' }))).toBe('grouping = { aggregate = ["mean", "min", "max"], every = "1s" }');
    const toml = connectorToml(form({}), 'Process', 'signal');
    expect(toml).toContain('output = "group"');
    expect(toml).toContain('measurement = { type = "Process" }');
    expect(toml).not.toMatch(/batch|group_by|measurements/);
    expect(parseToml(toml)).toBeTruthy();
  });

  it('a fragment per signal: one measurement per pass holds several fragments of one type', () => {
    const one = run('Three polled points, filtered', {});
    const per = run('Three polled points, filtered', {}, 'signal');
    // The same messages, on the same topic: the fragments do not split them.
    expect(per.measurements.length).toBe(one.measurements.length);
    expect(per.measurements.every((m) => m.type === 'Process')).toBe(true);
    const full = per.measurements.find((m) => m.has.every(Boolean))!;
    expect(Object.keys(full.fragments)).toEqual(['temperature', 'setpoint', 'outdoor']);
    expect(Object.keys(full.fragments.temperature)).toEqual(['value']);
    expect(tedgeMeasurement(full)).toMatchObject({ time: expect.any(String), temperature: { value: expect.any(Number) } });
    expect(c8yMeasurement(full, per)).toMatchObject({ type: 'Process', temperature: { value: { value: expect.any(Number), unit: '°C' } } });
    // Every fragment of a measurement is a write of its own in Streaming Lake Ingestion.
    expect(fragmentWrites(one)).toBe(one.measurements.length);
    expect(fragmentWrites(per)).toBeGreaterThan(per.measurements.length);
  });

  it('reads and writes the fragment layout', () => {
    const e = example('Three polled points, filtered');
    const config = configFor(e.protocol, e.signals);
    expect(layoutOf(config)).toBe('type');
    const per = withLayout(config, 'signal');
    expect(layoutOf(per)).toBe('signal');
    expect(per.device[0].point[0].meta).toEqual({ measurement: { fragment: 'temperature', series: 'value' } });
    expect(layoutOf(withLayout(per, 'type'))).toBe('type');
    per.device[0].point[0].meta.measurement.series = 'T';
    expect(layoutOf(per)).toBe('custom');
  });
});

describe('signals and configuration', () => {
  it('every example round-trips: the configuration made for it resolves to its signals', () => {
    for (const e of EXAMPLES) {
      const s = stateOf(e.protocol, e.signals, e.durationMs);
      const r = resolve(s.config, s.sims);
      expect(r.specs, e.label).toEqual(e.signals);
      expect(r.unread).toEqual([]);
      expect(r.orphans).toEqual([]);
    }
  });

  it('a point inherits poll_interval and report from [connector], key by key', () => {
    const s = stateOf('opcua', [{ id: 'temperature', shape: 'noisy', source: 'polled', interval: '1s', outage: 'none', report: { deadband: 0.5 } }], 60_000);
    s.config.connector.poll_interval = '5s';
    s.config.connector.report = { max_interval: '1m', deadband: 2 };
    const [spec] = resolve(s.config, s.sims).specs;
    expect(spec.interval).toBe('5s');
    expect(spec.report).toEqual({ max_interval: '1m', deadband: 0.5 });
  });

  it('an OPC UA point subscribes unless subscribe = false; sampling_interval falls back to poll_interval', () => {
    const s = stateOf('opcua', [{ id: 'temperature', shape: 'noisy', source: 'polled', interval: '2s', outage: 'none', report: {} }], 60_000);
    delete s.config.device[0].point[0].subscribe;
    expect(resolve(s.config, s.sims).specs[0]).toMatchObject({ source: 'pushed', interval: '2s' });
  });

  it('an added signal is read as the connector reads a point by default', () => {
    const opcua = stateOf('opcua', [], 60_000);
    const signal = { id: 'outdoor', shape: 'sine' as const, outage: 'none' as const };
    expect(resolve(addPoint(opcua.config, signal), [signal]).specs[0]).toMatchObject({ source: 'pushed', interval: '1s' });
    const modbus = stateOf('modbus', [], 60_000);
    expect(resolve(addPoint(modbus.config, signal), [signal]).specs[0]).toMatchObject({ source: 'polled' });
  });

  it('a configuration from the config builder gets a signal per point, and says what it leaves out', () => {
    const e = example('Three polled points, filtered');
    const config = configFor(e.protocol, e.signals);
    config.device.push({ name: 'line2', point: [] });
    const { sims, notes } = signalsFor(config, 2);
    expect(sims.map((s) => s.id)).toEqual(['temperature', 'setpoint']);
    expect(sims[0].shape).toBe('noisy');
    expect(notes.join(' ')).toMatch(/first device.*first 2 points/);
  });

  it('switching the protocol keeps ids and policies, so the signals are still read', () => {
    const e = example('Three polled points, filtered');
    const s = stateOf(e.protocol, e.signals, e.durationMs);
    for (const protocol of ['opcua', 'snmp', 'canopen']) {
      const config = switchProtocol(s.config, protocol, s.sims);
      expect(config.connector.protocol).toBe(protocol);
      const r = resolve(config, s.sims);
      expect(r.specs.map((x) => [x.id, x.report])).toEqual(e.signals.map((x) => [x.id, x.report]));
    }
  });

  it('a demo config gets its points from its point library, with inline points applied over them', () => {
    setLibraries({ 'modbus/demo-sim': readFileSync('../demo/points.d/modbus/demo-sim.toml', 'utf8') });
    const config = parseToml(readFileSync('../demo/config/modbus.toml', 'utf8'));
    const library = parseToml(readFileSync('../demo/points.d/modbus/demo-sim.toml', 'utf8')).point as any[];
    const points = expandLibraries(config).device[0].point as any[];
    expect(points.map((p) => p.id).slice(0, library.length)).toEqual(library.map((p) => p.id));
    const { sims, notes } = signalsFor(config, 4);
    expect(sims.map((s) => s.id)).toEqual(points.filter((p) => p.enabled !== false).slice(0, 4).map((p) => p.id));
    expect(notes.join(' ')).not.toMatch(/No library/);
    expect(resolve(config, sims).specs).toHaveLength(4);
    // A patch overrides the library's definition, key by key.
    const patched = { ...config, device: [{ ...config.device[0], point: [{ id: library[0].id, poll_interval: '9s' }] }] };
    expect(expandLibraries(patched).device[0].point[0]).toMatchObject({ ...library[0], poll_interval: '9s' });
  });

  it('a signal no point reads, and a point with no signal, are reported', () => {
    const s = stateOf('modbus', [{ id: 'temperature', shape: 'noisy', source: 'polled', interval: '1s', outage: 'none', report: {} }], 60_000);
    s.config.device[0].point[0].id = 'temp';
    expect(resolve(s.config, s.sims)).toMatchObject({ specs: [], unread: ['temperature'], orphans: ['temp'] });
  });
});

describe('the tools keep up with the contract schema', async () => {
  const schema = (await import('../../doc/contract/schemas/config.schema.json')).default as any;
  const { POLICIES, AGGREGATES, OUTPUTS } = await import('../src/lib/batching');
  const { REPORT_FIELDS } = await import('../src/lib/playground');

  it('the grouping playground and the config builder offer every grouping.by, every aggregate and the current output names', () => {
    expect(POLICIES.map((p) => p.id).sort()).toEqual([...schema.$defs.grouping.properties.by.enum].sort());
    expect(OUTPUTS).toEqual(schema.properties.connector.properties.output.enum.filter((o: string) => o !== 'samples' && o !== 'measurements'));
    const offered = new Set(AGGREGATES.flatMap((a) => a.value ?? []));
    expect(schema.$defs.aggregate.enum.filter((a: string) => !offered.has(a)), 'aggregates the tools do not offer').toEqual([]);
  });

  it('the config builder has a field for every report key', () => {
    expect(['on_change', ...REPORT_FIELDS.map((f) => f.key)].sort()).toEqual(Object.keys(schema.$defs.report.properties).sort());
  });
});
