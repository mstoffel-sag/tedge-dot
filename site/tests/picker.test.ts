import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import YAML from 'yaml';
import { recommend, type Answers, type PackageData } from '../src/lib/picker';

const data: PackageData = YAML.parse(readFileSync(new URL('../src/data/packages.yaml', import.meta.url), 'utf8'));
const base: Answers = { architecture: 'amd64', distribution: 'debian11', protocols: ['modbus'], features: [], smallFootprint: false };

describe('package picker', () => {
  it('defaults to tedge-dot-c', () => {
    expect(recommend(data, base).pick).toBe('tedge-dot-c');
  });

  it('recommends tedge-dot-c on armhf and Debian 9, giving the glibc 2.17 floor (spec scenario)', () => {
    const r = recommend(data, { ...base, architecture: 'armhf', distribution: 'debian9' });
    expect(r.pick).toBe('tedge-dot-c');
    expect(r.reasons.join(' ')).toContain('glibc 2.17');
  });

  it('recommends tedge-dot-c for PROFIBUS, or building tedge-dot-rs from source (spec scenario)', () => {
    const r = recommend(data, { ...base, protocols: ['profibus'] });
    expect(r.pick).toBe('tedge-dot-c');
    expect(r.alternative).toContain('--features profibus');
  });

  it('builds Rust from source when PROFIBUS meets a Rust-only feature', () => {
    const r = recommend(data, { ...base, protocols: ['profibus', 'opcua'], features: ['opcua-basic128rsa15'] });
    expect(r.pick).toBe('tedge-dot-rs');
    expect(r.alternative).toContain('--features profibus');
  });

  it('builds Rust from source for PROFIBUS on a serial line', () => {
    const r = recommend(data, { ...base, features: ['profibus-serial'] });
    expect(r.pick).toBe('tedge-dot-rs');
    expect(r.alternative).toContain('--features profibus');
  });

  it('recommends tedge-dot-rs for SNMPv3 SHA-2', () => {
    const r = recommend(data, { ...base, protocols: ['snmp'], features: ['snmpv3-sha2'] });
    expect(r.pick).toBe('tedge-dot-rs');
  });

  it('recommends tedge-dot-rs on a modern gateway that needs Basic128Rsa15, and names the default as the alternative', () => {
    const r = recommend(data, { ...base, protocols: ['opcua'], features: ['opcua-basic128rsa15'] });
    expect(r.pick).toBe('tedge-dot-rs');
    expect(r.alternative).toContain('tedge-dot-c');
  });

  it('names the caveat when an old gateway needs a Rust-only feature', () => {
    const r = recommend(data, { ...base, architecture: 'armhf', features: ['snmpv3-sha2'] });
    expect(r.pick).toBe('tedge-dot-c');
    expect(r.caveats[0]).toContain('not available in tedge-dot-c');
  });

  it('the config builder marks the same protocols as preview as packages.yaml', () => {
    const protocols = JSON.parse(readFileSync(new URL('../src/data/protocols.json', import.meta.url), 'utf8'));
    const marked = Object.keys(protocols).filter((k) => protocols[k].preview === true);
    expect(marked.sort()).toEqual([...(data.preview_protocols ?? [])].sort());
    for (const id of data.preview_protocols ?? []) expect(data.protocols[id]).toBeDefined();
  });

  it('every feature the packages name has a label', () => {
    for (const p of data.packages) for (const f of p.features) expect(data.features[f]).toBeDefined();
  });
});
