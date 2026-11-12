// Pure helpers behind sync-docs.mjs: turn a repository Markdown file into a Starlight page.
// Everything here works on strings so that the unit tests need no file system.

import { posix } from 'node:path';
import { fromMarkdown } from 'mdast-util-from-markdown';
import { gfm } from 'micromark-extension-gfm';
import { gfmFromMarkdown } from 'mdast-util-gfm';
import { visit } from 'unist-util-visit';

/** Extract the first level-1 heading as plain text and remove it from the body. */
export function extractTitle(markdown) {
  const tree = parse(markdown);
  const h1 = tree.children.find((n) => n.type === 'heading' && n.depth === 1);
  if (!h1) return { title: null, body: markdown };
  const title = plainText(h1).trim();
  const { start, end } = h1.position;
  const body = markdown.slice(0, start.offset) + markdown.slice(end.offset).replace(/^\r?\n/, '');
  return { title, body: body.replace(/^\s*\n/, '') };
}

/** The first paragraph after the title, as plain text, for the page description. */
export function extractDescription(markdown, maxLength = 200) {
  const tree = parse(markdown);
  const p = tree.children.find((n) => n.type === 'paragraph' && !onlyImagesOrLinks(n));
  if (!p) return null;
  const text = plainText(p).replace(/\s+/g, ' ').trim();
  if (text.length <= maxLength) return text;
  const cut = text.slice(0, maxLength);
  return cut.slice(0, cut.lastIndexOf(' ')).replace(/[,;:]$/, '') + '…';
}

const ALERTS = {
  NOTE: { type: 'note' },
  TIP: { type: 'tip' },
  IMPORTANT: { type: 'note', title: 'Important' },
  WARNING: { type: 'caution' },
  CAUTION: { type: 'danger', title: 'Caution' },
};

/**
 * GitHub alerts (`> [!NOTE]` and friends) become Starlight asides (`:::note`). A blockquote
 * that does not start with an alert marker is left alone, as is anything in a code block.
 */
export function convertAlerts(markdown) {
  const tree = parse(markdown);
  const edits = [];
  visit(tree, 'blockquote', (node) => {
    const first = node.children[0];
    if (!first || first.type !== 'paragraph') return;
    const source = markdown.slice(node.position.start.offset, node.position.end.offset);
    const match = /^>\s*\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\][ \t]*\r?\n?/.exec(source);
    if (!match) return;
    const alert = ALERTS[match[1]];
    const indent = ' '.repeat(node.position.start.column - 1);
    const inner = source
      .slice(match[0].length)
      .split('\n')
      .map((line, i) => (i === 0 ? line : line.slice(indent.length)).replace(/^>[ \t]?/, ''))
      .join('\n')
      .trimEnd();
    const open = alert.title ? `:::${alert.type}[${alert.title}]` : `:::${alert.type}`;
    const lines = [open, ...inner.split('\n'), ':::'];
    edits.push({
      start: node.position.start.offset,
      end: node.position.end.offset,
      text: lines.map((l, i) => (i === 0 ? l : indent + l)).join('\n'),
    });
    return 'skip';
  });
  return applyEdits(markdown, edits);
}

/**
 * Rewrite the relative links of a page published from `sourcePath`:
 * - a link to a mapped file (or to a directory whose README is mapped) becomes a site link,
 *   keeping its anchor;
 * - a link to any other repository path becomes a GitHub URL (`tree` for a directory,
 *   `blob` otherwise);
 * - absolute URLs, `mailto:` and in-page anchors are kept.
 * Images that are repository files are reported in `images` and pointed at `imageBase`.
 *
 * @param {string} markdown
 * @param {object} ctx
 * @param {string} ctx.sourcePath    repository path of the file being published
 * @param {Map<string,string>} ctx.slugs  repository path -> site slug
 * @param {string} ctx.repository    e.g. "thin-edge/tedge-dot"
 * @param {string} ctx.branch        e.g. "main"
 * @param {string} ctx.base          site base path, e.g. "/tedge-dot"
 * @param {(path: string) => boolean} [ctx.isDirectory]
 * @param {string} [ctx.imageBase]   site path that copied images are served from
 */
export function rewriteLinks(markdown, ctx) {
  const tree = parse(markdown);
  const edits = [];
  const images = new Set();
  visit(tree, (node) => {
    if (!['link', 'image', 'definition'].includes(node.type)) return;
    if (!node.position) return;
    const target = resolveTarget(node.url, ctx, node.type === 'image');
    if (target.image) images.add(target.image);
    if (target.url === node.url) return;
    // Replace the URL inside the node's own source, so text and titles stay untouched.
    const start = node.position.start.offset;
    const source = markdown.slice(start, node.position.end.offset);
    const at = findUrl(source, node.url);
    if (at < 0) return;
    edits.push({ start: start + at, end: start + at + node.url.length, text: target.url });
  });
  return { markdown: applyEdits(markdown, edits), images: [...images] };
}

/** Where a link from `ctx.sourcePath` to `url` should point on the site. */
export function resolveTarget(url, ctx, isImage = false) {
  if (!url || /^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith('//') || url.startsWith('#')) {
    return { url };
  }
  // Already a site link (e.g. one the sync script adds): no repository path to resolve.
  if (ctx.base && (url === ctx.base || url.startsWith(`${ctx.base}/`))) return { url };
  const [, rawPath, suffix = ''] = /^([^?#]*)(.*)$/.exec(url);
  const anchor = suffix.includes('#') ? suffix.slice(suffix.indexOf('#')) : '';
  const decoded = safeDecode(rawPath);
  const joined = decoded.startsWith('/')
    ? decoded.slice(1)
    : posix.join(posix.dirname(ctx.sourcePath), decoded);
  const repoPath = posix.normalize(joined).replace(/\/$/, '');

  if (repoPath.startsWith('..')) {
    // Outside the repository: GitHub would resolve it against the blob URL; do the same.
    return { url: new URL(url, githubUrl(ctx, 'blob', ctx.sourcePath)).href };
  }

  if (isImage) {
    if (ctx.imageBase) return { url: `${ctx.imageBase}/${repoPath}`, image: repoPath };
    return { url: githubRaw(ctx, repoPath) };
  }

  const isDir = ctx.isDirectory ? ctx.isDirectory(repoPath) : decoded.endsWith('/');
  const candidates = isDir
    ? [`${repoPath}/README.md`.replace(/^\.\//, '')]
    : [repoPath === '.' ? 'README.md' : repoPath];
  for (const candidate of candidates) {
    const slug = ctx.slugs.get(candidate);
    if (slug) return { url: `${ctx.base}/${slug}/${anchor}` };
  }
  return { url: githubUrl(ctx, isDir ? 'tree' : 'blob', repoPath) + suffix };
}

export function githubUrl(ctx, kind, repoPath) {
  const path = repoPath === '.' ? '' : `/${repoPath}`;
  return `https://github.com/${ctx.repository}/${kind}/${ctx.branch}${path}`;
}

function githubRaw(ctx, repoPath) {
  return `https://raw.githubusercontent.com/${ctx.repository}/${ctx.branch}/${repoPath}`;
}

/** Starlight front matter for a generated page. */
export function frontMatter({ title, description, editUrl, sourcePath }) {
  const lines = ['---', `title: ${JSON.stringify(title)}`];
  if (description) lines.push(`description: ${JSON.stringify(description)}`);
  lines.push(`editUrl: ${JSON.stringify(editUrl)}`);
  lines.push('# Generated by site/scripts/sync-docs.mjs from ' + sourcePath + ' -- edit that file.');
  lines.push('---', '');
  return lines.join('\n');
}

/**
 * Replace the text between `<!-- site:<name> -->` and `<!-- /site:<name> -->` markers.
 * Returns null when the markers are missing.
 */
export function replaceBetweenMarkers(markdown, name, replacement) {
  const re = new RegExp(`(<!-- site:${name} -->\\r?\\n)([\\s\\S]*?)(<!-- /site:${name} -->)`);
  if (!re.test(markdown)) return null;
  return markdown.replace(re, (_, open, _old, close) => `${open}${replacement.trimEnd()}\n${close}`);
}

/** The text between `<!-- site:<name> -->` markers, or null. */
export function betweenMarkers(markdown, name) {
  const m = new RegExp(`<!-- site:${name} -->\\r?\\n([\\s\\S]*?)<!-- /site:${name} -->`).exec(markdown);
  return m ? m[1] : null;
}

/** The package table of doc/install.md, rendered from site/src/data/packages.yaml. */
export function renderPackageTable(data) {
  const rows = data.packages.map(
    (p) => `| \`${p.id}\` | ${p.implementation} (\`${p.source}\`) | ${p.table.pick.trim()} |`,
  );
  return ['| Package | Implementation | Pick it when |', '|---|---|---|', ...rows].join('\n') + '\n';
}

const BOX_DRAWING = /[\u2500-\u257f\u25b2-\u25c4\u2190-\u2193]/;

/**
 * ASCII diagrams (a top-level fence with box-drawing characters and no language, or `text`)
 * become a `<pre class="td-ascii">` that knows its width in columns, so the site's CSS can size
 * its font to fit the column instead of making the reader scroll the picture sideways.
 */
export function fitDiagrams(markdown) {
  const lines = markdown.split('\n');
  const out = [];
  for (let i = 0; i < lines.length; i++) {
    // Any fence, at any indent: skip it whole, so a closing fence is never read as an opening one.
    const open = /^( *)(`{3,}|~{3,})(.*)$/.exec(lines[i]);
    if (!open) {
      out.push(lines[i]);
      continue;
    }
    const [, indent, fence, info] = open;
    let j = i + 1;
    const close = new RegExp(`^ *${fence[0] === '`' ? '`' : '~'}{${fence.length},}\\s*$`);
    while (j < lines.length && !close.test(lines[j])) j++;
    const body = lines.slice(i + 1, j);
    const lang = info.trim();
    if (indent === '' && (lang === '' || lang === 'text' || lang === 'txt') && body.some((l) => BOX_DRAWING.test(l))) {
      const cols = Math.max(...body.map((l) => [...l].length));
      const escaped = body.map((l) => l.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')).join('\n');
      out.push(`<pre class="td-ascii" style="--cols: ${cols}" data-cols="${cols}">${escaped}</pre>`);
    } else {
      out.push(...lines.slice(i, Math.min(j + 1, lines.length)));
    }
    i = j;
  }
  return out.join('\n');
}

/** Publish one repository file as a page. */
export function convertPage(markdown, page, ctx) {
  const { title: heading, body } = extractTitle(markdown);
  const title = heading || page.label || posix.basename(page.source, '.md');
  const withAlerts = convertAlerts(body);
  const { markdown: linked, images } = rewriteLinks(withAlerts, { ...ctx, sourcePath: page.source });
  const description = extractDescription(withAlerts);
  const head = frontMatter({
    title,
    description,
    editUrl: githubUrl(ctx, 'edit', page.source),
    sourcePath: page.source,
  });
  return { content: head + fitDiagrams(escapeForStarlight(linked)), images, title };
}

/**
 * Markdown that GitHub renders literally but Astro's Markdown would read as HTML: a bare
 * `<placeholder>` outside code (e.g. `tedge-dot pki trust <thumbprint>` in running text)
 * would be swallowed as an unknown tag. Escape the `<` of such tags; real HTML (known tags,
 * comments) is kept.
 */
export function escapeForStarlight(markdown) {
  const tree = parse(markdown);
  const edits = [];
  visit(tree, 'html', (node) => {
    const value = node.value;
    if (/^<!--/.test(value)) return;
    const tag = /^<\/?([a-zA-Z][\w-]*)/.exec(value);
    if (tag && HTML_TAGS.has(tag[1].toLowerCase())) return;
    edits.push({ start: node.position.start.offset, end: node.position.start.offset + 1, text: '&lt;' });
  });
  return applyEdits(markdown, edits);
}

const HTML_TAGS = new Set(
  ('a abbr b br code dd del details div dl dt em h1 h2 h3 h4 h5 h6 hr i img ins kbd li ol p pre ' +
    'q s samp small span strong sub summary sup table tbody td tfoot th thead tr tt u ul var picture source')
    .split(' '),
);

// --- internals -----------------------------------------------------------------------------

function parse(markdown) {
  return fromMarkdown(markdown, { extensions: [gfm()], mdastExtensions: [gfmFromMarkdown()] });
}

function plainText(node) {
  if (node.type === 'text' || node.type === 'inlineCode') return node.value;
  if (node.type === 'image') return node.alt || '';
  if (!node.children) return '';
  return node.children.map(plainText).join('');
}

function onlyImagesOrLinks(paragraph) {
  return paragraph.children.every(
    (c) => c.type === 'image' || c.type === 'link' || (c.type === 'text' && !c.value.trim()),
  );
}

function applyEdits(text, edits) {
  let out = text;
  for (const e of [...edits].sort((a, b) => b.start - a.start)) {
    out = out.slice(0, e.start) + e.text + out.slice(e.end);
  }
  return out;
}

function findUrl(source, url) {
  // The URL follows `](`, `]: ` or `<`; search after the link text to avoid a match inside it.
  const afterText = Math.max(source.lastIndexOf(']('), source.indexOf(']:'));
  const at = source.indexOf(url, afterText < 0 ? 0 : afterText);
  if (at >= 0) return at;
  return source.indexOf(url);
}

function safeDecode(s) {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}
