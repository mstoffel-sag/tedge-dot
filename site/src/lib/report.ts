// The reporting policy (report by exception, contract §5.3), ported from the Rust SDK
// (impl/rust/crates/sdk/src/report.rs) for the report-policy playground.
//
// It is held to the same rules by the same test vectors: tests/report.test.ts runs every case
// in doc/contract/test-vectors/report/vectors.json, as the Rust and C SDKs do. A vector that
// this port does not satisfy fails the docs check. Times are milliseconds from any origin.

export type Quality = 'good' | 'bad' | 'stale';

/** What a reading is compared by: numbers numerically, everything else exactly. */
export type Obs = { kind: 'number'; value: number } | { kind: 'other'; value: string };

export type Deadband = { kind: 'absolute'; value: number } | { kind: 'percent'; value: number };

export interface ReportPolicy {
  onChange: boolean;
  deadband: Deadband | null;
  minInterval: number | null;
  maxInterval: number | null;
  debounce: number | null;
}

/** A `report` table as written in a configuration. */
export interface ReportTable {
  on_change?: boolean;
  deadband?: number | string;
  min_interval?: string;
  max_interval?: string;
  debounce?: string;
}

/** Differences up to this are not a change for `on_change` without a deadband. */
const EPSILON = 1e-9;

/** A thin-edge duration (`"500ms"`, `"2s"`, `"1.5m"`, `"2h"`; no unit = seconds) in ms. */
export function parseDuration(input: string): number | null {
  const s = input.trim();
  const at = s.search(/[^0-9.]/);
  const number = at < 0 ? s : s.slice(0, at);
  const unit = at < 0 ? '' : s.slice(at).trim();
  const digits = (part: string) => /^[0-9]+$/.test(part);
  const parts = number.split('.');
  const wellFormed = parts.length === 1 ? digits(parts[0]) : parts.length === 2 && digits(parts[0]) && digits(parts[1]);
  if (!wellFormed) return null;
  if (unit === 'ms') return parts.length === 1 ? Number(number) : null;
  const scale = { '': 1000, s: 1000, m: 60_000, h: 3_600_000 }[unit];
  if (scale === undefined) return null;
  return Number(number) * scale;
}

/** `"<p>%"` as `p`. */
export function parsePercent(s: string): number | null {
  if (!s.endsWith('%')) return null;
  const number = s.slice(0, -1);
  if (!/^[0-9]+(\.[0-9]+)?$/.test(number)) return null;
  return Number(number);
}

/** Build the policy from a (merged) table. A zero duration or deadband switches it off. */
export function policyFromTable(table: ReportTable): ReportPolicy {
  const duration = (v: string | undefined) => {
    const d = typeof v === 'string' ? parseDuration(v) : null;
    return d ? d : null;
  };
  let deadband: Deadband | null = null;
  if (typeof table.deadband === 'number' && table.deadband > 0) {
    deadband = { kind: 'absolute', value: table.deadband };
  } else if (typeof table.deadband === 'string') {
    const p = parsePercent(table.deadband);
    if (p !== null && p > 0) deadband = { kind: 'percent', value: p };
  }
  return {
    onChange: table.on_change === true,
    deadband,
    minInterval: duration(table.min_interval),
    maxInterval: duration(table.max_interval),
    debounce: duration(table.debounce),
  };
}

/** Validate one table, with the SDKs' messages. Returns null when it is valid. */
export function checkTable(table: Record<string, unknown>): string | null {
  if ('on_change' in table && typeof table.on_change !== 'boolean') {
    return 'report.on_change must be true or false';
  }
  if ('deadband' in table) {
    const v = table.deadband;
    const ok =
      (typeof v === 'number' && Number.isFinite(v) && v >= 0) ||
      (typeof v === 'string' && parsePercent(v) !== null);
    if (!ok) return 'report.deadband must be a number >= 0 or a percentage such as "2%"';
  }
  const durations: Record<string, number | null> = {};
  for (const key of ['min_interval', 'max_interval', 'debounce']) {
    if (!(key in table)) continue;
    const v = table[key];
    const d = typeof v === 'string' ? parseDuration(v) : null;
    if (d === null) {
      return `report.${key} must be a duration such as "500ms", "2s" or "5m" ("0" switches it off)`;
    }
    durations[key] = d;
  }
  const min = durations.min_interval;
  const max = durations.max_interval;
  if (min && max && max <= min) return 'report.max_interval must be longer than report.min_interval';
  return null;
}

function filtersChanges(p: ReportPolicy): boolean {
  return p.onChange || p.deadband !== null || p.debounce !== null;
}

/** Whether `obs` is a change from `base` under the policy. */
export function changed(p: ReportPolicy, obs: Obs, base: Obs): boolean {
  if (obs.kind === 'number' && base.kind === 'number') {
    const a = obs.value;
    const b = base.value;
    if (Number.isNaN(a) || Number.isNaN(b)) return Number.isNaN(a) !== Number.isNaN(b);
    const delta = Math.abs(a - b);
    if (p.deadband?.kind === 'absolute') return delta >= p.deadband.value;
    if (p.deadband?.kind === 'percent') {
      if (b === 0) return delta > 0;
      return delta >= (p.deadband.value / 100) * Math.abs(b);
    }
    return delta > EPSILON;
  }
  if (obs.kind === 'other' && base.kind === 'other') return obs.value !== base.value;
  return true;
}

interface Candidate<T> {
  first: Obs;
  since: number;
  latest: T;
  latestObs: Obs;
}

export interface Due<T> {
  /** A held or settled reading to publish now. */
  publish: T | null;
  /** Read the point on demand: its heartbeat is due and nothing delivers it. */
  read: boolean;
}

/** The reporting state of one point. */
/** How long a point stays on source time after its last replayed reading, when no live reading
 * ends the replay first (§5.3), in milliseconds. */
export const REPLAY_IDLE = 2000;

export class ReportState<T> {
  private origin: number;
  private last: { obs: Obs; quality: Quality } | null = null;
  private lastPublished: number | null = null;
  private lastRead: number | null = null;
  private unreadable = false;
  private pending: { item: T; obs: Obs } | null = null;
  private candidate: Candidate<T> | null = null;
  /** Runtime time of the last replayed reading while the point is on source time; null on the
   * runtime clock. */
  private replayLastOffer: number | null = null;
  /** Source time of the last reading published during a replay, to resume from. */
  private lastPublishedSrc: number | null = null;

  constructor(
    readonly policy: ReportPolicy,
    now: number,
  ) {
    this.origin = now;
  }

  /** Forget everything, so the next reading is published. */
  reset(now: number): void {
    this.origin = now;
    this.last = null;
    this.lastPublished = null;
    this.lastRead = null;
    this.unreadable = false;
    this.pending = null;
    this.candidate = null;
    this.replayLastOffer = null;
    this.lastPublishedSrc = null;
  }

  /** A heartbeat read returned nothing for this point: it cannot be read on demand. */
  noData(): void {
    this.unreadable = true;
  }

  private sincePublished(now: number): number {
    return Math.max(0, now - (this.lastPublished ?? this.origin));
  }

  private doPublish(item: T, obs: Obs, quality: Quality, now: number): T {
    this.last = { obs, quality };
    this.lastPublished = now;
    this.pending = null;
    this.candidate = null;
    return item;
  }

  /** Offer a reading. Returns it when it is to be published now; otherwise it is dropped or held. */
  offer(item: T, obs: Obs, quality: Quality, now: number): T | null {
    if (this.replayLastOffer !== null) this.handOver(now);
    return this.offerAt(item, obs, quality, now);
  }

  /** Offer a reading replayed from the source's history, with its source time `src`. Intervals
   * are measured on source time; returns the readings to publish now, in order (§5.3 rule 7). */
  offerReplayed(item: T, obs: Obs, quality: Quality, src: number, now: number): T[] {
    if (this.replayLastOffer === null) {
      this.lastPublished = this.lastPublishedSrc;
      if (this.candidate) this.candidate.since = src;
    }
    this.replayLastOffer = now;
    const out: T[] = [];
    const released = this.release(src);
    if (released !== null) out.push(released);
    const offered = this.offerAt(item, obs, quality, src);
    if (offered !== null) out.push(offered);
    if (this.lastPublished !== null) this.lastPublishedSrc = this.lastPublished;
    return out;
  }

  /** End a replay: the timers of a held or debouncing reading restart on the runtime clock. */
  private handOver(now: number): void {
    this.replayLastOffer = null;
    if (this.lastPublished !== null) {
      this.lastPublishedSrc = this.lastPublished;
      this.lastPublished = now;
    }
    if (this.candidate) this.candidate.since = now;
  }

  private offerAt(item: T, obs: Obs, quality: Quality, now: number): T | null {
    const p = this.policy;
    if (!this.last) return this.doPublish(item, obs, quality, now);
    if (this.last.quality !== quality) return this.doPublish(item, obs, quality, now);
    if (p.maxInterval !== null && this.sincePublished(now) >= p.maxInterval) {
      return this.doPublish(item, obs, quality, now);
    }
    const changedFromLast = changed(p, obs, this.last.obs);

    if (p.debounce !== null) {
      if (!changedFromLast) {
        // Back at the published value: nothing settled or held is news any more.
        this.candidate = null;
        this.pending = null;
        return null;
      }
      const c = this.candidate;
      this.candidate = null;
      if (c && !changed(p, obs, c.first)) {
        c.latest = item;
        c.latestObs = obs;
        if (Math.max(0, now - c.since) < p.debounce) {
          this.candidate = c;
          return null;
        }
        item = c.latest;
        obs = c.latestObs;
      } else {
        this.candidate = { first: obs, since: now, latest: item, latestObs: obs };
        return null;
      }
    }

    if (p.minInterval !== null && this.sincePublished(now) < p.minInterval) {
      this.pending = { item, obs };
      return null;
    }
    // A newer reading supersedes anything held, whether or not it is published itself.
    this.pending = null;
    if (filtersChanges(p) && !changed(p, obs, this.last.obs)) return null;
    return this.doPublish(item, obs, quality, now);
  }

  /** A pass of the runtime loop. */
  due(now: number, pushed: boolean): Due<T> {
    if (this.replayLastOffer !== null) {
      // Replayed readings release what they hold themselves, and no heartbeat read is due.
      if (Math.max(0, now - this.replayLastOffer) < REPLAY_IDLE) return { publish: null, read: false };
      this.handOver(now);
    }
    const p = this.policy;
    const out = this.release(now);
    let read = false;
    if (p.maxInterval !== null) {
      const reference = Math.max(this.lastPublished ?? this.origin, this.lastRead ?? this.origin);
      if (pushed && !this.unreadable && Math.max(0, now - reference) >= p.maxInterval) {
        this.lastRead = now;
        read = true;
      }
    }
    return { publish: out, read };
  }

  /** A held reading whose interval ended, or a debounced one that settled, by `now`. */
  private release(now: number): T | null {
    const p = this.policy;
    let out: T | null = null;
    if (p.minInterval !== null && this.pending) {
      if (this.sincePublished(now) >= p.minInterval) {
        const { item, obs } = this.pending;
        this.pending = null;
        out = this.publishIfChanged(item, obs, now);
      }
    }
    if (out === null && p.debounce !== null && this.candidate) {
      const c = this.candidate;
      if (Math.max(0, now - c.since) >= p.debounce) {
        this.candidate = null;
        if (p.minInterval !== null && this.sincePublished(now) < p.minInterval) {
          this.pending = { item: c.latest, obs: c.latestObs };
        } else {
          out = this.publishIfChanged(c.latest, c.latestObs, now);
        }
      }
    }
    return out;
  }

  private publishIfChanged(item: T, obs: Obs, now: number): T | null {
    if (!this.last) return null;
    if (filtersChanges(this.policy) && !changed(this.policy, obs, this.last.obs)) return null;
    return this.doPublish(item, obs, this.last.quality, now);
  }
}
