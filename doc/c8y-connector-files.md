# Managing connector files from Cumulocity

The Device Management plugin in [ui/](../ui/) keeps tedge-dot connector files and point
libraries in Cumulocity's configuration repository, checks and edits them in one place, and
installs them on thin-edge.io gateways through configuration management. Nothing has to be
installed or changed on the gateways.

## Requirements

- Cumulocity with Device Management 1025.
- Gateways with thin-edge.io 2.x and tedge-dot 0.0.13 or later. Earlier tedge-dot releases do not
  apply a changed file on their own; they need a reload after each update, for example the
  `config_update` workflow step the OPC-UA solution blueprint uses.
- These roles, which are the ones the platform asks for:
  - Adding an entry: inventory admin or inventory create, or managed-object admin or create
    together with binary admin or create. Without them, the OT connectors page hides *Add*,
    *Upload* and *Duplicate*, as Device Management hides *Save to repository* on a snapshot.
  - Changing or deleting an entry: inventory admin or managed-object admin, or owning the entry.
  - Sending to a gateway: device control admin, or admin rights on the device through an inventory
    role. Adding a file the gateway does not have yet also uploads a temporary file: inventory
    admin or create, or binary admin or create.
  - Sending to a group: bulk operation admin.

  The other actions stay visible, as in Device Management; the platform refuses what a user may
  not do, and the plugin shows its message.

## Installing the plugin

1. Download `tedge-dot-ui_<version>.zip` from the
   [release](https://github.com/thin-edge/tedge-dot/releases).
2. In Administration > Ecosystem > Extensions, add the zip.
3. Add the plugin to the Device Management application you use. Plugins go into a custom
   application: if your tenant runs the built-in Device Management, create a copy of it first.

## How files are named

A connector file `/etc/tedge/plugins/ot/<name>.toml` is the configuration type
`tedge-dot-<name>`. Names use letters, digits, `.`, `_` and `-`, and start with a letter or a
digit. The OPC-UA solution blueprint registers `opcua-pump.toml` as `tedge-dot-opcua-pump`, so its
gateways show up as they are.

A point library `/etc/tedge/plugins/ot/points.d/<protocol>/<name>.toml` is the type
`tedge-dot-points.<protocol>.<name>`, for example `tedge-dot-points.modbus.acme-meter`. Connector
file names may not start with `points.`, so the two kinds never share a type.

## OT connectors in the Management menu

The page lists the configuration repository entries whose type starts with `tedge-dot-`: connector
files first, point libraries in a second list (see [Point libraries](#point-libraries)).

- **Add connector file** opens the editor with a starting point for a protocol. Give the entry a
  name, the file name it gets on gateways, and optionally a device type.
- **Upload connector file** adds an existing `.toml` file. The file is checked like the editor
  checks a file and becomes an entry named after it: `line-3.toml` is the type `tedge-dot-line-3`.
  A file with a `[library]` table becomes a point library of its protocol instead. A valid file is
  added with *Add file*, optionally for one device type. A file with problems can be opened in the
  editor to fix it there. Other TOML files are refused.
- **Edit** opens the file in the editor. Saving replaces the entry's file.
- **Duplicate** opens a copy in the editor, not saved yet: the same content, description and
  device type, named "<name> copy" with the file name `<file>-copy`. The copy of a connector file
  gets the service name `tedge-dot-<file>-copy`, because a service name must be unique on a
  gateway; everything else, comments included, stays as it is.
- **Send to a gateway** installs the file on one gateway, adding it first when the gateway does
  not have it (see [Adding a file to a gateway](#adding-a-file-to-a-gateway)).
- **Send to a group** creates one bulk operation for the devices in a group. A group send never
  adds files: the dialog lists the devices in the group that do not offer the file, because
  their operations would fail. It checks the devices directly in the group, not those in
  subgroups.
- **Delete** removes the entry and its file from the repository. Gateways keep their copy.

### Form and TOML

The editor shows a file as a **form** or as **TOML**, switched above the editing area. Both edit
the same file.

The form has the sections of the docs site's config builder: `[connector]` with its reporting,
grouping and measurement settings, `[mqtt]`, `[connection]`, and each device with its settings,
`protocol_address` and points. Each field has a help text, and a schema error shows under the
field it concerns. *Add device*, *Add point* and the remove buttons start from the protocol's
templates, and switching the protocol gives every device and point the new protocol's address
fields while keeping policies. Device names and point ids that are still the old protocol's
defaults become the new protocol's: from OPC UA to SNMP, `opc1` becomes `switch1` and
`temperature_2` becomes `uptime_2`. Names you chose stay. Keys the form has no field for stay in
the file and are listed as "Also set, kept as is".

A form edit changes only the lines it concerns, so the file keeps its comments and layout. New
devices and points are added at the end of their device's section in the config builder's
layout. Removing a device or point also removes the comments inside its section. If an edit
cannot be written into the file that way, the editor asks before it rewrites the whole file in the
standard layout, and says how many comment lines that drops. Files open in the form, except files
with a syntax error, which open in TOML with the error marked.

### What the editor checks

Before saving, the editor validates the file with the same code as the docs site's config
builder: TOML syntax, then the contract schema (`doc/contract/schemas/config.schema.json`) and
each protocol's address shapes. Syntax errors are marked at their line; schema errors are listed
with the key they concern, for example `device "Pump01" › point "flow" › datatype`.

Every name in a device's `points_from` must be a point library that exists for the file's
protocol: one in the repository, or one the tedge-dot packages install (see
[Point libraries](#point-libraries)). A path is refused, because Cumulocity can neither deliver
nor check the file it names. Like a schema error, an unknown reference keeps *Save* disabled.

A method point (`access = "call_method"`) takes no value settings: `datatype`, `poll_interval`,
`subscribe` and the other fields that describe a value are refused with "does not apply to a
call_method point", as the connectors refuse them. For OPC UA, the method address keys
(`object_id`, `method_id`, `inputs`) apply only to a method point, and the variable keys not to
one.

The check cannot see what only the gateway knows, such as whether a file named by a setting can
be read. **A file that tedge-dot refuses still counts as installed in Cumulocity**: the connector
logs the problem and keeps its previous configuration until the next restart, after which it
stays stopped until the file is fixed. Check the connector after sending a file that changes file
paths.

## Point libraries

A point library (contract §3.4) is the point list of one device type, without connection data:
a `[library]` table with the protocol and optionally the device type, and `[[point]]` entries with
the same fields as a device's points. A device lists the libraries it inherits from in
`points_from`; tedge-dot applies them in order, then the device's own points, and a point with an
id that exists patches it.

The second list on the OT connectors page holds the repository's libraries, with the same actions
as connector files: **Add point library**, **Upload point library**, **Edit**, **Duplicate**,
**Send to a gateway**, **Send to a group** and **Delete**. Deleting a library that connector entries
reference names those entries first, because they cannot be saved again while the reference
stays.

In the editor, a library has a library name instead of a file name. Its form shows the protocol,
the `[library]` settings (device type, version, description, and for OPC UA the namespaces) and
the points, with the same point fields as a device. The protocol and the name are fixed once the
library is saved, because its type and its folder on gateways name them.

In a connector file, each device's **Point libraries** field is an ordered list: add a library
from the ones available for the protocol, move it up or down, or remove it. Below it, *From point
libraries* lists the points the device inherits, read-only, with the library each comes from.
*Override* adds a point with the same id to the device, which patches the library's point; the
device's own points that override one are marked. A device without its own type shows the type it
inherits from its first library that declares one.

The tedge-dot packages install a `demo-sim` library for each protocol in
`/usr/share/tedge-dot/points.d`. The plugin knows the libraries of its own tedge-dot version:
connector files can reference them without a repository entry, and they are never sent. A
repository library of the same name takes their place, as on the gateway.

Sending a connector file to a gateway brings the repository libraries it references. The ones the
gateway does not offer yet are added to its list of managed files in the same edit as the
connector file, with one confirmation, and sent before it. A library the gateway already offers is
not sent again; send the library itself to update it. tedge-dot re-reads the libraries its
connector files reference, so a new version applies without a restart.

A group send stays one bulk operation and adds no files: its check also lists the devices that
lack a library the file references, and reminds you to send changed libraries to the group
first.

## Points from a namespace scan

Instead of copying node ids from an OPC UA client, build a library or a device's points from a
scan of the server. The third list on the OT connectors page, **Namespaces**, holds the scans
stored in Cumulocity ([namespace files](namespace-files.md)):

- **Scan address space** asks a gateway to scan one of its OPC UA devices, with the device's own
  connection settings, and adds the result to the list. The gateway needs
  [opcua-scan](namespace-files.md#install-opcua-scan). A scope (start node, depth, include and
  exclude patterns, node classes, children per node) keeps large address spaces manageable. A
  device keeps one namespace file: a new scan replaces the previous one once it succeeds.
- **Upload namespace file** stores a file written by
  `opcua-scan browse ... --output tedge-dot-namespace` elsewhere, such as on a laptop. It replaces
  an earlier upload of the same server.
- **Download** saves a file as `.json.gz`, and **Delete** removes it. Libraries made from it stay.

**Create from namespace** opens a file as a tree. Expand it, search by name or path, and filter by
node class, data type, writable nodes, or nodes with points. Check a variable to take its proposed
point, or an object to take the points of the variables below it; a structure proposes one point
per field. Methods are actions, not values: check a method itself to add its `call_method` point,
which Cumulocity can then trigger with a `c8y_CallMethod` operation (an object with nothing but
methods below it takes those). A node marked with a ban sign has no point, and its details say why
(an array, a property, an unsupported type). *truncated* marks a branch the scan did not read in
full.

The checked points are listed on the right. Change a point's id, name or unit in place, or mark
several and set subscribe or poll with an interval, the access, and the measurement type and
fragment at once. Method points take only an id and a name: a change of several points leaves
their other settings alone and says so. Two points with the same id are marked, and the result cannot be created until
one is renamed.

Choose where the points go:

- **New point library**: a name and an optional device type. The library gets the scan's
  namespace table.
- **Existing point library**, or a **device in a connector file**: the points the target already
  has for nodes in the scan start checked. Unchecking one removes it, checking a node adds its
  point, and the target's other points, settings and comments stay. New points are written in the
  target's namespace numbering; a namespace it lacks is appended to its table.

**Open in editor** opens the result unsaved, with a note on what changed. Check it and save it as
any other entry, then send it to the gateway.

When a scan's connector file and device are in the repository, the picker starts with that device
as the target, so a scan made from a gateway lands in the right file without choosing it.

You can also start from the file: in the editor of an OPC UA connector file, each device has
**Add points from namespace** next to *Add point*; an OPC UA library has it too. The picker opens
with the device's scan (one made from the same connector file first, then the newest; choose
another at the top) and the file you are editing as the target, unsaved changes included. If the
device has no scan yet, scan it or upload a file from there. **Open in editor** brings you back
with the points added; **Back to the editor** brings you back unchanged.

## The OT connectors tab

Every device that offers the configuration type `tedge-configuration-plugin` gets an **OT
connectors** tab after Configuration: thin-edge.io gateways, and child devices that run their own
thin-edge.io agent. It works like the Configuration tab, limited to connector files and point
libraries:

- **On device** lists the gateway's connector files and, below them, its point libraries. *Get
  snapshot from device* reads the file; the preview can save the snapshot to the repository or
  download it.
- **Available from repository** lists the repository entries of the selected file's type. Unlike
  the Configuration tab, it leaves out entries without a type, which would replace the connector
  file with unrelated content. *Send configuration to device* installs one; *Edit in repository*
  opens it on the OT connectors page.
- **Add connector file** installs a repository entry the gateway does not have yet: a connector
  file, with the libraries it references, or a point library.
- **Register a file on the gateway** makes a file that is already there, such as a packaged
  `modbus.toml`, a configuration type, so you can take snapshots of it and send new versions.

Below them, **Namespaces** lists the namespace files this gateway's scans produced, with **Scan
address space** when the gateway offers it ([Points from a namespace scan](#points-from-a-namespace-scan)).

## Adding a file to a gateway

thin-edge.io's file plugin manages the files listed in
`/etc/tedge/plugins/tedge-configuration-plugin.toml`, and offers that list itself as the type
`tedge-configuration-plugin`. To add the connector file `<name>` (and the libraries it needs),
the plugin:

1. requests a snapshot of `tedge-configuration-plugin` from the gateway;
2. stops if the list already has `/etc/tedge/plugins/ot/<name>.toml` under another type, or the
   type `tedge-dot-<name>` for another path;
3. shows the entry it will append and waits for your confirmation:

   ```toml
   # Added by the tedge-dot OT connectors plugin for Device Management
   [[files]]
   path = '/etc/tedge/plugins/ot/<name>.toml'
   type = 'tedge-dot-<name>'
   user = 'tedge'
   group = 'tedge'
   mode = 0o644
   ```

   A point library's entry also sets `parent_user = 'tedge'`, `parent_group = 'tedge'` and
   `parent_mode = 0o755`, so the file plugin creates its protocol folder in `points.d` when the
   first library of that protocol arrives (thin-edge.io 2.1). Every file the send needs is in one
   entry list and one confirmation.

4. sends the extended list back, leaving everything else in it unchanged byte for byte, and
   removes the temporary upload afterwards;
5. waits until the gateway offers every new type, sends the new libraries and waits for each to
   succeed, then sends the connector file.

## Moving from a file registered by hand

A connector file already registered with the type `tedge-dot-<name>` needs nothing. One
registered under another type keeps working in the Configuration tab, but this plugin refuses to
register it a second time. Rename its type in `tedge-configuration-plugin.toml` to
`tedge-dot-<name>` to manage it here.
