## ADDED Requirements

### Requirement: Published to GitHub Pages
The documentation site SHALL be built from the repository and deployed to GitHub Pages at
`https://thin-edge.github.io/tedge-dot/` on every push to `main`, on every published release and
on manual dispatch. A pull request that touches the site's sources SHALL build the site and run
its checks without deploying.

#### Scenario: Push to main deploys
- **WHEN** a commit that changes `doc/reducing-data-volume.md` is pushed to `main`
- **THEN** the docs workflow builds the site and deploys it, and the page for that document shows the change

#### Scenario: Pull request builds without deploying
- **WHEN** a pull request changes a file under `site/`, `doc/`, `changes/` or `demo/config/`
- **THEN** the docs workflow builds the site and runs the link and tool checks, and nothing is deployed

### Requirement: Single source for reference content
Reference pages SHALL be generated at build time from repository files listed in a page map
(`site/docs-map.yaml`). The generated pages SHALL NOT be committed. A page's title SHALL come
from the source file's first heading. Every generated page SHALL link to its source file on
GitHub for editing.

#### Scenario: Mapped file is published
- **WHEN** `doc/mapping-values.md` is listed in the page map under Reference
- **THEN** the site has a page titled from its first heading, in the Reference sidebar section, with an "Edit on GitHub" link to `doc/mapping-values.md`

#### Scenario: Unmapped file stays on GitHub
- **WHEN** a published page links to `doc/upstream/snmp2-varbind-truncation.md`, which is not in the page map
- **THEN** the link points to that file on GitHub (`blob/main`)

### Requirement: Links are rewritten and checked
The build SHALL rewrite relative links in generated pages: links to mapped files SHALL become
site links (keeping the anchor), and links to other repository paths SHALL become GitHub URLs. The build
SHALL fail on any broken internal link or anchor.

#### Scenario: Link between mapped documents
- **WHEN** `README.md` links `doc/reducing-data-volume.md#heartbeats` and both files are mapped
- **THEN** the rendered link points to the site page for that document with the `#heartbeats` anchor

#### Scenario: Broken anchor fails the build
- **WHEN** a guide links to an anchor that does not exist on the target page
- **THEN** the site build fails and names the page and the link

### Requirement: Task-oriented structure
The site SHALL offer, in this order in the navigation: Get started (overview, install, first
device with the demo), Guides (one per scenario), Tools, Reference (contract, configuration,
connectors, flows, SDK, design/RFCs, migration) and What's new. Search SHALL work offline in the
static build.

#### Scenario: Scenario guides are listed
- **WHEN** a visitor opens Guides
- **THEN** guides exist at least for Modbus TCP to Cumulocity, secured OPC UA, CAN bus/CANopen, SNMP, fleets with point libraries, reducing data volume, writing to devices from the cloud, and small/old gateways with `tedge-dot-c`

#### Scenario: Search finds reference content
- **WHEN** a visitor searches for `max_interval`
- **THEN** results include the reducing-data-volume page and the configuration reference

### Requirement: Guide snippets come from real files
A config or flow shown in a guide SHALL be included from the repository file it describes
(e.g. `demo/config/modbus.toml`), not copied into the guide. A guide that shows part of a file
SHALL select it by marker comments in that file, not by line numbers.

#### Scenario: Demo config change reaches the guide
- **WHEN** `demo/config/modbus.toml` changes a point and the site is rebuilt
- **THEN** the Modbus guide shows the changed point without the guide being edited

### Requirement: Landing page quality bar
The landing page SHALL present what tedge-dot does, the supported protocols, the two packages
and a quick start, with an animated data-flow illustration. The animation SHALL stop for visitors
who prefer reduced motion. The landing page SHALL work in light and dark mode and at phone
width without horizontal scrolling, SHALL ship at most 100 kB of JavaScript, and SHALL score at
least 85 for performance and 95 for accessibility in Lighthouse.

#### Scenario: Reduced motion
- **WHEN** a visitor whose system prefers reduced motion opens the landing page
- **THEN** the data-flow illustration is shown static

#### Scenario: Lighthouse budget enforced
- **WHEN** the landing page is audited in CI
- **THEN** the performance score is at least 90 and the accessibility score at least 95, or the check fails

### Requirement: The site keeps up with the product
The site SHALL describe every user-visible feature on `main`. A change that adds or changes a
user-visible feature (a configuration key, a policy, an output mode, a command, a connector)
SHALL update the site in the same change: its reference document in `doc/` placed in the page
map, the guides and the overview that cover the area, and the interactive tools whose subject it
is. Every Markdown file under `doc/` SHALL be either a page in `site/docs-map.yaml` or match an
entry of its `github_only` list, and the site build and the docs check SHALL fail, naming the
file, when one is neither. A change's tasks SHALL include the site update (`openspec/config.yaml`
rule), so a feature is not done while the site describes the product without it.

#### Scenario: New document left off the site
- **WHEN** a pull request adds `doc/concurrent-reads.md` without adding it to `pages` or `github_only`
- **THEN** the docs check fails and names `doc/concurrent-reads.md`

#### Scenario: Contributor document kept off the site on purpose
- **WHEN** a pull request adds `doc/upstream/async-opcua-new-bug.md` and `github_only` lists `doc/upstream/**`
- **THEN** the docs check passes, and a published link to that file points to GitHub

#### Scenario: Feature lands with its pages
- **WHEN** a change adds `output = "measurements"` to the connector configuration
- **THEN** the same change places `doc/measurement-output.md` in the page map, mentions the mode where the overview and the data-volume guide describe what the connector publishes, and lets the config builder set it

