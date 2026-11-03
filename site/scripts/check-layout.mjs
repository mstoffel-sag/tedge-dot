#!/usr/bin/env node
// Layout check of the built site (dist/), in a real browser, at desktop and phone width:
//
//   - words glued to a link or inline code ("thenInto"), from a lost space in a template;
//   - content spilling out of its box (not counting scroll containers);
//   - buttons in one row with different heights or tops;
//   - sideways scrolling of the page;
//   - script errors;
//   - buttons that do nothing when clicked (checked at desktop width).
//
//   npm run build && npm run check:layout
//
// Uses Playwright's Chromium (`npx playwright install chromium`), or a local Chrome named by
// CHROME_PATH.

import { createReadStream, existsSync, readdirSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const site = fileURLToPath(new URL('..', import.meta.url));
// DIST: check another build directory than site/dist.
const dist = process.env.DIST ?? join(site, 'dist');
const PORT = 4499;
const BASE = `http://localhost:${PORT}/tedge-dot/`;

// Plural code names (`LinkReport`s) are written that way on purpose.
const GLUE_ALLOWED = [/^s\b/];

function pages() {
  const out = [];
  (function walk(dir) {
    for (const f of readdirSync(dir)) {
      const p = join(dir, f);
      if (statSync(p).isDirectory()) {
        if (!['_astro', 'pagefind', 'repo'].includes(f)) walk(p);
      } else if (f === 'index.html') out.push(p.slice(dist.length + 1).replace(/index\.html$/, ''));
    }
  })(dist);
  return out.sort();
}

// A static server for dist/ under /tedge-dot/ (astro preview allows one server per project).
const TYPES = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.woff2': 'font/woff2',
  '.xml': 'application/xml', '.wasm': 'application/wasm', '.txt': 'text/plain',
};
function startServer() {
  const server = createServer((req, res) => {
    const url = new URL(req.url, BASE);
    let rel = decodeURIComponent(url.pathname).replace(/^\/tedge-dot\/?/, '');
    if (rel === '' || rel.endsWith('/')) rel += 'index.html';
    let file = join(dist, normalize(rel));
    if (!file.startsWith(dist) || !existsSync(file)) {
      res.writeHead(404).end();
      return;
    }
    if (statSync(file).isDirectory()) file = join(file, 'index.html');
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream' });
    createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(PORT, () => resolve(server)));
}

// Runs in the page.
function inspect(glueAllowed) {
  const out = [];
  const allowed = glueAllowed.map((s) => new RegExp(s));
  const vis = (el) => {
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    return r.width > 0 && r.height > 0 && cs.visibility !== 'hidden' && cs.display !== 'none';
  };
  const desc = (el) => `<${el.tagName.toLowerCase()}${el.classList[0] ? '.' + el.classList[0] : ''}> "${(el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 40)}"`;
  const SKIP = 'pre, svg, .expressive-code, .td-ascii, .sl-anchor-link, .sr-only, nav, button, .td-btn, .sl-link-button, .site-title';
  const root = document.querySelector('main') || document.body;

  for (const el of root.querySelectorAll('a, code, strong, em, kbd')) {
    if (!vis(el) || el.closest(SKIP)) continue;
    const t = el.textContent;
    if (!t.trim()) continue;
    const text = (n) => (n && (n.nodeType === 3 || (n.nodeType === 1 && getComputedStyle(n).display.startsWith('inline'))) ? n.textContent : null);
    const before = text(el.previousSibling);
    const after = text(el.nextSibling);
    if (before && /[a-z,;:]$/.test(before) && /^[A-Za-z0-9(]/.test(t)) out.push(`glued words: "…${before.slice(-12)}${t.slice(0, 16)}…"`);
    if (after && /[A-Za-z0-9]$/.test(t) && /^[A-Za-z(]/.test(after) && !allowed.some((a) => a.test(after))) {
      out.push(`glued words: "…${t.slice(-16)}${after.slice(0, 12)}…"`);
    }
  }

  for (const el of root.querySelectorAll('*')) {
    if (!vis(el) || el.closest('pre, svg, table, .expressive-code, .td-ascii, .sl-anchor-link, .sr-only, .sl-heading-wrapper')) continue;
    const cs = getComputedStyle(el);
    if (['auto', 'scroll', 'hidden', 'clip'].includes(cs.overflowX) || cs.display === 'inline') continue;
    // Inside a scroll container (a tab bar, a wide table) overflowing is the point.
    let scroller = el.parentElement;
    while (scroller && !['auto', 'scroll'].includes(getComputedStyle(scroller).overflowX)) scroller = scroller.parentElement;
    if (scroller && root.contains(scroller)) continue;
    if (el.clientWidth > 0 && el.scrollWidth > el.clientWidth + 2 && el.children.length < 30 && !el.matches('.sl-container, .sl-markdown-content')) {
      out.push(`content spills out of ${desc(el)} (${el.scrollWidth}px in ${el.clientWidth}px)`);
    }
  }

  const BTN = 'button, .td-btn, .sl-link-button, summary.td-btn, select';
  for (const parent of new Set([...root.querySelectorAll(BTN)].filter(vis).map((e) => e.parentElement))) {
    const rows = [];
    for (const btn of [...parent.children].filter((c) => c.matches(BTN) && vis(c))) {
      const r = btn.getBoundingClientRect();
      const row = rows.find((rw) => rw.some(([, q]) => r.top < q.bottom && q.top < r.bottom));
      if (row) row.push([btn, r]);
      else rows.push([[btn, r]]);
    }
    for (const row of rows.filter((r) => r.length > 1)) {
      const hs = row.map(([, r]) => r.height);
      const tops = row.map(([, r]) => r.top);
      if (Math.max(...hs) - Math.min(...hs) > 2 || Math.max(...tops) - Math.min(...tops) > 2) {
        out.push(`buttons in a row differ: ${row.map(([e, r]) => `${desc(e)} ${r.height.toFixed(0)}px @${r.top.toFixed(0)}`).join(', ')}`);
      }
    }
  }

  const sideways = document.documentElement.scrollWidth - innerWidth;
  if (sideways > 0) out.push(`the page scrolls sideways by ${sideways}px`);
  return [...new Set(out)];
}

async function scrollThrough(page) {
  // Hydrates every client:visible island.
  await page.evaluate(async () => {
    for (let y = 0; y < document.body.scrollHeight; y += 600) {
      scrollTo(0, y);
      await new Promise((r) => setTimeout(r, 30));
    }
    scrollTo(0, 0);
  });
  await page.waitForTimeout(200);
}

async function clickEverything(page, path, errors) {
  const problems = [];
  const sel = 'main button, main summary, main [role=tab]';
  const n = await page.locator(sel).count();
  for (let i = 0; i < n; i++) {
    const el = page.locator(sel).nth(i);
    if (!(await el.isVisible().catch(() => false))) continue;
    // Selected tabs, pressed toggles and checked radios do nothing on purpose.
    const on = await Promise.all(['aria-selected', 'aria-pressed', 'aria-checked'].map((a) => el.getAttribute(a)));
    if (on.includes('true')) continue;
    const label = ((await el.innerText().catch(() => '')) || (await el.getAttribute('aria-label')) || '?').trim().replace(/\s+/g, ' ').slice(0, 40);
    const before = await page.evaluate(() => document.querySelector('main').innerHTML);
    const url = page.url();
    const errorsBefore = errors.length;
    let downloaded = false;
    page.once('download', () => (downloaded = true));
    await page.evaluate(() => navigator.clipboard.writeText('')).catch(() => {});
    try {
      await el.click({ timeout: 2000 });
    } catch (e) {
      problems.push(`/${path}: "${label}" cannot be clicked`);
      continue;
    }
    await page.waitForTimeout(200);
    const copied = await page.evaluate(() => navigator.clipboard.readText()).catch(() => '');
    const after = await page.evaluate(() => document.querySelector('main')?.innerHTML ?? '');
    if (page.url() === url && !downloaded && !copied && after === before) problems.push(`/${path}: "${label}" does nothing`);
    if (page.url() !== url) {
      // A button that navigates. Chrome rejects the cross-document view transition's promise
      // when leaving, as on any link: that is not the page's error.
      for (let k = errors.length - 1; k >= errorsBefore; k--) if (/^Transition was (aborted|skipped)/.test(errors[k])) errors.splice(k, 1);
      await page.goto(url, { waitUntil: 'networkidle' });
    }
  }
  return problems;
}

const server = await startServer();
const browser = await chromium.launch(process.env.CHROME_PATH ? { executablePath: process.env.CHROME_PATH } : {});
const problems = [];
try {
  for (const [width, height] of [[1280, 900], [390, 844]]) {
    const context = await browser.newContext({
      viewport: { width, height },
      colorScheme: 'dark',
      reducedMotion: 'reduce',
      permissions: ['clipboard-read', 'clipboard-write'],
      acceptDownloads: true,
    });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e.message ?? e)));
    for (const path of pages()) {
      errors.length = 0;
      await page.goto(BASE + path, { waitUntil: 'networkidle' });
      await page.evaluate(() => document.fonts.ready);
      await scrollThrough(page);
      for (const p of await page.evaluate(inspect, GLUE_ALLOWED.map((r) => r.source))) problems.push(`/${path} at ${width}px: ${p}`);
      if (width === 1280) problems.push(...(await clickEverything(page, path, errors)));
      for (const e of errors) problems.push(`/${path} at ${width}px: script error: ${e}`);
    }
    await context.close();
  }
} finally {
  await browser.close();
  server.close();
}

for (const p of problems) console.log(p);
if (problems.length) {
  console.error(`check-layout: ${problems.length} problem${problems.length > 1 ? 's' : ''}`);
  process.exit(1);
}
console.log(`check-layout: ${pages().length} pages clean at 1280px and 390px`);
