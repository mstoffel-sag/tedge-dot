## 1. Release highlights groundwork (branch docs/slim-release-notes)

- [x] 1.1 Slim `packaging/release-notes.md` to a fixed header linking the docs at the release tag
- [x] 1.2 Move install, package choice and upgrade notes to `doc/install.md`; link it from the README
- [x] 1.3 Add `changes/` with its README, and select highlights added since the previous tag in `release.yaml`
- [x] 1.4 Add the openspec `tasks` rule that asks for a highlight in `changes/`
- [x] 1.5 Count only releases with assets as published; carry an unpublished tag's highlights and changelog into the next release
- [x] 1.6 Merge, then regenerate the 0.0.16 release body (with the 0.0.15 highlights written by hand, since `changes/` did not exist yet)
- [x] 1.7 What's new page: backfill the highlights of releases before `changes/` existed

## 2. Site scaffold

- [x] 2.1 Create `site/` as an Astro + Starlight project (Node LTS, lockfile committed), `base: /tedge-dot`
- [x] 2.2 Theme: tokens for light/dark, typography, tedge-dot wordmark and favicon; settle the brand open question with a maintainer
- [x] 2.3 Sidebar skeleton: Get started, Guides, Tools, Reference, What's new
- [x] 2.4 `just docs` / `just docs-build` recipes; `site/` entries in `.gitignore` (generated content, `dist/`, `node_modules/`)

## 3. Single-source sync

- [x] 3.1 `site/docs-map.yaml` listing every published repository file, its slug and its section
- [x] 3.2 `site/scripts/sync-docs.mjs`: front matter from the first heading, "Edit on GitHub" link, image copying
- [x] 3.3 Link rewriting: mapped files → site links (anchors kept), other paths → GitHub `blob/main`
- [x] 3.4 GitHub alerts (`> [!NOTE]` etc.) → Starlight asides
- [x] 3.5 Unit tests for the sync script (link rewriting, title extraction, unmapped files)
- [x] 3.6 `starlight-links-validator` enabled; fix every broken link it reports in `doc/`

## 4. Get started and guides

- [x] 4.1 Overview page (what tedge-dot is, architecture diagram, how it fits thin-edge.io)
- [x] 4.2 Install page rendering `doc/install.md` with the package table from `site/src/data/packages.yaml`
- [x] 4.3 "Your first device in 5 minutes" with the demo compose stack
- [x] 4.4 Marker-comment snippet include helper (select part of a repository file by markers)
- [x] 4.5 Guide: Modbus TCP PLC to Cumulocity
- [x] 4.6 Guide: secured OPC UA server (PKI, trust, `tedge-dot pki`)
- [x] 4.7 Guide: CAN bus and CANopen
- [x] 4.8 Guide: SNMP network devices (poll, SET, traps)
- [x] 4.9 Guide: a fleet of identical devices with point libraries
- [x] 4.10 Guide: reducing data volume on a metered link (links the playground)
- [x] 4.11 Guide: writing to devices from the cloud (device parameters, `map`, literal parameters)
- [x] 4.12 Guide: small and old gateways with `tedge-dot-c`

## 5. Interactive tools

- [x] 5.1 Preact integration; shared island styling that follows the site theme
- [x] 5.2 Config builder: schema-driven form, `smol-toml` emit, `ajv` validation, per-field errors, copy/download
- [x] 5.3 Config builder: start from any `demo/config/*.toml`
- [x] 5.4 Check: every `demo/config` and `packaging/config` file validates and round-trips through the builder
- [x] 5.5 Package picker driven by `packages.yaml`, with an explanation for each recommendation
- [x] 5.6 Report-policy engine in TypeScript; test that runs every case in `report/vectors.json`
- [x] 5.7 Report-policy playground UI: signal presets, policy form, chart of readings, published samples and heartbeats

## 6. Landing page

- [x] 6.1 Hero with animated SVG data flow (devices → tedge-dot → flows → thin-edge.io → cloud), reduced-motion fallback
- [x] 6.2 Protocol grid, "two packages, one contract" comparison, quick-start terminal block, guide cards
- [x] 6.3 Review in light/dark and at phone width; JS ≤ 100 kB on the landing page
- [x] 6.4 Lighthouse CI check (performance ≥ 85, accessibility ≥ 95)

## 7. What's new

- [x] 7.1 Build-time loader: each `changes/*.md` → the first published release (with assets) that contained it, or Unreleased; same rule as `release.yaml`
- [x] 7.2 What's new page, newest first, linking each GitHub release

## 8. CI and deployment

- [x] 8.1 `docs.yaml`: PR build + sync tests + link check + tool checks on relevant paths
- [x] 8.2 Deploy job on push to `main`, `release: published` and dispatch (`fetch-depth: 0`, upload-pages-artifact, deploy-pages)
- [x] 8.3 Dependabot for `site/package.json`
- [x] 8.4 Smoke: run the Modbus guide's commands against the demo stack in CI

## 9. Go live

- [ ] 9.1 Ask a repository admin to enable GitHub Pages with the "GitHub Actions" source; run the deploy
- [ ] 9.2 Point `packaging/release-notes.md` and the README's top section at the site; set the repository homepage URL
- [x] 9.3 Add a release highlight in `changes/docs-site.md` announcing the site

## 10. Keeping up with the product

- [x] 10.1 `github_only` in `docs-map.yaml`; the sync and a test fail on a `doc/` file that is neither published nor GitHub-only
- [x] 10.2 `openspec/config.yaml` rule: a user-visible change includes a task to update the site
- [x] 10.3 Batching engine in TypeScript (port of the Rust SDK's `Batcher`); test that runs every case in `measurements/vectors.json`
- [x] 10.4 Batching playground on the report-policy playground page: `batch` form, the three policies side by side, timeline of samples and measurements, thin-edge and Cumulocity JSON
- [x] 10.5 Test: the playgrounds offer every `report` key, `group_by` and aggregate in the schema
- [x] 10.6 Catch up with measurement output and concurrent reads: overview, data-volume guide, landing card, config builder (`output`, `max_inflight`, `max_inflight_total`, `batch`)
- [x] 10.7 Release highlight in `changes/docs-batching-playground.md`
- [x] 10.8 One device, up to four signals, as the page's shared input: shape, polled or pushed on change, interval, outage and `report` per signal; run length
- [x] 10.9 Batching from the signals' published samples, with a view window; one cell per signal on each measurement and a per-signal table, so a missing signal shows
- [x] 10.10 Gateway playground: the page in four steps (equipment, configuration, package, outcome) over one shared state; equipment editor with protocol, signals and examples
- [x] 10.11 Config builder bound to the playground (protocol from the equipment, a point per signal); point `poll_interval`, `sampling_interval`, `subscribe`, `report` and `batch`, and `batch.aggregate`, in the builder on every page
- [x] 10.12 Package line in the outcome from the configuration's features, linking the package picker (which stays its own page)
- [x] 10.13 Outcome resolved from the configuration (interval and `report` inheritance, the configuration's `batch` tables); tests for the resolution and the examples' round trip
- [x] 10.14 Paste or load a configuration with the signals following (superseded the page-to-page handoff once the pages merged)
- [x] 10.15 Config workbench: the config builder and the playground merged into one page (`tools/workbench`), the old URLs redirected and every link moved; the signals step replaces the equipment step, the protocol is chosen in the builder
- [x] 10.16 Point libraries resolved for the simulation (`points_from` against `demo/points.d`, inline points patching them), so the demo configs run
- [x] 10.17 Collapsible points, `[mqtt]`, `[connection]`, device settings, the measurement table and the page's explanations
- [x] 10.18 Help popups on every input field (texts in `src/lib/help.ts`, test for coverage); collapsed sections styled as panels with a chevron; the Paste TOML panel opens in place
