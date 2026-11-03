/** @jsxImportSource preact */
// A field's help: a small "?" button next to its label that shows a short text in a popup, on
// hover, on keyboard focus, or when tapped. The texts are src/lib/help.ts.

import { useEffect, useId, useRef, useState } from 'preact/hooks';
import './help.css';

export default function Help({ text, label }: { text: string | undefined; label: string }) {
  const [open, setOpen] = useState(false);
  const id = `td-help${useId()}`;
  const tip = useRef<HTMLSpanElement>(null);
  // Keep an opened popup inside the window: move it left when it would run off the right edge.
  useEffect(() => {
    const el = tip.current;
    if (!el || !open) return;
    el.style.removeProperty('--shift');
    const over = el.getBoundingClientRect().right - (document.documentElement.clientWidth - 8);
    if (over > 0) el.style.setProperty('--shift', `${-over}px`);
  }, [open]);
  if (!text) return null;
  return (
    <span class={`td-help ${open ? 'open' : ''}`} onMouseLeave={() => setOpen(false)}>
      <button
        type="button"
        class="td-help-btn"
        aria-label={`About ${label}`}
        aria-expanded={open}
        aria-describedby={id}
        onClick={(e) => {
          // Inside a <label>, a click must not reach the field.
          e.preventDefault();
          setOpen(!open);
        }}
        onBlur={() => setOpen(false)}
        onKeyDown={(e) => e.key === 'Escape' && setOpen(false)}
      >
        ?
      </button>
      <span role="tooltip" id={id} class="td-help-tip" ref={tip}>
        {text}
      </span>
    </span>
  );
}
