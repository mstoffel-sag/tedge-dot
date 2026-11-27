# tedge-dot OT connectors

Edit the connector files and point libraries of tedge-dot, thin-edge.io's OT connector service, in
Cumulocity Device Management, and install them on your gateways without logging in to them.

## What you get

- **Management > OT connectors**: your connector files, kept in the configuration repository.
  Add, upload, edit, duplicate, and delete them, and send them to one gateway or to a group. An
  uploaded file is checked first; a file with problems opens in the editor.
- **Point libraries**: keep the point list of a device type once, as a library, and let devices
  in many connector files inherit it. Pick a device's libraries from a list; the form shows the
  points it inherits. Sending a connector file brings the libraries it needs.
- **A form and a TOML editor**: write a connector file or point library in a form, with a short
  help text for every setting, or edit its TOML directly. Changes made in the form keep the file's
  comments. Before you save, the editor checks the file and shows each problem at the setting it
  concerns.
- **Points from an OPC UA scan**: scan an OPC UA device's address space from its gateway, keep
  the scan in Cumulocity as a namespace file, pick nodes from a tree, adjust the proposed points,
  and get a new point library or points added to a library or a device.
- **An OT connectors tab** on thin-edge.io devices: the device's connector files and point
  libraries, with snapshots, saving to the repository, downloading, and sending, like the
  Configuration tab.
- **New files on a gateway**: when a gateway does not have a file yet, the plugin adds it to the
  gateway's list of managed files, shows you the entry for confirmation, and then sends the file.

## Requirements

- Cumulocity with Device Management 1025.
- Gateways with thin-edge.io 2.x and tedge-dot 0.0.13 or later.
- To scan from Cumulocity: a tedge-dot version with the `c8y_ScanAddressSpace` operation, and
  opcua-scan 0.2.0 or later on the gateway. Uploading a namespace file made elsewhere needs
  neither.
- Inventory admin to add, change and delete entries, device control admin to send them to a
  gateway, and bulk operation admin to send them to a group. Users who can't add entries don't see
  *Add*, *Upload* and *Duplicate*. The guide lists the finer roles that work too.

## Getting started

1. In **Administration > Ecosystem > Extensions**, add the plugin's zip file.
2. Add the **OT connectors** plugin to the Device Management application you use. Plugins go into
   a custom application, so copy the built-in Device Management first if you use that one.
3. Open **Management > OT connectors**, select **Add connector file**, pick the protocol to start
   from, fill in the form, and save.
4. Open a gateway, go to its **OT connectors** tab, select **Add connector file**, pick your file,
   and confirm.

## Good to know

- A connector file `/etc/tedge/plugins/ot/<name>.toml` appears in Cumulocity as the configuration
  type `tedge-dot-<name>`. Files that are already registered that way show up as they are. A point
  library `points.d/<protocol>/<name>.toml` is the type `tedge-dot-points.<protocol>.<name>`.
- A device can only reference point libraries that exist: one in the repository, or one installed
  with tedge-dot, such as `demo-sim`.
- The check covers the file's syntax, settings, values, protocol addresses, and library
  references. It cannot see what only the gateway knows, such as whether a file a setting names
  exists there. A file that tedge-dot refuses still shows as installed, so check the connector
  after you send a file.
- A namespace file is a binary of type `tedge-dot-namespace`. The scan connects with the
  device's settings from its connector file; credentials never leave the gateway.
- Sending to a group does not add files to gateways. The dialog lists the devices in the group
  that do not have the file or a library it needs yet.

The full guide is
[Managing connector files from Cumulocity](https://github.com/thin-edge/tedge-dot/blob/main/doc/c8y-connector-files.md).
