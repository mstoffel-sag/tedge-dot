# Installing and upgrading tedge-dot

## Pick a package

tedge-dot ships as **two interchangeable packages**, each containing the same
`/usr/bin/tedge-dot` binary, the same systemd unit and the same `/etc/tedge/plugins/ot/` config
layout. They speak the same [OT Connector Contract](contract/), so a config, a flow or a cloud
integration built against one works unchanged against the other. Install **one or the other**:
they declare each other as conflicting. Start with **`tedge-dot-c`**; pick `tedge-dot-rs` when you
need one of the features only the Rust build has.

> **Not final yet:** both packages are maintained and released today, but whether both will remain
> in the final product is not decided. They implement the same contract, so a config, a flow or a
> cloud integration built for one carries over to the other.

<!-- site:packages -->
| Package | Implementation | Pick it when |
|---|---|---|
| `tedge-dot-c` | C (`impl/c/`) | **Default.** ~25x smaller, a **glibc 2.17** floor (Debian 8 / RHEL 7 era), amd64, arm64 and armhf packages, and every protocol, including the PROFIBUS-DP preview. |
| `tedge-dot-rs` | Rust (`impl/rust/`) | When you need a feature only the Rust build has: SNMPv3 with SHA-2 or AES-192/256, the deprecated Basic128Rsa15 policy, or CAN FD. |
<!-- /site:packages -->

Where the two differ in behaviour, see the parity table in [impl/c/README.md](../impl/c/README.md).

- `tedge-dot-rs` omits the PROFIBUS connector: its serial dependency has a native libudev build
  script that does not cross-compile. Build from source on Linux with
  `cargo build --manifest-path impl/rust/Cargo.toml --features profibus`, or use `tedge-dot-c`.
- `tedge-dot-c` runtime dependencies (Debian/Ubuntu names): `libmodbus5`, `libmosquitto1`,
  `libcjson1`; open62541 is statically linked. It is cross-compiled with zig against the
  glibc 2.17 floor.

## Install from the community repository

Both packages are published to the thin-edge.io **community** repository, which also carries
their `tedge-parameter-plugin` dependency (it maps the Cumulocity *Parameters* tab onto point
writes). With that repository set up
([instructions](https://thin-edge.github.io/thin-edge.io/install/#community-plugins)):

```sh
# Debian / Ubuntu
sudo apt-get install -y tedge-dot-c         # or tedge-dot-rs
sudo systemctl status tedge-dot

# RPM distros
sudo dnf install tedge-dot-c                # or tedge-dot-rs
```

## Install a downloaded package

Every [release](https://github.com/thin-edge/tedge-dot/releases) attaches `.deb`, `.rpm` and
`.apk` packages for each architecture, and one `SHA256SUMS` covering every asset. The package
manager still resolves `tedge-parameter-plugin` from the community repository, so set that up
first:

```sh
sudo apt-get install -y ./tedge-dot-c_*_amd64.deb           # or ./tedge-dot-rs_*_linux_amd64.deb
sudo dnf install ./tedge-dot-c_*_amd64.rpm                  # or ./tedge-dot-rs_*_linux_amd64.rpm
```

> **Alpine:** the attached `.apk` files carry a version string apk-tools rejects
> (`apk version -c` refuses both this project's tag format and the snapshot form), so `apk add`
> will not install them. Use the tarball on Alpine until that is fixed (see TODO.md). Also, apk
> does not carry the `conflicts` field, so the two packages are not mutually exclusive there:
> remove one with `apk del` before installing the other rather than forcing the overwrite of
> `/usr/bin/tedge-dot`.

## Run from a tarball

The tarballs hold the binary, which doubles as a one-shot read/write CLI, plus the device-side
flows. The `tedge-dot-c` tarball also carries the default configs to start from:

```sh
./tedge-dot read -c config-defaults/modbus.toml --json
```

## After installing

- The service starts with **no devices configured**. Add `[[device]]` sections under
  `/etc/tedge/plugins/ot/` (each file documents the syntax), or copy a demo config from
  `/usr/share/tedge-dot/demo/`. The service picks the change up within a few seconds; see
  [Install](../README.md#install) in the README for how reloads work.
- The connector is a dumb driver: the device-side **flows** that map its envelopes onto the
  thin-edge data model are deployed active into `/etc/tedge/mappers/c8y/flows/`, with the opt-in
  alarm and event flows in `/usr/share/tedge-dot/flows/`. See [flows/README.md](../flows/README.md).

## Upgrading

An upgrade keeps your `/etc/tedge/plugins/ot/` configuration. Packaged default configs only
change on a fresh install, so a new default (for example the `report = { max_interval = "30m" }`
heartbeat the Modbus, OPC UA, CANopen and PROFIBUS defaults set) has to be added to an existing
file by hand. Check the release notes of every version you skip for such changes.

### From a release before the package split

The package formerly called `tedge-dot` is now **`tedge-dot-rs`**. Both packages `Replaces:`
the old name, so installing either over an existing `tedge-dot` works and keeps your
configuration:

```sh
sudo apt-get install -y tedge-dot-c    # or tedge-dot-rs: the same Rust build as the old package
```

Two caveats:

- `apt install tedge-dot` no longer resolves: `tedge-dot` is now a *virtual* package provided
  by both, so apt cannot choose. Install by the real name, and update scripts and runbooks that
  use the old one.
- A plain `apt upgrade` will **not** migrate an existing `tedge-dot` install. It stays on the old
  package name and receives no further updates until you install one of the new ones
  explicitly.
