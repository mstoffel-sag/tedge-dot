// Time axes shared by the playground charts.

/** About `count` ticks on round times within [from, from + span]. */
export function timeTicks(span: number, count: number, from = 0): number[] {
  const steps = [100, 200, 500, 1000, 2000, 5000, 10_000, 15_000, 30_000, 60_000, 120_000, 300_000];
  const step = steps.find((s) => span / s <= count) ?? 600_000;
  const out: number[] = [];
  for (let t = Math.ceil(from / step) * step; t <= from + span + 1e-6; t += step) out.push(t);
  return out;
}

/** m:ss, or m:ss.mmm with `ms` when the time is not on a whole second. */
export function fmtTime(t: number, ms = false): string {
  const s = Math.floor(t / 1000);
  const base = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  const rest = Math.round(t % 1000);
  return ms && rest ? `${base}.${String(rest).padStart(3, '0')}` : base;
}
