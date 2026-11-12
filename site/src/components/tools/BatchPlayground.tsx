/** @jsxImportSource preact */
// The grouping playground: what the configured points publish, grouped by the configuration's
// `grouping` policy into thin-edge measurements, and what Cumulocity receives. Each measurement
// is drawn with one cell per signal, so a signal missing from it shows. The `grouping` form here
// edits `[connector] grouping` of the shared configuration, and the fragment layout the points'
// `meta.measurement`. The engine is src/lib/batch.ts, which runs the contract's grouped-output
// vectors in CI.

import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import {
  AGGREGATES,
  c8yMeasurement,
  connectorToml,
  EVERY,
  FRAGMENT_LAYOUTS,
  fragmentWrites,
  groupingTable,
  layoutOf,
  POLICIES,
  seriesOf,
  simulate,
  tedgeMeasurement,
  WINDOWS,
  withLayout,
  type GroupingForm,
  type GroupRun,
  type Measurement,
} from '../../lib/batching';
import { fmtTime, timeTicks } from '../../lib/chart';
import { setAt } from '../../lib/config-builder';
import { expandLibraries, formOfGrouping, hasOwnGrouping, resolve, withGrouping } from '../../lib/gateway';
import { outageWindow, simulateSignal } from '../../lib/playground';
import { usePlayground } from '../../lib/signals-store';
import { HELP } from '../../lib/help';
import Help from './Help';
import './batch-playground.css';

const COLORS = 6;
const LANE = 34;
const PAD = { top: 8, right: 16, bottom: 26, left: 108 };
const SPANS = [
  { label: '5 s', ms: 5_000 },
  { label: '10 s', ms: 10_000 },
  { label: '30 s', ms: 30_000 },
  { label: '1 min', ms: 60_000 },
  { label: 'whole run', ms: Infinity },
];
const MAX_ROWS = 300;

export default function BatchPlayground() {
  const [state, setState] = usePlayground();
  const { durationMs } = state;
  // What the connector loads: library points collected, inline points applied over them.
  const config = useMemo(() => expandLibraries(state.config), [state.config]);
  const signals = useMemo(() => resolve(config, state.sims).specs, [JSON.stringify(config), JSON.stringify(state.sims)]);
  // The durations a policy does not use are kept here, so switching back restores them.
  const [memory, setMemory] = useState<GroupingForm>({ by: 'time', window: '500ms', every: '5s', aggregate: 'none' });
  const form = formOfGrouping(config.connector, memory);
  // "measurements" is the deprecated name of "group".
  const groupedOutput = config.connector?.output === 'group' || config.connector?.output === 'measurements';
  const ownGrouping = hasOwnGrouping(config);
  const layout = layoutOf(state.config);
  const type: string = config.connector?.measurement?.type || config.connector?.measurement?.group || config.connector?.protocol || '';
  const [spanMs, setSpanMs] = useState(30_000);
  const [start, setStart] = useState(0);
  const [selected, setSelected] = useState(0);
  const chartRef = useRef<HTMLDivElement>(null);
  const [W, setW] = useState(760);
  useEffect(() => {
    const el = chartRef.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => setW(Math.max(300, Math.round(e.contentRect.width))));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const reports = useMemo(() => signals.map((s) => simulateSignal(s, durationMs)), [JSON.stringify(signals), durationMs]);
  const compare = useMemo(
    () => POLICIES.map((p) => ({ id: p.id, run: simulate(signals, durationMs, withGrouping(config, { ...form, by: p.id }), reports) })),
    [reports, JSON.stringify(config), form.window, form.every, form.aggregate],
  );
  // The detail runs the configuration as it is, with any device or point `grouping` tables.
  const run = useMemo(
    () => (ownGrouping ? simulate(signals, durationMs, config, reports) : compare.find((c) => c.id === form.by)!.run),
    [compare, ownGrouping],
  );

  const span = Math.min(spanMs, durationMs);
  const from = Math.max(0, Math.min(start, durationMs - span));
  const to = from + span;
  useEffect(() => setSelected(0), [form.by, from, span, JSON.stringify(signals)]);
  const inView = (t: number) => t >= from && t <= to;
  const visible = run.measurements.map((m, i) => ({ m, i })).filter(({ m }) => inView(m.time));
  const sel = visible[Math.min(selected, visible.length - 1)];

  const narrow = W < 520;
  const left = narrow ? 98 : PAD.left;
  const x = (t: number) => left + ((t - from) / span) * (W - left - PAD.right);
  const N = signals.length;
  const set = <K extends keyof GroupingForm>(key: K, value: GroupingForm[K]) => {
    const next = { ...form, [key]: value };
    setMemory(next);
    setState((s) => {
      // The deprecated `batch` gives way to the `grouping` written here.
      const { batch: _, ...connector } = s.config.connector ?? {};
      return { ...s, config: setAt({ ...s.config, connector }, '/connector/grouping', groupingTable(next)) };
    });
  };
  const setLayout = (id: 'type' | 'signal') => setState((s) => ({ ...s, config: withLayout(s.config, id) }));
  const policyName = (id: string) => (id === 'frame' ? `frame ${form.window}` : id === 'sample' ? `sample ${form.every}` : 'time');
  const fmt = (v: number) => String(Number(v.toFixed(3)));
  const ticks = timeTicks(span, narrow ? 3 : 6, from);

  // Detail chart geometry: a lane per signal, then the measurements.
  const laneY = (s: number) => PAD.top + s * LANE + LANE / 2;
  const cell = 6;
  const outY = PAD.top + N * LANE + 30;
  const H = outY + (N * (cell + 1)) / 2 + 10 + PAD.bottom;
  const visibleX = spread(visible.map(({ m }) => x(m.time)), 12);
  const owner = new Map<number, number>();
  run.measurements.forEach((m, i) => m.from.forEach((r) => owner.has(r) || owner.set(r, i)));
  const withheld = (s: number) => {
    const rep = reports[s];
    const pub = new Set(rep.published.map((p) => p.index));
    return rep.readings.filter((r, i) => !pub.has(i) && inView(r.t));
  };
  const tooMany = visible.length > 400;

  // Compare strip geometry.
  const rowH = Math.max(26, N * 5 + 10);
  const rowY = (i: number) => 22 + i * rowH + rowH / 2;
  const CH = 22 + POLICIES.length * rowH + 22;

  return (
    <div class="td-tool not-content bp">
      {!groupedOutput && (
        <p class="td-panel bp-notice">
          The configuration publishes samples (<code>output = "{config.connector?.output ?? 'single'}"</code>): every published reading is its own message, and
          nothing is grouped. The view below shows what <code>output = "group"</code> would send.{' '}
          <button type="button" class="td-btn" onClick={() => setState((s) => ({ ...s, config: setAt(s.config, '/connector/output', 'group') }))}>
            Set output = "group"
          </button>
        </p>
      )}
      {signals.length === 0 && <p class="td-panel bp-notice">No point of the configuration reads a signal.</p>}

      <fieldset class="td-panel bp-fieldset">
        <legend><code>grouping</code> on <code>[connector]</code><Help text={HELP['grouping.by']} label="by" /></legend>
        <div class="bp-choices" role="radiogroup" aria-label="by">
          {POLICIES.map((p) => (
            <button type="button" role="radio" aria-checked={form.by === p.id} class="td-btn" onClick={() => set('by', p.id)}><code>{p.id}</code></button>
          ))}
        </div>
        <div class="bp-row">
          <Select label="window" note="frame" value={form.window} options={WINDOWS} onChange={(v) => set('window', v)} />
          <Select label="every" note="sample" value={form.every} options={EVERY} onChange={(v) => set('every', v)} />
          <Select
            label="aggregate"
            note="sample"
            value={form.aggregate}
            options={AGGREGATES.map((a) => a.id)}
            names={Object.fromEntries(AGGREGATES.map((a) => [a.id, a.label]))}
            onChange={(v) => set('aggregate', v)}
          />
        </div>
        <small>{POLICIES.find((p) => p.id === form.by)!.text}</small>
        {ownGrouping && <small>Some devices or points set their own <code>grouping</code> in the configuration; the chart below runs them as configured, the strip compares the policies for every point.</small>}
        <div class="bp-choices" role="radiogroup" aria-label="Fragments">
          <span>Fragments of <code>{type}</code><Help text={HELP['workbench.fragments']} label="Fragments" /></span>
          {FRAGMENT_LAYOUTS.map((l) => (
            <button type="button" role="radio" aria-checked={layout === l.id} class="td-btn" onClick={() => setLayout(l.id)}>{l.label}</button>
          ))}
        </div>
        <small>
          {layout === 'custom'
            ? <>The points name their own fragments in <code>meta.measurement</code>; the views run them as configured.</>
            : FRAGMENT_LAYOUTS.find((l) => l.id === layout)!.text}
        </small>
        <pre class="rp-toml bp-toml" aria-label="The grouping settings in TOML"><code>{connectorToml(form, type, layout)}</code></pre>
      </fieldset>

      <div class="bp-view" role="group" aria-label="Part of the run to show">
        <label class="td-field bp-select">
          <span>Show<Help text={HELP['workbench.span']} label="Show" /></span>
          <select value={String(spanMs)} onChange={(e) => setSpanMs(Number((e.currentTarget as HTMLSelectElement).value))}>
            {SPANS.filter((s) => s.ms < durationMs || s.ms === Infinity).map((s) => <option value={String(s.ms)}>{s.label}</option>)}
          </select>
        </label>
        {span < durationMs && (
          <label class="td-field bp-range">
            <span>from {fmtTime(from)} to {fmtTime(to)}<Help text={HELP['workbench.from']} label="part of the run" /></span>
            <input type="range" min={0} max={durationMs - span} step={Math.max(100, span / 20)} value={from} onInput={(e) => setStart(Number((e.currentTarget as HTMLInputElement).value))} />
          </label>
        )}
      </div>

      <figure class="bp-compare">
        <figcaption>
          The same samples, three ways to publish them ({fmtTime(from)} to {fmtTime(to)}). Each mark is a measurement with one cell per
          signal, in the order of the configured points: a hollow cell is a signal missing from that measurement.
        </figcaption>
        <div ref={chartRef}>
          <svg viewBox={`0 0 ${W} ${CH}`} role="img" aria-label="The samples and the measurements each grouping policy makes of them">
            <text x={left - 8} y={10} class="bp-label" text-anchor="end" dominant-baseline="middle">samples</text>
            <line x1={left} x2={W - PAD.right} y1={10} y2={10} class="rp-grid-line" />
            {!tooMany && run.inputs.map((r) => (inView(r.ts) && r.quality === 'good' ? <circle cx={x(r.ts)} cy={10} r={2.5} class="bp-input" /> : null))}
            {compare.map((c, i) => {
              const y = rowY(i);
              const ms = c.run.measurements.filter((m) => inView(m.time));
              return (
                <g class={`bp-row-g ${c.id === form.by ? 'on' : ''}`} onClick={() => set('by', c.id)}>
                  <rect x={0} y={y - rowH / 2 + 1} width={W} height={rowH - 2} class="bp-row-bg" />
                  <text x={left - 8} y={y} class="bp-label" text-anchor="end" dominant-baseline="middle">{narrow ? c.id : policyName(c.id)}</text>
                  <line x1={left} x2={W - PAD.right} y1={y} y2={y} class="rp-grid-line" />
                  {c.run.windows.filter((w) => w.to >= from && w.from <= to).map((w) => (
                    <rect x={x(Math.max(from, w.from))} width={Math.max(1, x(Math.min(to, w.to)) - x(Math.max(from, w.from)))} y={y - rowH / 2 + 3} height={rowH - 6} class="bp-window" />
                  ))}
                  {c.run.samples.filter(inView).map((t) => <line x1={x(t)} x2={x(t)} y1={y - rowH / 2 + 2} y2={y + rowH / 2 - 2} class="bp-sample" />)}
                  {ms.length <= 600 && spread(ms.map((m) => x(m.time)), 6).map((px, j) => <Cells x={px} y={y} m={ms[j]} size={4} w={5} color={j} />)}
                  {ms.length > 600 && <text x={(left + W) / 2} y={y} class="rp-axis" text-anchor="middle" dominant-baseline="middle">{ms.length} measurements: show a shorter part</text>}
                </g>
              );
            })}
            {ticks.map((t) => <text x={x(t)} y={CH - 6} class="rp-axis" text-anchor="middle">{fmtTime(t, true)}</text>)}
          </svg>
        </div>
        <ul class="bp-compare-sum">
          {compare.map((c) => {
            const ms = c.run.measurements;
            return (
              <li>
                <button type="button" class="td-btn" aria-pressed={c.id === form.by} onClick={() => set('by', c.id)}>
                  <code>{policyName(c.id)}</code>
                  <span>{ms.length} measurement{ms.length === 1 ? '' : 's'} in the run; {c.run.missing} time{c.run.missing === 1 ? '' : 's'} a signal is missing from one</span>
                  <span>
                    {c.run.dropped.length ? `${c.run.dropped.length} sample${c.run.dropped.length === 1 ? '' : 's'} not sent` : 'every sample sent'}
                    {c.run.maxShift ? `, times moved up to ${c.run.maxShift} ms` : c.id === 'sample' ? ', stamped with the sample time' : ', exact times'}
                  </span>
                </button>
              </li>
            );
          })}
        </ul>
      </figure>

      <Stats run={run} />

      <div class="bp-chart">
        <svg viewBox={`0 0 ${W} ${H}`} role="img" aria-label={`Timeline of ${policyName(form.by)}: ${run.inputs.length} samples of ${N} signals become ${run.measurements.length} measurements.`}>
          {run.windows.filter((w) => w.to >= from && w.from <= to).map((w) => (
            <rect x={x(Math.max(from, w.from))} width={Math.max(1, x(Math.min(to, w.to)) - x(Math.max(from, w.from)))} y={PAD.top} height={N * LANE} class="bp-window" />
          ))}
          {run.samples.filter(inView).map((t) => <line x1={x(t)} x2={x(t)} y1={PAD.top} y2={outY} class="bp-sample" />)}
          {signals.map((s, i) => {
            const o = outageWindow(s.outage, durationMs);
            return (
              <g>
                {o && o.to >= from && o.from <= to && (
                  <rect x={x(Math.max(from, o.from))} width={x(Math.min(to, o.to)) - x(Math.max(from, o.from))} y={laneY(i) - LANE / 2 + 2} height={LANE - 4} class="rp-outage" />
                )}
                <line x1={left} x2={W - PAD.right} y1={laneY(i)} y2={laneY(i)} class="rp-grid-line" />
                <text x={left - 8} y={laneY(i)} class="bp-label" text-anchor="end" dominant-baseline="middle">{s.id}</text>
              </g>
            );
          })}
          <line x1={left} x2={W - PAD.right} y1={outY} y2={outY} class="rp-grid-line" />
          <text x={left - 8} y={outY} class="bp-label bp-label-strong" text-anchor="end" dominant-baseline="middle">{narrow ? 'sent' : 'measurements'}</text>

          {!tooMany &&
            signals.map((_, s) =>
              withheld(s).map((r) =>
                r.quality === 'good' ? (
                  <circle cx={x(r.t)} cy={laneY(s)} r={2} class="bp-withheld" />
                ) : (
                  <path d={`M${x(r.t) - 2},${laneY(s) - 2}l4,4m0,-4l-4,4`} class="bp-bad" />
                ),
              ),
            )}
          {!tooMany &&
            visible.map(({ m, i }, j) =>
              m.from.map((r) => {
                const input = run.inputs[r];
                return <line x1={x(input.ts)} y1={laneY(input.signal)} x2={visibleX[j]} y2={outY} class={`bp-link c${i % COLORS} ${sel?.i === i ? 'on' : ''}`} />;
              }),
            )}
          {!tooMany &&
            run.inputs.map((r, k) => {
              if (!inView(r.ts)) return null;
              if (r.quality !== 'good') {
                return (
                  <path d={`M${x(r.ts) - 3},${laneY(r.signal) - 3}l6,6m0,-6l-6,6`} class="bp-bad bp-bad-pub">
                    <title>{`${signals[r.signal].id}: read failed at ${fmtTime(r.ts, true)}, published with quality "bad"`}</title>
                  </path>
                );
              }
              const m = owner.get(k);
              return (
                <circle
                  cx={x(r.ts)}
                  cy={laneY(r.signal)}
                  r={4}
                  class={m === undefined ? 'bp-dropped' : `bp-reading c${m % COLORS} ${sel?.i === m ? 'on' : ''}`}
                  onClick={() => {
                    const v = visible.findIndex((x) => x.i === m);
                    if (v >= 0) setSelected(v);
                  }}
                >
                  <title>{`${signals[r.signal].id} = ${r.value} at ${fmtTime(r.ts, true)}${m === undefined ? ', not sent' : `, in measurement ${m + 1}`}`}</title>
                </circle>
              );
            })}
          {!tooMany &&
            visible.map(({ m, i }, j) => (
              <g class={`bp-msg ${sel?.i === i ? 'on' : ''}`} onClick={() => setSelected(j)}>
                <Cells x={visibleX[j]} y={outY} m={m} size={cell} w={9} color={i} />
                <title>{`Measurement ${i + 1} at ${fmtTime(m.time, true)}: ${describe(m, run)}`}</title>
              </g>
            ))}
          {tooMany && <text x={(left + W) / 2} y={outY - 20} class="rp-axis" text-anchor="middle">{visible.length} measurements in view: show a shorter part of the run.</text>}
          {ticks.map((t) => <text x={x(t)} y={H - 6} class="rp-axis" text-anchor="middle">{fmtTime(t, true)}</text>)}
        </svg>
      </div>
      <ul class="rp-legend bp-legend" aria-label="Legend">
        <li><svg width="12" height="12" aria-hidden="true"><circle cx="6" cy="6" r="4" class="bp-reading c0" /></svg>Sample, coloured by its measurement</li>
        <li><svg width="12" height="12" aria-hidden="true"><circle cx="6" cy="6" r="4" class="bp-dropped" /></svg>Sample not sent</li>
        <li><svg width="12" height="12" aria-hidden="true"><circle cx="6" cy="6" r="1.5" class="bp-withheld" /></svg>Reading held back by <code>report</code></li>
        <li><svg width="12" height="12" aria-hidden="true"><path d="M3,3l6,6m0,-6l-6,6" class="bp-bad bp-bad-pub" /></svg>Read failed</li>
        <li><svg width="12" height="16" aria-hidden="true"><rect x="2" y="1" width="8" height="6" class="bp-cell c0" /><rect x="2" y="9" width="8" height="6" class="bp-cell-missing c0" /></svg>Measurement: a cell per signal, hollow when missing</li>
        {form.by === 'frame' && <li><svg width="16" height="12" aria-hidden="true"><rect width="16" height="12" class="bp-window" /></svg>Frame window</li>}
        {form.by === 'sample' && <li><svg width="16" height="12" aria-hidden="true"><line x1="8" x2="8" y1="0" y2="12" class="bp-sample" /></svg>Sample time</li>}
      </ul>

      <div class="bp-out">
        <details class="bp-table rp-table">
          <summary>
            {visible.length} measurement{visible.length === 1 ? '' : 's'} on <code>{run.topic}</code>, {fmtTime(from)} to {fmtTime(to)}, as a table
            {visible.length > MAX_ROWS && ` (first ${MAX_ROWS})`}
          </summary>
          <table>
            <thead>
              <tr>
                <th>#</th>
                <th><code>time</code></th>
                {signals.map((s) => <th><code>{s.id}</code></th>)}
              </tr>
            </thead>
            <tbody>
              {visible.slice(0, MAX_ROWS).map(({ m, i }, j) => (
                <tr class={sel?.i === i ? 'on' : ''} onClick={() => setSelected(j)}>
                  <td><button type="button" class={`bp-swatch c${i % COLORS}`} aria-label={`Show measurement ${i + 1}`} aria-pressed={sel?.i === i} onClick={() => setSelected(j)}>{i + 1}</button></td>
                  <td>{fmtTime(m.time, true)}</td>
                  {signals.map((s, k) => (
                    <td class={m.has[k] ? '' : 'bp-missing'}>
                      {m.has[k]
                        ? seriesOf(m, run.names[k])
                            .map(([name, v]) => (name === run.names[k].series ? fmt(v) : `${name.slice(run.names[k].series.length + 1)} ${fmt(v)}`))
                            .join(', ')
                        : 'missing'}
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
          {visible.length === 0 && <p class="bp-empty">No measurement in this part of the run.</p>}
        </details>
        {sel && <Json m={sel.m} index={sel.i} run={run} />}
      </div>
    </div>
  );
}

function describe(m: Measurement, run: GroupRun): string {
  const ids = (has: boolean) => run.signals.filter((_, s) => m.has[s] === has).map((s) => s.id).join(', ');
  return m.has.includes(false) ? `${ids(true)}; missing ${ids(false)}` : ids(true);
}

/** A measurement mark: one cell per signal, filled when the measurement has it. */
function Cells(props: { x: number; y: number; m: Measurement; size: number; w: number; color: number }) {
  const { x, y, m, size, w } = props;
  const h = m.has.length * (size + 1) - 1;
  return (
    <g class={`c${props.color % COLORS}`}>
      {m.has.map((has, s) => (
        <rect x={x - w / 2} y={y - h / 2 + s * (size + 1)} width={w} height={size} rx={1} class={has ? 'bp-cell' : 'bp-cell-missing'} />
      ))}
    </g>
  );
}

function Stats({ run }: { run: GroupRun }) {
  const readings = run.reports.reduce((n, r) => n + r.readings.length, 0);
  const series = run.measurements.reduce((n, m) => n + Object.values(m.fragments).reduce((k, f) => k + Object.keys(f).length, 0), 0);
  return (
    <dl class="rp-stats bp-stats">
      <div><dt>Readings</dt><dd>{readings}</dd></div>
      <div><dt>Samples after report</dt><dd>{run.inputs.length}</dd></div>
      <div><dt>Measurements</dt><dd>{run.measurements.length}</dd></div>
      <div><dt>Series sent</dt><dd>{series}</dd></div>
      <div><dt>Fragments sent<Help text={HELP['workbench.fragment_writes']} label="Fragments sent" /></dt><dd>{fragmentWrites(run)}</dd></div>
      <div><dt>Signal missing</dt><dd>{run.missing}</dd></div>
      <div><dt>Samples not sent</dt><dd>{run.dropped.length}</dd></div>
    </dl>
  );
}

function Json({ m, index, run }: { m: Measurement; index: number; run: GroupRun }) {
  const missing = run.signals.filter((_, s) => !m.has[s]).map((s) => s.id);
  return (
    <div class="bp-json">
      <p>Measurement {index + 1} in Cumulocity, on the child device <code>{run.device}</code>:</p>
      <pre class="rp-toml"><code>{JSON.stringify(c8yMeasurement(m, run), null, 2)}</code></pre>
      {missing.length > 0 && (
        <p>
          Not in it: {missing.map((id, i) => <>{i ? ', ' : ''}<code>{id}</code></>)}. Cumulocity stores nothing for {missing.length === 1 ? 'that series' : 'those series'} at this time.
        </p>
      )}
      <p>From the connector, published at {fmtTime(m.publishedAt, true)} on <code>{`te/device/${run.device}///m/${m.type}`}</code>:</p>
      <pre class="rp-toml"><code>{JSON.stringify(tedgeMeasurement(m))}</code></pre>
    </div>
  );
}

function Select(props: { label: string; note: string; value: string; options: string[]; names?: Record<string, string>; onChange: (v: string) => void }) {
  return (
    <label class="td-field bp-select">
      <span><code>{props.label}</code> <small>({props.note})</small><Help text={HELP[`grouping.${props.label}`]} label={props.label} /></span>
      <select value={props.value} onChange={(e) => props.onChange((e.currentTarget as HTMLSelectElement).value)}>
        {props.options.map((o) => <option value={o}>{props.names?.[o] ?? `"${o}"`}</option>)}
      </select>
    </label>
  );
}

/** `xs` moved right where needed so that neighbours (in x order) are at least `gap` apart. */
function spread(xs: number[], gap: number): number[] {
  const order = xs.map((_, i) => i).sort((a, b) => xs[a] - xs[b]);
  const out = [...xs];
  let last = -Infinity;
  for (const i of order) {
    out[i] = Math.max(xs[i], last + gap);
    last = out[i];
  }
  return out;
}
