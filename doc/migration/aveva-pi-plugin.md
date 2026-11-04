# Migrating from thin-edge-aveva-pi-connector

The community [thin-edge-aveva-pi-connector](https://github.com/Cumulocity-IoT/thin-edge-aveva-pi-connector)
is a Python container that polls an AVEVA PI Web API server and publishes measurements to
thin-edge.io. tedge-dot's `aveva-pi` connector replaces it. This guide maps the plugin's
configuration onto the connector and lists what changes.

The connector is a **preview**: it is tested in CI against the project's PI Web API simulator,
not yet against a real PI System. Run it beside the plugin on a test system before you switch,
and [tell us how it went](https://github.com/thin-edge/tedge-dot/issues).

AVEVA, PI System and PI Web API are trademarks of AVEVA Group Limited or its subsidiaries.

## What you gain

| Plugin | `aveva-pi` connector |
| --- | --- |
| Two HTTP requests per tag per poll (a point lookup, then a read) | WebIds looked up once per connection; one request reads all of a device's points |
| Current value at each poll; events between polls are lost | Every archived event, pushed over Channels with its PI timestamp |
| An outage loses its data | The gap is filled from the PI archive after an outage or a restart |
| `verify=False`: TLS certificates are not checked | TLS verified against the system store or your CA bundle |
| Password in `pi_config.json`, base64-encoded | Password, token or key in a file the config names; client certificates and Bearer tokens supported |
| Read only | Writes, and PI points as Cumulocity device parameters |
| A batch ledger file to stay under 16 KB per message | Measurement output (`output = "measurements"`) batches samples |
| Measurements on the gateway device | Measurements on a child device per PI Web API endpoint, with a link status and availability |

## Configuration mapping

| Plugin | Connector |
| --- | --- |
| `pi_config.json` `PI_URL` | `[[device]] protocol_address.base_url`. With a username and password it must be `https://`: the connector refuses to send credentials over plain HTTP |
| `PI_USER`, `PI_PASSWORD` | `[connection] auth = { method = "basic", username = "...", password_file = "..." }`; the password goes into the file, in clear text, mode 0600 |
| `POLL_INTERVAL` | `[connector] poll_interval` (snapshot delivery); with Channels, values arrive as PI archives them |
| `RECORDING_AT_TIME` | Not needed: the connector reads current values with `streamsets/value` and history with `streamsets/recorded` |
| `datapoints.json` (a list of tag names) | One `[[device.point]]` per tag, `address = { tag = "<TAG>" }`, with a `datatype` |
| `tag_batches.json` ledger | `output = "measurements"` |

The plugin found each tag by name across the server. The connector needs the PI Data Archive
the tags live in (`protocol_address.data_server`); a tag on another server is addressed with its
full path, `address = { path = '\\OTHERSRV\TAG' }`.

### Generate the config

`connectors/aveva-pi/tools/plugin-to-tedge-dot.py` reads the plugin's two files, asks the PI
Web API for each tag's point type (once), and prints a connector config with a datatype for every
point:

```bash
python3 connectors/aveva-pi/tools/plugin-to-tedge-dot.py \
    --pi-config config/pi_config.json --datapoints config/datapoints.json \
    --device pi-plant1 --ca-file /etc/tedge/plugins/ot/certs/pi-ca.pem > aveva-pi.toml
```

It writes no password: put it into the file the output names, then install both:

```bash
sudo install -d -m 0700 -o tedge -g tedge /etc/tedge/plugins/ot/secrets
sudo install -m 0600 -o tedge -g tedge /dev/stdin /etc/tedge/plugins/ot/secrets/aveva-pi.password
sudo install -m 0640 -o tedge -g tedge aveva-pi.toml /etc/tedge/plugins/ot/aveva-pi.toml
```

The generated config keeps the plugin's `POLL_INTERVAL` and uses the connector's default
delivery, Channels. Set `delivery = "snapshot"` in `protocol_address` to keep the plugin's behavior
of one current value per poll.

## What changes in Cumulocity

The connector reads the same PI tags, but its measurements reach Cumulocity on another device
and under another type, so they are new series there. The plugin's history stays on the gateway
device; dashboards and data exports that read it need to point at the new device and type.

| | Plugin | Connector |
| --- | --- | --- |
| Device | The gateway (main device): the plugin publishes to `c8y/measurement/measurements/create` | A child device per `[[device]]`, external id `<gateway>:device:<name>`, registered by the `ot-registration` flow |
| Measurement type and fragment | `pi_historianMeasurement_batch<N>`, from the plugin's batch ledger | `aveva-pi` (the protocol), or a point's `meta.measurement.group` |
| Series | Tag name with `.` replaced by `_` | The point id; the script writes the same names |
| Unit | PI `EngineeringUnits`, in every measurement | The same unit, published once as metadata; the c8y mapper adds it to the series |
| Digital tag | `value` = state code, plus `stringValue` = state name | `value` = state code (`datatype = "int32"`); the name is not sent |
| Tag metadata | Twin fragments `pi_historianMetadata_batch<N>` on the gateway, holding each tag's PI Descriptor | None |

Point ids are the tag names with `.` replaced by `_`. Two tags that differ only in `.` and `_`
get the same id; the script warns about them.

A digital tag stays a numeric series of its state codes. The script writes it as `int32`: a
`string` point carries PI's state name, which the measurement flow does not publish. A PI digital
state set is an enumeration the server already holds, so the connector reads the names from PI
and needs no `map` for them. To have both, configure the tag twice: an `int32` point for the
series and a `string` point for the name (for a device parameter, an alarm or an event):

```toml
  [[device.point]]
  id       = "PUMP01_STATE"
  datatype = "int32"
  address  = { tag = "PUMP01.STATE" }

  [[device.point]]
  id       = "PUMP01_STATE_name"
  datatype = "string"
  address  = { tag = "PUMP01.STATE" }
```

The plugin looks tags up with `PointType:Float32` by default, so it only found Float32 points. The
connector finds every point type, so a datapoints list may now produce series that the plugin
silently skipped.

## Timestamps

The plugin stamped values with the time it read them. With Channels (the default) and recorded
delivery, a sample carries the time PI archived the value, so Cumulocity shows when it happened,
not when it was read. With `delivery = "snapshot"` the read time is kept, and the PI time is in
the sample's `addr.timestamp`.

## Authentication

PI Web API servers that only allow Kerberos are not supported. Enable Basic authentication on the
PI Web API (the plugin needed it too), or OpenID Connect and use a Bearer token from a file that
your token refresher updates.

## TLS

The plugin did not check certificates. The connector does, and some PI Web API installations use a
self-signed IIS certificate without a subject alternative name, which TLS libraries refuse today.
The link status then reads `certificate not valid for name ...` (Rust) or names the verification
failure (C). Issue a certificate whose SAN holds the host name the gateway uses, and name its CA in
`tls.ca_file`. `tls.insecure_skip_verify = true` exists for a test bench; it is logged at `warn` on
every connect.

## Rollback

Stop or remove `/etc/tedge/plugins/ot/aveva-pi.toml` and start the plugin's container again. The
connector keeps no state outside `/var/lib/tedge-dot/aveva-pi`.
