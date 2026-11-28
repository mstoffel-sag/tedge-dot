import { canScan, currentScan, lines, scanDevices, scanOperation, scanResult, scopeProblem } from './namespace-scan';

describe('scan operation', () => {
  it('builds the operation from the dialog', () => {
    expect(scanOperation('123', 'plc-7', { include: ' Objects/Factory/** \n\n', depth: '', types: ['Variable', 'Method'] })).toEqual({
      deviceId: '123',
      description: 'Scan the address space of plc-7',
      c8y_ScanAddressSpace: { device: 'plc-7', depth: 'unlimited', include: ['Objects/Factory/**'], type: 'Variable,Method' },
    });
    expect(scanOperation('1', 'd', { root: 'ns=2;s=Line1', depth: '3', exclude: '**/Diag/**', maxChildren: '5000' })['c8y_ScanAddressSpace']).toEqual({
      device: 'd',
      root: 'ns=2;s=Line1',
      depth: 3,
      exclude: ['**/Diag/**'],
      max_children: 5000,
    });
  });

  it('checks the scope', () => {
    expect(scopeProblem({ depth: 'deep' })).toMatch(/depth/);
    expect(scopeProblem({ maxChildren: '0' })).toMatch(/child limit/);
    expect(scopeProblem({ depth: '4', maxChildren: '10' })).toBeUndefined();
    expect(lines(' a \n\n b')).toEqual(['a', 'b']);
  });

  it('offers OPC UA devices when the gateway names protocols', () => {
    const children = [
      { name: 'plc-7', 'ot-protocol': 'opcua' },
      { name: 'meter-1', 'ot-protocol': 'modbus' },
    ];
    expect(scanDevices(children).map(c => c.name)).toEqual(['plc-7']);
    expect(scanDevices([{ name: 'a' }, { name: 'b' }]).map(c => c.name)).toEqual(['a', 'b']);
  });

  it('tells gateways that can scan', () => {
    expect(canScan({ c8y_SupportedOperations: ['c8y_Restart', 'c8y_ScanAddressSpace'] })).toBe(true);
    expect(canScan({ c8y_SupportedOperations: ['c8y_Restart'] })).toBe(false);
    expect(canScan(undefined)).toBe(false);
  });

  it('reads the result from the operation', () => {
    expect(scanResult({ c8y_ScanAddressSpace: { device: 'plc-7', binaryId: '4711', nodes: 3 } })).toEqual({ binaryId: '4711', nodes: 3 });
    expect(scanResult({})).toEqual({});
  });

  it('finds the scan a new one would replace', () => {
    const stored = [
      { id: 'old', meta: { gatewayId: '1', device: 'plc-7', scannedAt: '2026-11-01T00:00:00Z' } },
      { id: 'new', meta: { gatewayId: '1', device: 'plc-7', scannedAt: '2026-11-20T00:00:00Z' } },
      { id: 'other-gateway', meta: { gatewayId: '2', device: 'plc-7', scannedAt: '2026-11-25T00:00:00Z' } },
      { id: 'other-device', meta: { gatewayId: '1', device: 'plc-8', scannedAt: '2026-11-25T00:00:00Z' } },
      { id: 'upload', meta: { device: 'plc-7', scannedAt: '2026-11-26T00:00:00Z' } },
    ];
    expect(currentScan(stored, '1', 'plc-7')?.id).toBe('new');
    expect(currentScan(stored, '1', 'plc-9')).toBeUndefined();
  });
});
