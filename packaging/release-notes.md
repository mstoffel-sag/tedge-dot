tedge-dot ships as **two interchangeable packages**, each containing the same
`/usr/bin/tedge-dot` binary, the same systemd unit and the same
`/etc/tedge/plugins/ot/` config layout. They speak the same
[OT Connector Contract](https://github.com/thin-edge/tedge-dot/blob/main/doc/contract/),
so a config, a flow or a cloud integration built against one works unchanged against
the other. Install **one or the other** — they declare each other as conflicting.

| Package | Implementation | Pick it when |
|---|---|---|
| `tedge-dot-rs` | Rust (`impl/rust/`) | Default. Richest protocol support, one static binary, no shared-library dependencies. |
| `tedge-dot-c` | C (`impl/c/`) | Small or old devices: ~25x smaller, a **glibc 2.17** floor (Debian 8 / RHEL 7 era), and it additionally ships the **PROFIBUS-DP** connector. |

### Install

Both packages are published to the thin-edge.io **community** repository, which
also carries their `tedge-parameter-plugin` dependency (it maps the Cumulocity
*Parameters* tab onto point writes). With that repository set up
([instructions](https://thin-edge.github.io/thin-edge.io/install/#community-plugins)):

```sh
# Debian / Ubuntu
sudo apt-get install -y tedge-dot-rs                         # or tedge-dot-c
sudo systemctl status tedge-dot

# RPM distros
sudo dnf install tedge-dot-rs                                # or tedge-dot-c
```

Or download the package for your architecture from the assets below. The
package manager still resolves `tedge-parameter-plugin` from the community
repository, so set that up first:

```sh
sudo apt-get install -y ./tedge-dot-rs_*_linux_amd64.deb    # or ./tedge-dot-c_*_amd64.deb
sudo dnf install ./tedge-dot-rs_*_linux_amd64.rpm           # or ./tedge-dot-c_*_amd64.rpm
```

> **Alpine:** the attached `.apk` files carry a version string apk-tools rejects
> (`apk version -c` refuses both this project's tag format and the snapshot
> form), so `apk add` will not install them. Use the tarball on Alpine until
> that is fixed — see TODO.md.

Or grab a tarball and run the binary directly — it doubles as a one-shot
read/write CLI. The `tedge-dot-c` tarball carries the default configs to start from:

```sh
./tedge-dot read -c config-defaults/modbus.toml --json
```

`SHA256SUMS` covers every asset in this release.

### Upgrading from a release before the split

The package formerly called `tedge-dot` is now **`tedge-dot-rs`**. Both packages
`Replaces:` the old name, so installing either over an existing `tedge-dot`
works and keeps your `/etc/tedge/plugins/ot/` configuration:

```sh
sudo apt-get install -y ./tedge-dot-rs_*_linux_amd64.deb
```

Two caveats:

- `apt install tedge-dot` no longer resolves — `tedge-dot` is now a *virtual*
  package provided by both, so apt cannot choose. Install by the real name.
  Scripts and runbooks using the old name need updating.
- A plain `apt upgrade` will **not** migrate an existing `tedge-dot` install; it
  stays on the old package name and receives no further updates until you
  install one of the new ones explicitly.

### Report by exception (`report`)

A point, a device or `[connector]` can now declare a **reporting policy**, applied by the
connector itself for every protocol before a sample is published: `on_change`, `deadband`
(absolute, or a percentage such as `"2%"`), `min_interval` (a change held back is published
when the interval ends), `max_interval` (a heartbeat that publishes a fresh reading of a flat
signal, reading a subscribed OPC UA node on demand) and `debounce`. See
`doc/reducing-data-volume.md`.

- The packaged Modbus, OPC UA, CANopen and PROFIBUS configs set
  `[connector] report = { max_interval = "30m" }`, so a device whose values never change stays
  available in Cumulocity. This applies to **fresh installs only**: an existing
  `/etc/tedge/plugins/ot/*.toml` is kept on upgrade. To opt in, add that line to its
  `[connector]` section and reload the service. Without a `report` anywhere, every reading is
  published as before.
- The `ot-measurement` flow's `on_change`, `deadband`, `min_interval` and `debounce` settings,
  and the same keys in a point's `meta`, are **deprecated**. They keep working; move them to
  the point's `report` table, and do not use both.
- `tedge-dot-c`, CAN bus: a signal is now published once per received frame, as in
  `tedge-dot-rs`, instead of republishing the last frame on every poll.

### Value mapping (`map`)

A point can now declare a **value map**, which the connector applies in both directions for
every protocol. It maps state codes to labels, using exact values, lists or inclusive ranges
with a catch-all `default`, and it converts with `as = "number" | "string" | "bool"`, so numeric
text becomes a measurement. Samples carry the mapped value and add the original as
`source_value`.

Writes carry the mapped value as well, and the connector maps it back to the device's code. This
covers the `write` and `write-batch` commands, the CLI and Cumulocity device parameters, where
`tedge-dot describe` offers a mapped parameter's labels as a choice. See
`doc/mapping-values.md`.

- A point without `map` is unchanged.
- The demo point libraries show the feature: the Modbus `boiler_state` maps by range, and the
  OPC UA `run_state` is an enumerated parameter.

### Literal device parameters (`meta.parameter.fragment`)

A device parameter no longer has to sit inside an object. With
`meta.parameter = { fragment = "pump_speed" }` the point's value is published as the fragment
itself, so the managed object holds `"pump_speed": 42` rather than an object around one value.
`tedge-dot describe` renders it as a primitive Cumulocity definition (number, boolean or string),
and an edit from the *Parameters* tab carrying the bare value is written to the point.

- The name is used verbatim, like `set`. It cannot be combined with `set`, `group` or `key`, and
  one name cannot be both a literal and a set. `describe` refuses either.
- Configurations without `fragment` are unchanged.

### Notes

- On a fresh install the service starts with **no devices configured**. Add
  `[[device]]` sections under `/etc/tedge/plugins/ot/`, or copy a demo config
  from `/usr/share/tedge-dot/demo/`, then restart the service.
- The connector is a dumb driver: the device-side **flows** that map its
  envelopes onto the thin-edge data model are deployed active into
  `/etc/tedge/mappers/c8y/flows/`, with the opt-in alarm/event flows in
  `/usr/share/tedge-dot/flows/`. See `flows/README.md`.
- `tedge-dot-c` runtime dependencies (Debian/Ubuntu names): `libmodbus5`,
  `libmosquitto1`, `libcjson1`; open62541 is statically linked. It is
  cross-compiled with zig against the glibc 2.17 floor.
- `tedge-dot-rs` omits the PROFIBUS connector: its serial dependency has a
  native libudev build script that does not cross-compile. Build from source on
  Linux with `cargo build --manifest-path impl/rust/Cargo.toml --features profibus`, or use `tedge-dot-c`.
- Where the two implementations differ in behaviour, see the parity table in
  `impl/c/README.md`.
- On **Alpine**, the two packages are not mutually exclusive by metadata: apk
  expresses conflicts differently and the `conflicts` field is not carried into
  the `.apk`. Installing one over the other will collide on
  `/usr/bin/tedge-dot` — remove the first with `apk del` before installing the
  second, rather than forcing the overwrite.
