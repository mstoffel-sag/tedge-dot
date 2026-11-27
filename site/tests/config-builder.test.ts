// Every shipped config validates against the schema (plus the builder's protocol shapes) and
// survives the builder's parse -> emit -> parse round trip with the same meaning.
import { readdirSync, readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { emitToml, newConfig, parseToml, PROTOCOL_IDS, setAt, coerce, validateConfig, validateLibrary } from '../src/lib/config-builder';

const repo = new URL('../../', import.meta.url);
const files = (dir: string) =>
  readdirSync(new URL(dir, repo))
    .filter((f) => f.endsWith('.toml'))
    .map((f) => `${dir}${f}`);

const configs = [...files('demo/config/'), ...files('packaging/config/')];
const libraries = readdirSync(new URL('demo/points.d/', repo), { withFileTypes: true })
  .filter((d) => d.isDirectory())
  .flatMap((d) => files(`demo/points.d/${d.name}/`));

describe('shipped configs', () => {
  it('are found', () => expect(configs.length).toBeGreaterThan(8));
  for (const file of configs) {
    it(`${file} validates and round-trips through the builder`, () => {
      const parsed = parseToml(readFileSync(new URL(file, repo), 'utf8'));
      expect(validateConfig(parsed), `${file} does not validate`).toEqual([]);
      const again = parseToml(emitToml(parsed));
      expect(again, `${file} changes meaning through the builder`).toEqual(parsed);
    });
  }
  for (const file of libraries) {
    it(`${file} validates as a point library`, () => {
      const parsed = parseToml(readFileSync(new URL(file, repo), 'utf8'));
      expect(validateLibrary(parsed), `${file} does not validate`).toEqual([]);
    });
  }
});

describe('builder', () => {
  for (const protocol of PROTOCOL_IDS) {
    it(`starts a valid ${protocol} config`, () => {
      expect(validateConfig(newConfig(protocol))).toEqual([]);
    });
  }

  it('builds a Modbus device with two holding-register points (spec scenario)', () => {
    let cfg = newConfig('modbus');
    cfg = setAt(cfg, '/device/0/point/1', {
      id: 'pressure',
      datatype: 'float32',
      access: 'read',
      address: { table: 'holding', address: 2, count: 2 },
    });
    expect(validateConfig(cfg)).toEqual([]);
    expect(emitToml(cfg)).toContain('[[device.point]]');
  });

  it("flags a non-numeric device port on the field (spec scenario)", () => {
    const cfg = setAt(newConfig('modbus'), '/device/0/protocol_address/port', coerce('five-oh-two', 'integer'));
    const errors = validateConfig(cfg);
    expect(errors).toContainEqual({ path: '/device/0/protocol_address/port', message: 'must be integer' });
  });

  it('names a missing required key on its own path', () => {
    const cfg = setAt(newConfig('modbus'), '/connector/protocol', undefined);
    expect(validateConfig(cfg).map((e) => e.path)).toContain('/connector/protocol');
  });

  it('emits inline tables and aligned keys', () => {
    const toml = emitToml(newConfig('modbus'));
    expect(toml).toContain('protocol_address = { transport = "tcp", host = "192.168.0.10", port = 502, unit_id = 1 }');
    expect(toml).toMatch(/^\[connector\]\nprotocol {6}= "modbus"\nservice_name {2}= /);
  });
});

describe('unknown protocols', () => {
  it('still offer device and point templates', async () => {
    const { protocolInfo } = await import('../src/lib/config-builder');
    expect(protocolInfo('bacnet').newDevice.name).toBe('device1');
    expect(protocolInfo(undefined as unknown as string).newPoint.id).toBe('point1');
  });
});

// A method point is not a value (contract §3.1, OPC UA connector spec §3.10): the builder must
// refuse what the connectors refuse, with their messages, at the field concerned.
describe('call_method points', () => {
  const config = (point: Record<string, unknown>) => ({
    connector: { protocol: 'opcua' },
    device: [{ name: 'plc', protocol_address: { endpoint: 'opc.tcp://plc:4840' }, point: [point] }],
  });
  const method = { id: 'reset', access: 'call_method', address: { object_id: 'ns=2;s=Pump', method_id: 'ns=2;s=Pump.Reset' } };

  it('accepts a method point', () => {
    expect(validateConfig(config({ ...method, address: { ...method.address, inputs: ['uint32'] } }))).toEqual([]);
  });

  for (const [field, value] of [
    ['datatype', 'int32'], ['poll_interval', '5s'], ['sampling_interval', '1s'], ['subscribe', false],
    ['report', { on_change: true }], ['map', { cases: [{ eq: 1, to: 'on' }] }], ['transform', { multiplier: 2 }],
  ] as const) {
    it(`refuses ${field} on a method point`, () => {
      expect(validateConfig(config({ ...method, [field]: value }))).toContainEqual({
        path: `/device/0/point/0/${field}`,
        message: 'does not apply to a call_method point',
      });
    });
  }

  it('refuses meta.parameter on a method point, and keeps other meta', () => {
    expect(validateConfig(config({ ...method, meta: { parameter: true } }))).toContainEqual({
      path: '/device/0/point/0/meta/parameter',
      message: 'does not apply to a call_method point',
    });
    expect(validateConfig(config({ ...method, meta: { measurement: { type: 'Pump' } } }))).toEqual([]);
  });

  it('needs the method keys and refuses variable keys on a method point', () => {
    const errors = validateConfig(config({ id: 'reset', access: 'call_method', address: { node_id: 'ns=2;s=X' } }));
    expect(errors).toContainEqual({ path: '/device/0/point/0/address/object_id', message: 'is required' });
    expect(errors).toContainEqual({ path: '/device/0/point/0/address/node_id', message: 'does not apply to a call_method point' });
  });

  it('refuses method keys on a point with another access', () => {
    expect(validateConfig(config({ ...method, access: 'read' }))).toContainEqual({
      path: '/device/0/point/0/address/object_id',
      message: 'applies only to a call_method point',
    });
  });

  it('applies to point libraries too', () => {
    const library = { library: { protocol: 'opcua' }, point: [{ ...method, poll_interval: '5s' }] };
    expect(validateLibrary(library)).toContainEqual({ path: '/point/0/poll_interval', message: 'does not apply to a call_method point' });
  });
});
