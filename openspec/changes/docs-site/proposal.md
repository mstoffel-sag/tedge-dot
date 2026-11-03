## Why

tedge-dot's documentation is a set of Markdown files in the repository: a long README, connector
specs, RFCs and the contract. They are thorough, but they are written for contributors and are
arranged by how the code is built, not by what a user is trying to do. Someone who wants to get
a Modbus PLC into Cumulocity, or decide between `tedge-dot-rs` and `tedge-dot-c`, has to read
specs to find out. Now that the release notes link to the in-repo install guide instead of
repeating it, the docs are where users land, so they need to be a proper product site: easy to
navigate, task-oriented, and good enough to make tedge-dot stand out.

## What Changes

- A documentation site published to GitHub Pages (`https://thin-edge.github.io/tedge-dot/`),
  built from the repository on every push to `main` and on every release.
- A landing page that explains tedge-dot in under a minute: an animated data-flow hero (device →
  connector → flows → thin-edge.io → cloud), the supported protocols, the two packages, and a
  one-command start.
- Scenario guides, each runnable end to end against the demo simulators: Modbus TCP PLC to
  Cumulocity, a secured OPC UA server, CAN bus / CANopen, SNMP network devices, a fleet of
  identical devices with point libraries, cutting data volume on a metered link, writing to
  devices from the cloud, and running on an old or small gateway with `tedge-dot-c`.
- Interactive tools that run entirely in the browser:
  - a **config workbench** that builds a connector TOML for a chosen protocol, validated live
    against `doc/contract/schemas/config.schema.json`, and runs it on simulated signals: which
    samples `report` (on_change, deadband, min/max interval, debounce) publishes, and how
    measurement output's `batch` policy (time, frame, sample, aggregates) groups them into
    Cumulocity measurements;
  - a **package picker** that recommends `tedge-dot-rs` or `tedge-dot-c` from a few questions.
- The site keeps up with the product: a user-visible change updates the site in the same
  change, and CI fails on a document in `doc/` that is neither on the site nor declared
  GitHub-only, or on a schema policy value the tools do not offer.
- Reference sections generated from the existing in-repo docs (contract, connector specs, SDK,
  flows, RFCs, migration), so each fact keeps a single source: the site renders what is in
  `doc/`, it does not copy it.
- A **What's new** page built from the highlight fragments in `changes/`, grouped by release.
- Versioned links: the GitHub release header links the site in place of the raw Markdown once
  the site is live.

Non-goals: per-version documentation (one site tracks `main`, releases link their tag for the
raw files), a custom domain, translations, a hosted backend or analytics, and rewriting the
connector specs or RFCs.

## Capabilities

### New Capabilities

- `docs-site`: the published site: how it is built from the repository, deployed to GitHub
  Pages, its structure (landing, guides, tools, reference, what's new), quality gates (links,
  build, accessibility, light/dark) and how it stays in sync with `doc/`.
- `docs-interactive-tools`: the in-browser config builder, package picker and report-policy
  playground, their behaviour and their tie to the contract schemas and test vectors.
- `release-highlights`: the `changes/` fragment convention, how a release's notes select the
  highlights added since the previous tag, and how the site's What's new page groups them.

### Modified Capabilities

(none)

## Impact

- New `site/` directory (static site project with its own `package.json`); Node is a build-time
  dependency of the site only, never of the packages.
- New GitHub Actions workflow for building and deploying Pages; GitHub Pages must be enabled for
  the repository with the "GitHub Actions" source.
- `doc/` gains front matter (title, description) where needed, and a few user-facing pages
  (guides) are added; existing files are not moved, so their GitHub links keep working.
- `packaging/release-notes.md` header switches its links to the site.
- CI: a site build and link check on pull requests touching `doc/`, `changes/`, `site/` or the
  README.
