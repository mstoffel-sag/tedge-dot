/** @jsxImportSource preact */
// The package picker: a few questions about the gateway, and which of the two packages to
// install and why. The decision is src/lib/picker.ts over src/data/packages.yaml.

import { useState } from 'preact/hooks';
import { recommend, type Answers, type PackageData } from '../../lib/picker';
import { HELP } from '../../lib/help';
import Help from './Help';
import './package-picker.css';

const ARCHITECTURES = [
  { id: 'amd64', label: 'x86-64 (amd64)' },
  { id: 'arm64', label: '64-bit ARM (arm64 / aarch64)' },
  { id: 'armhf', label: '32-bit ARM (armhf / ARMv7)' },
];

export default function PackagePicker({ data }: { data: PackageData }) {
  const [a, setA] = useState<Answers>({
    architecture: 'arm64',
    distribution: 'debian11',
    protocols: ['modbus'],
    features: [],
    smallFootprint: false,
  });
  const r = recommend(data, a);
  const toggle = (key: 'protocols' | 'features', id: string) =>
    setA({ ...a, [key]: a[key].includes(id) ? a[key].filter((x) => x !== id) : [...a[key], id] });
  const rustOnly = Object.keys(data.features);

  return (
    <div class="td-tool not-content pp">
      <form class="td-panel pp-form" onSubmit={(e) => e.preventDefault()}>
        <label class="td-field">
          <span>CPU architecture<Help text={HELP['picker.architecture']} label="CPU architecture" /></span>
          <select value={a.architecture} onChange={(e) => setA({ ...a, architecture: (e.currentTarget as HTMLSelectElement).value })}>
            {ARCHITECTURES.map((x) => <option value={x.id}>{x.label}</option>)}
          </select>
        </label>
        <label class="td-field">
          <span>Operating system<Help text={HELP['picker.distribution']} label="Operating system" /></span>
          <select value={a.distribution} onChange={(e) => setA({ ...a, distribution: (e.currentTarget as HTMLSelectElement).value })}>
            {data.distributions.map((d) => <option value={d.id}>{d.label}</option>)}
          </select>
        </label>
        <fieldset class="pp-set">
          <legend>Protocols<Help text={HELP['picker.protocols']} label="Protocols" /></legend>
          {Object.entries(data.protocols).map(([id, label]) => (
            <label class="pp-check">
              <input type="checkbox" checked={a.protocols.includes(id)} onChange={() => toggle('protocols', id)} />
              {label}
              {data.preview_protocols?.includes(id) && <span class="pp-preview" title="Tested against simulators, not yet on real equipment">preview</span>}
            </label>
          ))}
        </fieldset>
        <fieldset class="pp-set">
          <legend>Do you need any of these?<Help text={HELP['picker.features']} label="features" /></legend>
          {rustOnly.map((id) => (
            <label class="pp-check">
              <input type="checkbox" checked={a.features.includes(id)} onChange={() => toggle('features', id)} />
              {data.features[id].label}
            </label>
          ))}
        </fieldset>
        <label class="pp-check">
          <input type="checkbox" checked={a.smallFootprint} onChange={() => setA({ ...a, smallFootprint: !a.smallFootprint })} />
          Storage or memory is tight (a few MB to spare)<Help text={HELP['picker.footprint']} label="footprint" />
        </label>
      </form>

      <section class="pp-result" aria-live="polite" aria-label="Recommendation">
        <p class="pp-kicker">Install</p>
        <p class="pp-pick"><code>{r.pick}</code></p>
        <ul>{r.reasons.map((x) => <li>{keepNames(x)}</li>)}</ul>
        {r.caveats.length > 0 && (
          <div class="pp-caveats">
            <strong>Not covered</strong>
            <ul>{r.caveats.map((x) => <li>{keepNames(x)}</li>)}</ul>
          </div>
        )}
        {r.alternative && <p class="pp-alt" dangerouslySetInnerHTML={{ __html: codeSpans(r.alternative) }} />}
        <pre class="pp-cmd"><code>sudo apt-get install -y {r.pick}</code></pre>
        <p class="pp-foot">
          Both packages install the same <code>/usr/bin/tedge-dot</code> and read the same configuration, so you can switch later.
        </p>
      </section>
    </div>
  );
}

// Package names keep their hyphens on one line (non-breaking hyphens, for display only).
function keepNames(s: string): string {
  return s.replace(/tedge-dot-(rs|c)\b/g, 'tedge\u2011dot\u2011$1');
}

// The alternative text is our own data; only backtick spans are turned into <code>.
function codeSpans(s: string): string {
  const esc = s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  return esc.replace(/`([^`]+)`/g, '<code>$1</code>').replace(/tedge-dot-(rs|c)\b/g, 'tedge&#8209;dot&#8209;$1');
}
