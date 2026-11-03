// Every input field of the tools has a short help text.
import { describe, expect, it } from 'vitest';
import schema from '../../doc/contract/schemas/config.schema.json';
import protocols from '../src/data/protocols.json';
import { HELP, helpForPath } from '../src/lib/help';

const s = schema as any;
// Where each protocol-specific table sits in a configuration, as the builder's fields address it.
const PATHS: Record<string, string> = { connection: '/connection', protocol_address: '/device/0/protocol_address', address: '/device/0/point/0/address' };
const missing = (keys: string[], prefix: string) => keys.filter((k) => !HELP[`${prefix}.${k}`]);

describe('help texts', () => {
  it('cover every key the config builder offers in [connector], [[device]], points, report and batch', () => {
    // The builder's fields; the rest of the schema has no field (yet) and needs no text.
    const connector = ['protocol', 'service_name', 'poll_interval', 'sampling_interval', 'log_level', 'output', 'max_inflight', 'max_inflight_total', 'report', 'batch'];
    const device = ['name', 'poll_interval', 'type', 'default_mode', 'max_inflight', 'points_from'];
    const point = ['id', 'datatype', 'access', 'unit', 'poll_interval', 'sampling_interval', 'subscribe', 'report', 'batch'];
    for (const k of [...connector, ...device, ...point]) expect(k in s.properties.connector.properties || k in s.$defs.device.properties || k in s.$defs.point.properties, k).toBe(true);
    expect(missing(connector, 'connector')).toEqual([]);
    expect(missing(device, 'device')).toEqual([]);
    expect(missing(point, 'point')).toEqual([]);
    expect(missing(Object.keys(s.$defs.report.properties), 'report')).toEqual([]);
    expect(missing(['group_by', 'window', 'every', 'aggregate'], 'batch')).toEqual([]);
  });

  it("cover every key of every protocol's protocol_address, address and connection", () => {
    const gaps: string[] = [];
    for (const [protocol, info] of Object.entries(protocols as Record<string, any>)) {
      if (protocol === '$comment') continue;
      for (const table of ['protocol_address', 'address', 'connection']) {
        for (const key of Object.keys(info[table]?.properties ?? {})) {
          if (!helpForPath(`${PATHS[table]}/${key}`, protocol)) gaps.push(`${protocol}.${table}.${key}`);
        }
      }
    }
    expect(gaps).toEqual([]);
  });

  it('are short', () => {
    const long = Object.entries(HELP).filter(([, t]) => t.length > 160).map(([k]) => k);
    expect(long).toEqual([]);
  });
});
