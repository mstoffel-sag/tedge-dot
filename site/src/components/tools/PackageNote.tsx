/** @jsxImportSource preact */
// The config workbench's package line: which package runs the configuration, from the
// features it uses and the package data the package picker reads (src/data/packages.yaml).

import { packageNeeds } from '../../lib/gateway';
import type { PackageData } from '../../lib/picker';
import { usePlayground } from '../../lib/signals-store';
import './report-playground.css';

export default function PackageNote({ data }: { data: PackageData }) {
  const [state] = usePlayground();
  const needs = packageNeeds(state.config);
  const c = data.packages.find((p) => p.id === 'tedge-dot-c')!;
  const rsOnly = needs.features.filter((f) => !c.features.includes(f));
  const picker = <a href="/tedge-dot/tools/package-picker/">package picker</a>;
  return (
    <p class="td-panel not-content pn" aria-live="polite">
      {rsOnly.length ? (
        <>
          <strong>Package:</strong> this configuration uses {rsOnly.map((f) => data.features[f]?.label ?? f).join(', ')}, which only{' '}
          <code>tedge&#8209;dot&#8209;rs</code> has. The {picker} checks it against your gateway.
        </>
      ) : (
        <>
          <strong>Package:</strong> both packages run this configuration, with the same outcome; <code>tedge&#8209;dot&#8209;c</code> is the
          default. The {picker} chooses for your gateway.
        </>
      )}
    </p>
  );
}
