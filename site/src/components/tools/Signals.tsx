/** @jsxImportSource preact */
// The config workbench's first step: the signals the connector reads, each with a behaviour and
// an optional outage. Each signal is read by the config point with the same id; adding, renaming
// or removing a signal edits that point in the configuration below (src/lib/gateway.ts).

import { EXAMPLES } from '../../lib/batching';
import { addPoint, removePoint, renamePoint, resolve, reunitPoint, type SimSignal } from '../../lib/gateway';
import { OUTAGES, SIGNALS, type Outage, type SignalId } from '../../lib/playground';
import { MAX_SIGNALS, stateOf, usePlayground } from '../../lib/signals-store';
import { HELP } from '../../lib/help';
import Help from './Help';
import './report-playground.css';
import './batch-playground.css';

export default function Signals() {
  const [state, setState] = usePlayground();
  const { sims: signals, config } = state;
  const resolved = resolve(config, signals);

  const edit = (i: number, patch: Partial<SimSignal>) =>
    setState((s) => {
      const before = s.sims[i];
      const after = { ...before, ...patch };
      let next = s.config;
      if (patch.id !== undefined && patch.id !== before.id) next = renamePoint(next, before.id, patch.id);
      if (patch.shape !== undefined) next = reunitPoint(next, after);
      return {
        ...s,
        config: next,
        sims: s.sims.map((x, k) => (k === i ? after : x)),
        selected: s.selected === before.id ? after.id : s.selected,
      };
    });
  const add = () =>
    setState((s) => {
      const shape = SIGNALS.find((x) => !s.sims.some((y) => y.shape === x.id)) ?? SIGNALS[0];
      const id = s.sims.some((y) => y.id === shape.name) ? `${shape.name}_${s.sims.length + 1}` : shape.name;
      const signal: SimSignal = { id, shape: shape.id, outage: 'none' };
      return { ...s, sims: [...s.sims, signal], config: addPoint(s.config, signal), selected: id };
    });
  const remove = (i: number) =>
    setState((s) => {
      const gone = s.sims[i];
      const sims = s.sims.filter((_, k) => k !== i);
      return { ...s, sims, config: removePoint(s.config, gone.id), selected: s.selected === gone.id ? sims[0]?.id ?? '' : s.selected };
    });

  return (
    <div class="td-tool not-content rp eq">
      <div class="bp-examples" role="group" aria-label="Examples">
        <span class="bp-examples-label">Start from an example (it replaces the signals and the configuration):<Help text={HELP['workbench.example']} label="examples" /></span>
        {EXAMPLES.map((e) => (
          <button type="button" class="td-btn" onClick={() => setState(stateOf(e.protocol, e.signals, e.durationMs))}>
            {e.label}
          </button>
        ))}
      </div>

      <ul class="eq-signals" aria-label="Signals">
        {signals.map((s, i) => {
          const k = resolved.specs.findIndex((x) => x.id === s.id);
          const spec = resolved.specs[k];
          return (
            <li class="td-panel eq-signal">
              <label class="td-field">
                <span>Signal<Help text={HELP['workbench.signal']} label="Signal" /></span>
                <input type="text" value={s.id} spellcheck={false} onInput={(e) => edit(i, { id: (e.currentTarget as HTMLInputElement).value })} />
              </label>
              <label class="td-field">
                <span>Behaviour<Help text={HELP['workbench.behaviour']} label="Behaviour" /></span>
                <select value={s.shape} onChange={(e) => edit(i, { shape: (e.currentTarget as HTMLSelectElement).value as SignalId })}>
                  {SIGNALS.map((x) => <option value={x.id}>{x.label}{x.unit ? ` (${x.unit})` : ''}</option>)}
                </select>
              </label>
              <label class="td-field">
                <span>Outage (reads fail)<Help text={HELP['workbench.outage']} label="Outage" /></span>
                <select value={s.outage} onChange={(e) => edit(i, { outage: (e.currentTarget as HTMLSelectElement).value as Outage })}>
                  {OUTAGES.map((o) => <option value={o.id}>{o.label}</option>)}
                </select>
              </label>
              <p class={`eq-status ${spec ? '' : 'warn'}`}>
                {spec ? (
                  spec.source === 'pushed' ? (
                    <>Point <code>{s.id}</code> subscribes: the server samples it every {spec.interval} and sends changes ({resolved.origins[k].interval}).</>
                  ) : (
                    <>Point <code>{s.id}</code> polls it every {spec.interval} ({resolved.origins[k].interval}).</>
                  )
                ) : (
                  <>Not read: no point in the configuration has the id <code>{s.id}</code>.</>
                )}
              </p>
              {signals.length > 1 && (
                <button type="button" class="td-btn eq-remove" onClick={() => remove(i)} aria-label={`Remove ${s.id}`}>Remove</button>
              )}
            </li>
          );
        })}
      </ul>
      <div class="eq-actions">
        {signals.length < MAX_SIGNALS && <button type="button" class="td-btn" onClick={add}>+ Add signal</button>}
        {resolved.orphans.length > 0 && (
          <p class="eq-status warn">
            {resolved.orphans.map((id, i) => <>{i ? ', ' : ''}<code>{id || '(no id)'}</code></>)}{' '}
            {resolved.orphans.length === 1 ? 'is a point' : 'are points'} with no signal of that name: the workbench has nothing to read for{' '}
            {resolved.orphans.length === 1 ? 'it' : 'them'}.
          </p>
        )}
      </div>
    </div>
  );
}
