import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import YAML from 'yaml';
import {
  betweenMarkers,
  convertAlerts,
  convertPage,
  escapeForStarlight,
  extractTitle,
  renderPackageTable,
  rewriteLinks,
} from '../scripts/lib/sync.mjs';
import { loadMap, unplacedDocs } from '../scripts/sync-docs.mjs';

const ctx = {
  repository: 'thin-edge/tedge-dot',
  branch: 'main',
  base: '/tedge-dot',
  imageBase: '/tedge-dot/repo',
  slugs: new Map([
    ['README.md', 'reference/design/readme'],
    ['doc/reducing-data-volume.md', 'reference/configuration/reducing-data-volume'],
    ['flows/README.md', 'reference/flows/flows'],
  ]),
  isDirectory: (p) => ['doc/contract', 'flows', 'impl/c'].includes(p),
};

const rewrite = (md, sourcePath = 'README.md') => rewriteLinks(md, { ...ctx, sourcePath }).markdown;

describe('extractTitle', () => {
  it('takes the first level-1 heading as plain text and removes it', () => {
    const { title, body } = extractTitle('# `tedge-dot` — C *implementation*\n\nIntro.\n');
    expect(title).toBe('tedge-dot — C implementation');
    expect(body).toBe('Intro.\n');
  });

  it('ignores a heading inside a code block', () => {
    const md = '```sh\n# not a title\n```\n\n# Real title\n\nText\n';
    expect(extractTitle(md).title).toBe('Real title');
  });

  it('returns null when there is no level-1 heading', () => {
    expect(extractTitle('## Only h2\n').title).toBeNull();
  });
});

describe('rewriteLinks', () => {
  it('turns a link to a mapped file into a site link and keeps the anchor', () => {
    expect(rewrite('See [heartbeats](doc/reducing-data-volume.md#heartbeats).')).toBe(
      'See [heartbeats](/tedge-dot/reference/configuration/reducing-data-volume/#heartbeats).',
    );
  });

  it('resolves relative paths from the source file', () => {
    expect(rewrite('[up](../README.md#install)', 'doc/install.md')).toBe(
      '[up](/tedge-dot/reference/design/readme/#install)',
    );
  });

  it('links a directory to its README when that is mapped', () => {
    expect(rewrite('[flows/](flows/)')).toBe('[flows/](/tedge-dot/reference/flows/flows/)');
  });

  it('sends an unmapped file to GitHub blob/main', () => {
    expect(rewrite('[truncation](upstream/snmp2-varbind-truncation.md)', 'doc/install.md')).toBe(
      '[truncation](https://github.com/thin-edge/tedge-dot/blob/main/doc/upstream/snmp2-varbind-truncation.md)',
    );
  });

  it('sends an unmapped directory to GitHub tree/main', () => {
    expect(rewrite('[contract](contract/)', 'doc/install.md')).toBe(
      '[contract](https://github.com/thin-edge/tedge-dot/tree/main/doc/contract)',
    );
  });

  it('keeps the anchor of an unmapped file', () => {
    expect(rewrite('[justfile](justfile#L10)')).toBe(
      '[justfile](https://github.com/thin-edge/tedge-dot/blob/main/justfile#L10)',
    );
  });

  it('leaves absolute URLs, mailto and in-page anchors alone', () => {
    const md = '[a](https://thin-edge.io) [b](mailto:x@y.z) [c](#install)';
    expect(rewrite(md)).toBe(md);
  });

  it('does not touch links inside code', () => {
    const md = '`[a](doc/reducing-data-volume.md)`\n\n```md\n[a](doc/reducing-data-volume.md)\n```\n';
    expect(rewrite(md)).toBe(md);
  });

  it('rewrites reference-style definitions', () => {
    expect(rewrite('[x][1]\n\n[1]: doc/reducing-data-volume.md\n')).toBe(
      '[x][1]\n\n[1]: /tedge-dot/reference/configuration/reducing-data-volume/\n',
    );
  });

  it('keeps link text that looks like the URL', () => {
    expect(rewrite('[flows/README.md](flows/README.md)')).toBe('[flows/README.md](/tedge-dot/reference/flows/flows/)');
  });

  it('reports repository images and points them at the copied files', () => {
    const { markdown, images } = rewriteLinks('![arch](img/arch.svg)', { ...ctx, sourcePath: 'doc/README.md' });
    expect(markdown).toBe('![arch](/tedge-dot/repo/doc/img/arch.svg)');
    expect(images).toEqual(['doc/img/arch.svg']);
  });
});

describe('convertAlerts', () => {
  it('maps GitHub alerts to Starlight asides', () => {
    expect(convertAlerts('> [!NOTE]\n> Be **careful**.\n> Really.\n')).toBe(':::note\nBe **careful**.\nReally.\n:::\n');
    expect(convertAlerts('> [!WARNING]\n> Hot.')).toBe(':::caution\nHot.\n:::');
    expect(convertAlerts('> [!IMPORTANT]\n> Read.')).toBe(':::note[Important]\nRead.\n:::');
  });

  it('leaves a plain blockquote alone', () => {
    const md = '> **Status: alpha.** Things change.\n';
    expect(convertAlerts(md)).toBe(md);
  });
});

describe('escapeForStarlight', () => {
  it('escapes a bare placeholder that would be read as HTML, but not real HTML or code', () => {
    expect(escapeForStarlight('Run tedge-dot pki trust <thumbprint> now.\n')).toBe(
      'Run tedge-dot pki trust &lt;thumbprint> now.\n',
    );
    const kept = 'Use `<thumbprint>`.<br>\n\n<!-- comment -->\n';
    expect(escapeForStarlight(kept)).toBe(kept);
  });
});

describe('convertPage', () => {
  it('adds the front matter with an edit link to the source file', () => {
    const page = { source: 'doc/reducing-data-volume.md', slug: 'reference/configuration/reducing-data-volume' };
    const { content, title } = convertPage('# Reducing data volume\n\nPublish less.\n', page, ctx);
    expect(title).toBe('Reducing data volume');
    expect(content).toContain('title: "Reducing data volume"');
    expect(content).toContain('description: "Publish less."');
    expect(content).toContain('editUrl: "https://github.com/thin-edge/tedge-dot/edit/main/doc/reducing-data-volume.md"');
    expect(content).not.toContain('# Reducing data volume');
  });
});

describe('the repository', () => {
  it('has a page map whose sources exist and whose slugs are unique', () => {
    const map = loadMap();
    expect(map.pages.length).toBeGreaterThan(20);
    for (const page of map.pages) {
      expect(() => readFileSync(new URL(`../../${page.source}`, import.meta.url))).not.toThrow();
    }
  });

  it('places every document under doc/ on the site or in github_only', () => {
    expect(unplacedDocs(loadMap()), 'add each to docs-map.yaml: pages or github_only').toEqual([]);
  });

  it('reports a document that is neither published nor github_only', () => {
    const map = loadMap();
    const without = { ...map, pages: map.pages.filter((p) => p.source !== 'doc/measurement-output.md') };
    expect(unplacedDocs(without)).toEqual(['doc/measurement-output.md']);
    expect(unplacedDocs({ ...map, github_only: [] })).toContain('doc/upstream/snmp2-varbind-truncation.md');
  });

  it('keeps the package table in doc/install.md equal to site/src/data/packages.yaml', () => {
    const data = YAML.parse(readFileSync(new URL('../src/data/packages.yaml', import.meta.url), 'utf8'));
    const install = readFileSync(new URL('../../doc/install.md', import.meta.url), 'utf8');
    expect(
      betweenMarkers(install, 'packages'),
      'doc/install.md: update the table between the site:packages markers from site/src/data/packages.yaml',
    ).toBe(renderPackageTable(data));
  });
});

describe('fitDiagrams', async () => {
  const { fitDiagrams } = await import('../scripts/lib/sync.mjs');
  it('turns a box diagram into a sized <pre>, escaping its text', () => {
    const md = '```text\n┌──┐ <a>\n└──┘\n```\n';
    expect(fitDiagrams(md)).toBe('<pre class="td-ascii" style="--cols: 8" data-cols="8">┌──┐ &lt;a&gt;\n└──┘</pre>\n');
  });
  it('leaves code and indented fences alone', () => {
    const code = '```toml\n# ┌ not a diagram\nx = 1\n```\n';
    expect(fitDiagrams(code)).toBe(code);
    const indented = '1. Step\n\n   ```text\n   ┌──┐\n   ```\n';
    expect(fitDiagrams(indented)).toBe(indented);
  });
});

describe('fitDiagrams fence pairing', async () => {
  const { fitDiagrams } = await import('../scripts/lib/sync.mjs');
  it('never pairs a closing fence with the next opening one', () => {
    const md = '```toml\nx = 1\n```\n\n## Heading → with an arrow\n\n```sh\necho\n```\n';
    expect(fitDiagrams(md)).toBe(md);
  });
});

describe('tables in the published Markdown', () => {
  // A `|` inside inline code in a table row ends the cell, in GitHub's Markdown and on the site:
  // `a|b` must be written `a\|b`. A cell left with an odd number of backticks shows the split.
  const map = YAML.parse(readFileSync(new URL('../docs-map.yaml', import.meta.url), 'utf8'));
  for (const page of map.pages) {
    it(`${page.source}: no table cell splits a code span`, () => {
      const lines = readFileSync(new URL(`../../${page.source}`, import.meta.url), 'utf8').split('\n');
      const bad = [];
      let fence = false;
      lines.forEach((line, i) => {
        if (/^\s*(```|~~~)/.test(line)) fence = !fence;
        if (fence || !/^\s*\|/.test(line)) return;
        for (const cell of line.split(/(?<!\\)\|/)) {
          const ticks = (cell.replace(/\\`/g, '').match(/`/g) || []).length;
          if (ticks % 2) bad.push(`line ${i + 1}: ${cell.trim().slice(0, 50)}`);
        }
      });
      expect(bad).toEqual([]);
    });
  }
});
