import type { NamespaceMetadata } from './namespace-file';
import { scanProtocol, scansFor, targetForScan } from './namespace-context';

const meta = (m: Partial<NamespaceMetadata>) => m as NamespaceMetadata;

describe('picker context', () => {
  const stored = [
    { id: 'old-same-file', lastUpdated: '2026-11-01', meta: meta({ device: 'plc-7', connectorFile: 'opcua' }) },
    { id: 'new-other-file', lastUpdated: '2026-11-20', meta: meta({ device: 'plc-7', connectorFile: 'line' }) },
    { id: 'new-same-file', lastUpdated: '2026-11-10', meta: meta({ device: 'plc-7', connectorFile: 'opcua' }) },
    { id: 'other-device', lastUpdated: '2026-11-25', meta: meta({ device: 'plc-8', connectorFile: 'opcua' }) },
  ];

  it('finds the scans of a device, same connector file first, newest first', () => {
    expect(scansFor(stored, { kind: 'connector', protocol: 'opcua', device: 'plc-7', fileName: 'opcua' }).map(s => s.id)).toEqual([
      'new-same-file',
      'old-same-file',
      'new-other-file',
    ]);
    expect(scansFor(stored, { kind: 'connector', protocol: 'opcua', device: 'plc-9', fileName: 'opcua' })).toEqual([]);
  });

  it('offers every scan for a library, newest first', () => {
    expect(scansFor(stored, { kind: 'library', protocol: 'opcua', fileName: 'acme' }).map(s => s.id)).toEqual([
      'other-device',
      'new-other-file',
      'new-same-file',
      'old-same-file',
    ]);
  });

  it('offers only scans of the file\'s protocol', () => {
    const mixed = [
      ...stored,
      { id: 'canopen-plc-7', lastUpdated: '2026-11-26', meta: meta({ protocol: 'canopen', device: 'plc-7', connectorFile: 'opcua' }) },
      { id: 'no-metadata', lastUpdated: '2026-11-27' },
    ];
    expect(scansFor(mixed, { kind: 'connector', protocol: 'opcua', device: 'plc-7', fileName: 'opcua' }).map(s => s.id)).not.toContain('canopen-plc-7');
    expect(scansFor(mixed, { kind: 'library', protocol: 'opcua', fileName: 'x' }).map(s => s.id)).toContain('no-metadata');
    expect(scansFor(mixed, { kind: 'library', protocol: 'canopen', fileName: 'x' }).map(s => s.id)).toEqual(['canopen-plc-7']);
    expect(scanProtocol(undefined)).toBe('opcua');
  });

  it('finds the connector entry and device a scan belongs to', () => {
    const candidates = [
      { id: '1', fileName: 'opcua2', devices: ['opcutest'] },
      { id: '2', fileName: 'opcua-demo', devices: ['Pump00', 'Pump01'] },
    ];
    expect(targetForScan(meta({ connectorFile: 'opcua-demo', device: 'Pump01' }), candidates)).toEqual({ entryId: '2', device: 1 });
    expect(targetForScan(meta({ connectorFile: 'opcua-demo', device: 'Pump09' }), candidates)).toBeUndefined();
    expect(targetForScan(meta({ device: 'Pump01' }), candidates)).toBeUndefined();
    expect(targetForScan(undefined, candidates)).toBeUndefined();
  });
});
