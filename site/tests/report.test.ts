// Every case of doc/contract/test-vectors/report/vectors.json, as the Rust and C SDKs run them:
// the playground's engine cannot drift from the SDKs unnoticed.
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { checkTable, parseDuration, policyFromTable, ReportState, type Obs, type Quality } from '../src/lib/report';

const doc = JSON.parse(
  readFileSync(new URL('../../doc/contract/test-vectors/report/vectors.json', import.meta.url), 'utf8'),
);

function obsOf(sample: Record<string, unknown>): Obs {
  if (typeof sample.num === 'number') return { kind: 'number', value: sample.num };
  if ('nan' in sample) return { kind: 'number', value: NaN };
  if (typeof sample.bool === 'boolean') return { kind: 'other', value: `b:${sample.bool}` };
  if (typeof sample.str === 'string') return { kind: 'other', value: `s:${sample.str}` };
  return { kind: 'other', value: `r:${sample.raw ?? ''}` };
}

describe('report-policy vectors', () => {
  expect(doc.vectors.length).toBeGreaterThan(0);
  for (const vector of doc.vectors) {
    it(vector.name, () => {
      expect(checkTable(vector.policy), `${vector.name}: invalid policy`).toBeNull();
      const state = new ReportState<string>(policyFromTable(vector.policy), 0);
      vector.steps.forEach((step: Record<string, any>, index: number) => {
        const now = step.at;
        const published: string[] = [];
        let read = false;
        if (step.sample) {
          const quality: Quality = step.sample.quality ?? 'good';
          if (typeof step.sample.src === 'number') {
            published.push(...state.offerReplayed(step.sample.id, obsOf(step.sample), quality, step.sample.src, now));
          } else {
            const out = state.offer(step.sample.id, obsOf(step.sample), quality, now);
            if (out !== null) published.push(out);
          }
        } else if (step.tick) {
          const due = state.due(now, vector.pushed ?? false);
          if (due.publish !== null) published.push(due.publish);
          read = due.read;
        } else if (step.no_data) {
          state.noData();
        } else if (step.reset) {
          state.reset(now);
        } else {
          throw new Error(`${vector.name}: step ${index} has no kind`);
        }
        expect(published, `${vector.name}: step ${index} (at ${now} ms) published`).toEqual(step.publish ?? []);
        expect(read, `${vector.name}: step ${index} (at ${now} ms) read`).toBe(step.read ?? false);
      });
    });
  }
});

describe('table validation (same messages as the SDKs)', () => {
  it('accepts valid tables', () => {
    expect(checkTable({ on_change: true, deadband: 0.5, min_interval: '10s', max_interval: '1m' })).toBeNull();
    expect(checkTable({ deadband: '2.5%' })).toBeNull();
    expect(checkTable({ max_interval: '0', min_interval: '1h' })).toBeNull();
    expect(checkTable({ debounce: '0' })).toBeNull();
  });

  it('refuses invalid ones', () => {
    expect(checkTable({ min_interval: '10s', max_interval: '5s' })).toBe(
      'report.max_interval must be longer than report.min_interval',
    );
    expect(checkTable({ deadband: '-1%' })).toContain('report.deadband');
    expect(checkTable({ deadband: -1 })).toContain('report.deadband');
    expect(checkTable({ deadband: '2' })).toContain('report.deadband');
    expect(checkTable({ debounce: 'soon' })).toContain('report.debounce');
    expect(checkTable({ on_change: 'yes' })).toContain('report.on_change');
  });

  it('parses durations like the SDKs', () => {
    expect(parseDuration('500ms')).toBe(500);
    expect(parseDuration('1.5m')).toBe(90_000);
    expect(parseDuration('2')).toBe(2000);
    expect(parseDuration(' 2 h ')).toBe(7_200_000);
    expect(parseDuration('1.5ms')).toBeNull();
    expect(parseDuration('-1s')).toBeNull();
    expect(parseDuration('.5s')).toBeNull();
    expect(parseDuration('1e3s')).toBeNull();
  });
});
