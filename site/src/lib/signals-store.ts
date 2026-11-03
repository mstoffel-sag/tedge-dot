// The config workbench's shared state: the simulated signals, the connector configuration that
// reads them, and the run length. The signals, config-builder and outcome
// islands on the page all import this module, so they share one instance.

import { useEffect, useState } from 'preact/hooks';
import type { Config } from './config-builder';
import { configFor, type SimSignal } from './gateway';
import type { SignalSpec } from './playground';

export interface PlaygroundState {
  durationMs: number;
  sims: SimSignal[];
  config: Config;
  /** Id of the point the report chart shows. */
  selected: string;
}

export const MAX_SIGNALS = 4;

/** Signals and configuration made from simulation specs (the examples are written as specs). */
export function stateOf(protocol: string, specs: SignalSpec[], durationMs: number, batch?: Record<string, unknown>): PlaygroundState {
  return {
    durationMs,
    sims: specs.map((s) => ({ id: s.id, shape: s.shape, outage: s.outage })),
    config: configFor(protocol, specs, batch),
    selected: specs[0]?.id ?? '',
  };
}

export const DEFAULT_STATE: PlaygroundState = stateOf(
  'opcua',
  [
    { id: 'temperature', shape: 'noisy', source: 'polled', interval: '1s', outage: 'none', report: { deadband: 0.5 } },
    { id: 'setpoint', shape: 'step', source: 'polled', interval: '1s', outage: 'none', report: { on_change: true, max_interval: '2m' } },
    { id: 'pressure', shape: 'spike', source: 'pushed', interval: '500ms', outage: 'short', report: {} },
  ],
  600_000,
);

let state: PlaygroundState = DEFAULT_STATE;
const listeners = new Set<(s: PlaygroundState) => void>();

export function setState(next: PlaygroundState | ((s: PlaygroundState) => PlaygroundState)) {
  state = typeof next === 'function' ? next(state) : next;
  for (const l of listeners) l(state);
}

/**
 * The shared state, re-rendering the component when it changes. An island hydrates with the
 * state the server rendered, then takes the current one: another island may have changed it.
 */
export function usePlayground(): [PlaygroundState, typeof setState] {
  const [s, set] = useState(DEFAULT_STATE);
  useEffect(() => {
    listeners.add(set);
    set(state);
    return () => {
      listeners.delete(set);
    };
  }, []);
  return [s, setState];
}
