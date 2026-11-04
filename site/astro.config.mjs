// @ts-check
import { readFileSync } from 'node:fs';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import preact from '@astrojs/preact';
import starlightLinksValidator from 'starlight-links-validator';
import YAML from 'yaml';
import rehypeInlineCodeNowrap from './src/plugins/rehype-inline-code-nowrap.mjs';
import rehypeTableScroll from './src/plugins/rehype-table-scroll.mjs';

// The Reference sidebar comes from the page map, so a published repository file is listed in
// exactly one place.
const map = YAML.parse(readFileSync(new URL('./docs-map.yaml', import.meta.url), 'utf8'));
const reference = map.sections.map((section) => ({
  label: section,
  collapsed: true,
  items: [
    ...(section === 'Configuration'
      ? [{ label: 'Configuration file', link: '/reference/configuration/config-file/' }]
      : []),
    ...map.pages
      .filter((p) => p.section === section)
      .map((p) => {
        // `badge` in the page map: a sidebar badge, e.g. Preview for a connector not yet tested on real equipment.
        const badge = p.badge ? { badge: { text: p.badge, variant: 'caution' } } : {};
        return p.label || p.badge ? { label: p.label, slug: p.slug, ...badge } : p.slug;
      }),
  ],
}));

export default defineConfig({
  site: 'https://thin-edge.github.io',
  base: '/tedge-dot',
  trailingSlash: 'ignore',
  // Load a page when the pointer rests on its link, so a click usually shows it at once.
  prefetch: { prefetchAll: true, defaultStrategy: 'hover' },
  // The config builder and the report-policy playground became the config workbench.
  redirects: {
    '/tools/config-builder': '/tedge-dot/tools/workbench/',
    '/tools/report-playground': '/tedge-dot/tools/workbench/',
  },
  markdown: { rehypePlugins: [rehypeInlineCodeNowrap, rehypeTableScroll] },
  // The tools import the contract schemas and test vectors from the repository (../doc).
  vite: { server: { fs: { allow: ['..'] } } },
  integrations: [
    starlight({
      title: 'tedge-dot',
      description:
        'OT protocol connectors for thin-edge.io: Modbus, OPC UA, SNMP, CAN bus, CANopen, PROFIBUS-DP and AVEVA PI into one neutral MQTT contract.',
      favicon: '/favicon.svg',
      customCss: [
        '@fontsource-variable/inter',
        '@fontsource-variable/jetbrains-mono',
        './src/styles/theme.css',
      ],
      social: [{ icon: 'github', label: 'GitHub', href: 'https://github.com/thin-edge/tedge-dot' }],
      editLink: { baseUrl: 'https://github.com/thin-edge/tedge-dot/edit/main/site/' },
      lastUpdated: false,
      // Robot Framework has no Shiki grammar; show those blocks as plain text.
      expressiveCode: { shiki: { langAlias: { robotframework: 'txt', robot: 'txt' } } },
      components: {
        // The landing page's animated data-flow hero.
        Hero: './src/components/LandingHero.astro',
        // The logo, inline, so it does not blink on navigation.
        SiteTitle: './src/components/SiteTitle.astro',
        // A notice on every page: the project is AI-driven and alpha.
        Banner: './src/components/Banner.astro',
      },
      head: [
        { tag: 'meta', attrs: { name: 'theme-color', content: '#181564' } },
      ],
      sidebar: [
        {
          label: 'Get started',
          items: [
            { label: 'Overview', slug: 'get-started/overview' },
            { label: 'Install', slug: 'get-started/install' },
            { label: 'Your first device', slug: 'get-started/first-device' },
          ],
        },
        { label: 'Guides', items: [{ autogenerate: { directory: 'guides' } }] },
        {
          label: 'Tools',
          items: [
            { label: 'Config workbench', slug: 'tools/workbench' },
            { label: 'Package picker', slug: 'tools/package-picker' },
          ],
        },
        { label: 'Reference', items: reference },
        { label: "What's new", link: '/whats-new/' },
      ],
      plugins: [starlightLinksValidator({ errorOnLocalLinks: false })],
    }),
    preact(),
  ],
});
