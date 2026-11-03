// Every case of doc/contract/test-vectors/measurements/vectors.json, as the Rust and C SDKs run
// them: the batching playground's engine cannot drift from the SDKs unnoticed.
import { readFileSync } from 'node:fs';
import { parse } from 'smol-toml';
import { describe, expect, it } from 'vitest';
import { Batcher, SERIES_OVERHEAD, formatNumber, type Message, type Reading, type Routing } from '../src/lib/batch';

const doc = JSON.parse(
  readFileSync(new URL('../../doc/contract/test-vectors/measurements/vectors.json', import.meta.url), 'utf8'),
);

const WALL0 = Date.parse('2026-01-01T00:00:00.000Z');

function configOf(c: any): any {
  let text: string = c.config;
  if (c.generate) {
    for (let i = 0; i < c.generate.count; i++) {
      const id = c.generate.id.replace('{i:04}', String(i).padStart(4, '0'));
      text += `[[device.point]]\nid = "${id}"\ndatatype = "${c.generate.datatype}"\naddress = {}\n`;
    }
  }
  return parse(text);
}

/** The reading a step describes, as a module would return it for the configured point. */
function readingOf(config: any, r: any): Reading {
  const device = config.device.find((d: any) => d.name === r.device);
  const point = device.point.find((p: any) => p.id === r.point);
  const mode = point.mode ?? device.default_mode ?? 'typed';
  return {
    device: r.device,
    point: r.point,
    value: mode === 'typed' ? r.value : undefined,
    ts: Date.parse(r.ts),
    quality: r.quality ?? 'good',
  };
}

describe('measurement-output vectors', () => {
  expect(doc.cases.length).toBeGreaterThan(0);
  for (const c of doc.cases) {
    it(c.name, () => {
      const config = configOf(c);
      const batcher = new Batcher(config, 0);
      const out: Message[] = [];
      const routing: Routing[] = [];
      for (const step of c.steps) {
        if (step.read) {
          routing.push(batcher.offer(readingOf(config, step.read), step.at, out));
        } else if (step.read_all) {
          const device = config.device.find((d: any) => d.name === step.read_all.device);
          for (const p of device.point) batcher.offer(readingOf(config, { ...step.read_all, point: p.id }), step.at, out);
        } else if (step.pass) {
          batcher.endOfPass(out);
        } else if (step.due) {
          batcher.due(step.at, WALL0 + step.at, out);
        } else if (step.stale) {
          batcher.setStale(step.stale.device, step.stale.stale, step.at);
        } else {
          throw new Error(`${c.name}: unknown step ${JSON.stringify(step)}`);
        }
      }
      if (c.split) {
        const counts = out.map((m) => {
          expect(m.topic).toBe(c.split.topic);
          expect(new TextEncoder().encode(m.payload).length + SERIES_OVERHEAD * 200).toBeLessThan(16_184);
          const body = JSON.parse(m.payload);
          return Object.keys(Object.entries(body).find(([k]) => k !== 'time')![1] as object).length;
        });
        expect(counts).toEqual(c.split.series);
        return;
      }
      const got = out.map((m) => ({ topic: m.topic, retain: m.retain, payload: m.payload === '' ? '' : JSON.parse(m.payload) }));
      expect(got).toEqual(c.messages);
      expect(routing).toEqual(c.routing);
    });
  }
});

describe('number layout (as Rust ryu prints it)', () => {
  it('matches the contract examples', () => {
    expect(formatNumber(1)).toBe('1');
    expect(formatNumber(1e20)).toBe('1e20');
    expect(formatNumber(1.5e-7)).toBe('1.5e-7');
    expect(formatNumber(0.0001)).toBe('0.0001');
    expect(formatNumber(-2.5)).toBe('-2.5');
    expect(formatNumber(1e15)).toBe('1000000000000000.0');
  });
});
