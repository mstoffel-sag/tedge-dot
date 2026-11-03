/** @jsxImportSource preact */
// The config workbench's first outcome: for each point that reads a simulated signal, its
// readings and what its effective `report` policy publishes. Signals and configuration are the
// page's shared state (src/lib/signals-store.ts); the batching view below
// batches what these points publish. The engine is src/lib/report.ts, which runs the contract's
// test vectors in CI.

import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { fmtTime, timeTicks } from '../../lib/chart';
import { getAt, setAt } from '../../lib/config-builder';
import { resolve, reunitPoint } from '../../lib/gateway';
import { checkTable, type ReportTable } from '../../lib/report';
import { DURATIONS, outageWindow, SIGNALS, simulateSignal, tableToToml, type SignalId } from '../../lib/playground';
import { usePlayground } from '../../lib/signals-store';
import { HELP } from '../../lib/help';
import Help from './Help';
import './report-playground.css';

/** Presets set the selected signal's behaviour and its point's own `report` table. */
const PRESETS: { label: string; shape: SignalId; report: ReportTable }[] = [
  { label: 'Deadband on a noisy signal', shape: 'noisy', report: { deadband: 0.5 } },
  { label: 'Heartbeat on a flat signal', shape: 'flat', report: { on_change: true, max_interval: '2m' } },
  { label: 'Rate limit a sine', shape: 'sine', report: { on_change: true, min_interval: '30s' } },
  { label: 'Debounce a flapping switch', shape: 'flapping', report: { on_change: true, debounce: '5s' } },
  { label: 'Percent deadband + heartbeat', shape: 'step', report: { deadband: '2%', max_interval: '3m' } },
];

// The chart is drawn at its real pixel width (measured), so its labels keep their size on a
// phone instead of shrinking with a scaled-down picture.
const PAD = { top: 16, right: 24, bottom: 28, left: 44 };

export default function ReportPlayground() {
  const [state, setState] = usePlayground();
  const { sims, config, durationMs } = state;
  const resolved = resolve(config, sims);
  const k = Math.max(0, resolved.specs.findIndex((x) => x.id === state.selected));
  const spec = resolved.specs[k];
  const [hover, setHover] = useState<number | null>(null);
  const svgRef = useRef<SVGSVGElement>(null);
  const chartRef = useRef<HTMLDivElement>(null);
  const [W, setW] = useState(760);
  useEffect(() => {
    const el = chartRef.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => setW(Math.max(280, Math.round(e.contentRect.width))));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  const H = W < 500 ? 220 : 260;

  const table = spec?.report ?? {};
  const error = checkTable(table as Record<string, unknown>);
  const run = useMemo(() => (error || !spec ? null : simulateSignal(spec, durationMs)), [JSON.stringify(spec), durationMs, error]);
  const unit = spec ? SIGNALS.find((s) => s.id === spec.shape)!.unit : '';
  // A point from a library gets an inline patch with its own `report` (contract §3.4).
  const points: any[] = config.device?.[0]?.point ?? [];
  const inline = points.findIndex((p) => p.id === spec?.id);
  const pointIndex = inline >= 0 ? inline : points.length;
  const own = inline >= 0 ? getAt(config, `/device/0/point/${inline}/report`) : undefined;

  const applyPreset = (p: (typeof PRESETS)[number]) =>
    setState((s) => {
      const sims = s.sims.map((e) => (e.id === spec.id ? { ...e, shape: p.shape } : e));
      const signal = sims.find((e) => e.id === spec.id)!;
      const patched = inline >= 0 ? s.config : setAt(s.config, `/device/0/point/${pointIndex}`, { id: spec.id });
      const config = reunitPoint(setAt(patched, `/device/0/point/${pointIndex}/report`, p.report), signal);
      return { ...s, sims, config };
    });

  if (!spec) {
    return (
      <div class="td-tool not-content rp">
        <p class="td-panel">No point of the configuration reads a signal, so there is nothing to show. Give a point the id of a signal.</p>
      </div>
    );
  }

  // Scales
  const good = run?.readings.filter((r) => r.quality === 'good') ?? [];
  const values = good.length ? good.map((r) => r.value) : [0, 1];
  let lo = Math.min(...values);
  let hi = Math.max(...values);
  if (hi - lo < 1) {
    lo -= 0.5;
    hi += 0.5;
  }
  const span = hi - lo;
  lo -= span * 0.08;
  hi += span * 0.08;
  // Snap the domain to whole ticks, so the top and bottom of the plot are labelled.
  const step = tickStep(lo, hi, 4);
  lo = Math.floor(lo / step) * step;
  hi = Math.ceil(hi / step) * step;
  const decimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  const tMax = durationMs;
  const x = (t: number) => PAD.left + (t / tMax) * (W - PAD.left - PAD.right);
  const y = (v: number) => PAD.top + (1 - (v - lo) / (hi - lo)) * (H - PAD.top - PAD.bottom);
  const yTicks = niceTicks(lo, hi, step);
  const xTicks = timeTicks(tMax, W < 500 ? 3 : 6);
  const outage = outageWindow(spec.outage, durationMs);

  const publishedMap = new Map(run?.published.map((p) => [p.index, p]) ?? []);
  const shown = run?.published.filter((p) => run.readings[p.index].quality === 'good') ?? [];
  const heartbeats = run?.published.filter((p) => p.heartbeat).length ?? 0;
  const held = run?.published.filter((p) => p.at > run.readings[p.index].t).length ?? 0;
  const readings = run?.readings.length ?? 0;
  const published = run?.published.length ?? 0;
  const saved = readings ? Math.round((1 - published / readings) * 100) : 0;
  const toml = tableToToml(table);

  const nearest = (t: number) => {
    if (!run || !run.readings.length) return null;
    let a = 0;
    let b = run.readings.length - 1;
    while (b - a > 1) {
      const m = (a + b) >> 1;
      if (run.readings[m].t < t) a = m;
      else b = m;
    }
    return Math.abs(run.readings[a].t - t) <= Math.abs(run.readings[b].t - t) ? a : b;
  };
  const onPointer = (e: PointerEvent) => {
    if (!svgRef.current) return;
    const rect = svgRef.current.getBoundingClientRect();
    const px = ((e.clientX - rect.left) / rect.width) * W;
    setHover(nearest(((px - PAD.left) / (W - PAD.left - PAD.right)) * tMax));
  };
  const onKey = (e: KeyboardEvent) => {
    if (!run) return;
    const n = e.shiftKey ? 10 : 1;
    if (e.key === 'ArrowRight') setHover(Math.min(run.readings.length - 1, (hover ?? -1) + n));
    else if (e.key === 'ArrowLeft') setHover(Math.max(0, (hover ?? 1) - n));
    else return;
    e.preventDefault();
  };

  // The line breaks where reads fail. A pushed signal reports changes only: it holds its value
  // until the next one (or the outage), so it is drawn as steps.
  const stepped = spec.source === 'pushed';
  let linePath = '';
  let pen: number | null = null;
  for (const r of run?.readings ?? []) {
    if (r.quality !== 'good') {
      if (stepped && pen !== null) linePath += `H${x(r.t).toFixed(1)}`;
      pen = null;
      continue;
    }
    if (pen === null) linePath += `M${x(r.t).toFixed(1)},${y(r.value).toFixed(1)}`;
    else if (stepped) linePath += `H${x(r.t).toFixed(1)}V${y(r.value).toFixed(1)}`;
    else linePath += `L${x(r.t).toFixed(1)},${y(r.value).toFixed(1)}`;
    pen = r.value;
  }
  if (stepped && pen !== null) linePath += `H${x(tMax).toFixed(1)}`;
  const hovered = hover !== null && run ? run.readings[hover] : null;
  const hoveredPub = hover !== null ? publishedMap.get(hover) : undefined;

  return (
    <div class="td-tool not-content rp">
      <div class="rp-signals">
        <div class="rp-tabs" role="tablist" aria-label="Points that read a signal">
        {resolved.specs.map((x) => (
          <button type="button" role="tab" aria-selected={x.id === spec.id} class="td-btn rp-signal" onClick={() => setState((st) => ({ ...st, selected: x.id }))}>
            <code>{x.id}</code>
            <small>{x.source} {x.interval}{x.outage !== 'none' ? ', outage' : ''}</small>
          </button>
        ))}
        </div>
        <label class="td-field rp-duration">
          <span>Run length<Help text={HELP['workbench.duration']} label="Run length" /></span>
          <select value={String(durationMs)} onChange={(e) => setState((s) => ({ ...s, durationMs: Number((e.currentTarget as HTMLSelectElement).value) }))}>
            {DURATIONS.map((d) => <option value={String(d.ms)}>{d.label}</option>)}
          </select>
        </label>
      </div>

      <div class="rp-presets" role="group" aria-label={`Presets for ${spec.id}`}>
        {PRESETS.map((p) => (
          <button type="button" class="td-btn" aria-pressed={p.shape === spec.shape && JSON.stringify(p.report) === JSON.stringify(own ?? null)} onClick={() => applyPreset(p)}>
            {p.label}
          </button>
        ))}
      </div>

      <div class="rp-grid">
        <div class="td-panel rp-form rp-policy" aria-label={`Effective policy of ${spec.id}`}>
          <p>Effective <code>report</code> of <code>{spec.id}</code>{resolved.origins[k].report.length ? `, merged from ${resolved.origins[k].report.join(', ')}` : ' (none set: every reading is published)'}:</p>
          {error ? <p class="td-error" role="alert">{error}</p> : <pre class="rp-toml" aria-label="The effective report table in TOML"><code>{tableToToml(table)}</code></pre>}
          <p>{spec.source === 'pushed' ? 'Subscribed' : 'Polled'} every {spec.interval}, from {resolved.origins[k].interval}.</p>
          <p>Change either in the <a href="#2-configuration">configuration</a>; a preset sets this point's own <code>report</code> and the signal's behaviour.</p>
        </div>

        <div class="rp-out">
          <dl class="rp-stats">
            <div><dt>Readings</dt><dd>{readings}</dd></div>
            <div><dt>Published</dt><dd>{published}</dd></div>
            <div><dt>Saved</dt><dd>{saved}%</dd></div>
            <div><dt>Heartbeats</dt><dd>{heartbeats}</dd></div>
          </dl>

          <ul class="rp-legend" aria-label="Legend">
            <li><svg width="22" height="10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" class="rp-line" /></svg>Reading</li>
            <li><svg width="12" height="12" aria-hidden="true"><circle cx="6" cy="6" r="4.5" class="rp-pub" /></svg>Published</li>
            <li><svg width="12" height="12" aria-hidden="true"><rect x="2" y="2" width="8" height="8" class="rp-hb" transform="rotate(45 6 6)" /></svg>Heartbeat</li>
            <li><svg width="22" height="10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" class="rp-delay" /></svg>Held, published later</li>
            {outage && <li><svg width="16" height="12" aria-hidden="true"><rect width="16" height="12" class="rp-outage" /></svg>Outage: reads fail</li>}
          </ul>

          <div class="rp-chart" ref={chartRef}>
            <svg
              ref={svgRef}
              viewBox={`0 0 ${W} ${H}`}
              role="img"
              aria-label={`Chart of ${spec.id}: ${readings} readings, ${published} published, ${heartbeats} heartbeats. Use the arrow keys to inspect readings.`}
              tabIndex={0}
              onPointerMove={onPointer as any}
              onPointerLeave={() => setHover(null)}
              onKeyDown={onKey as any}
              onBlur={() => setHover(null)}
            >
              {outage && <rect x={x(outage.from)} width={x(outage.to) - x(outage.from)} y={PAD.top} height={H - PAD.top - PAD.bottom} class="rp-outage" />}
              {yTicks.map((v) => (
                <g>
                  <line x1={PAD.left} x2={W - PAD.right} y1={y(v)} y2={y(v)} class="rp-grid-line" />
                  <text x={PAD.left - 6} y={y(v)} class="rp-axis" text-anchor="end" dominant-baseline="middle">{v.toFixed(decimals)}</text>
                </g>
              ))}
              {xTicks.map((t) => (
                <text x={x(t)} y={H - 8} class="rp-axis" text-anchor={t === 0 ? 'start' : t === tMax ? 'end' : 'middle'}>{fmtTime(t)}</text>
              ))}
              <path d={linePath} class="rp-line" />
              {shown.map((p) => {
                const r = run!.readings[p.index];
                return p.at > r.t ? <line x1={x(r.t)} x2={x(p.at)} y1={y(r.value)} y2={y(r.value)} class="rp-delay" /> : null;
              })}
              {shown.map((p) => {
                const r = run!.readings[p.index];
                return p.heartbeat ? (
                  <rect x={x(r.t) - 4} y={y(r.value) - 4} width="8" height="8" class="rp-hb" transform={`rotate(45 ${x(r.t)} ${y(r.value)})`} />
                ) : (
                  <circle cx={x(r.t)} cy={y(r.value)} r="4.5" class="rp-pub" />
                );
              })}
              {hovered && (
                <line x1={x(hovered.t)} x2={x(hovered.t)} y1={PAD.top} y2={H - PAD.bottom} class="rp-crosshair" />
              )}
            </svg>
            {hovered && (
              <div
                class={`rp-tip ${x(hovered.t) > W * 0.6 ? 'left' : 'right'}`}
                style={{ left: `${(x(hovered.t) / W) * 100}%`, top: `${PAD.top + 4}px` }}
                aria-live="polite"
              >
                <strong>{hovered.quality === 'good' ? `${fmt(hovered.value)}${unit ? ` ${unit}` : ''}` : 'read failed'}</strong>
                <span>at {fmtTime(hovered.t, true)}</span>
                <span>
                  {hoveredPub
                    ? hovered.quality !== 'good'
                      ? 'Published: quality changed to bad'
                      : hoveredPub.heartbeat
                        ? 'Published: heartbeat'
                        : hoveredPub.at > hovered.t
                          ? `Published at ${fmtTime(hoveredPub.at, true)} (held back)`
                          : 'Published'
                    : 'Not published'}
                </span>
              </div>
            )}
          </div>
          <p class="rp-note">
            {spec.source === 'pushed'
              ? `The server samples ${spec.id} every ${spec.interval} and pushes a value only when it changed.`
              : `Polled every ${spec.interval}. A heartbeat on a polled point is its next poll after max_interval.`}
            {held > 0 && ` ${held} reading${held === 1 ? ' was' : 's were'} held back and published once the interval ended, with ${held === 1 ? 'its' : 'their'} own timestamp.`}
          </p>

          <details class="rp-table">
            <summary>Published samples as a table</summary>
            <div class="rp-table-scroll">
              <table>
                <thead><tr><th>Reading time</th><th>Value</th><th>Published at</th><th>Why</th></tr></thead>
                <tbody>
                  {run?.published.slice(0, 500).map((p) => {
                    const r = run.readings[p.index];
                    return (
                      <tr>
                        <td>{fmtTime(r.t, true)}</td>
                        <td>{r.quality === 'good' ? fmt(r.value) : r.quality}</td>
                        <td>{fmtTime(p.at, true)}</td>
                        <td>{r.quality !== 'good' ? 'quality changed' : p.heartbeat ? 'heartbeat' : p.at > r.t ? 'held, then published' : 'change'}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </details>
        </div>
      </div>
    </div>
  );
}

function tickStep(lo: number, hi: number, count: number): number {
  const raw = (hi - lo) / count;
  const mag = 10 ** Math.floor(Math.log10(raw));
  return [1, 2, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
}

function niceTicks(lo: number, hi: number, step: number): number[] {
  const out: number[] = [];
  for (let v = Math.ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-6; v += step) out.push(Math.round(v * 1e6) / 1e6);
  return out;
}

function fmt(v: number): string {
  return Number.isInteger(v) ? String(v) : v.toFixed(2);
}
