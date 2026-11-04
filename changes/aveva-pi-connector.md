### AVEVA PI Web API connector (preview), and replayed history in the reporting policy

A new `aveva-pi` connector reads PI points and AF attributes from an AVEVA PI System through the
PI Web API, in both builds. It replaces the community
[thin-edge-aveva-pi-connector](https://github.com/Cumulocity-IoT/thin-edge-aveva-pi-connector).
See the [AVEVA PI guide](https://thin-edge.github.io/tedge-dot/guides/aveva-pi/) and
[doc/connectors/aveva-pi-connector-spec.md](../doc/connectors/aveva-pi-connector-spec.md).

The connector is a **preview**: it is tested in CI against the project's PI Web API simulator,
but not yet against a real PI System, so expect rough edges and changes to its configuration.
If you run it against a real server, please
[tell us how it went](https://github.com/thin-edge/tedge-dot/issues).

- Points are addressed by tag, PI point path, AF attribute path or WebId. WebIds are looked up
  once, and one request reads all of a device's points.
- Values are pushed over the PI Web API Channels WebSocket with their PI timestamps, or polled
  from the archive (`delivery = "recorded"`), or read as current values (`delivery = "snapshot"`).
- After an outage or a restart, each pushed point catches up from the PI archive, from a cursor
  kept in `/var/lib/tedge-dot/aveva-pi`, bounded by `backfill.max_age` and `backfill.max_events`.
- Writes post a value to the PI stream, so PI points can be Cumulocity device parameters.
- TLS is verified by default. Authentication is Basic, Bearer or a client certificate, with
  secrets read from files that management commands cannot change, and never over plain HTTP.
  Kerberos is not supported.
- The packages install `/etc/tedge/plugins/ot/aveva-pi.toml` with no devices. Users of the
  community plugin: see [doc/migration/aveva-pi-plugin.md](../doc/migration/aveva-pi-plugin.md),
  which includes a script that converts its configuration.
- **Reporting policy (all connectors):** a module can mark pushed samples as replayed history.
  `min_interval` and `debounce` then measure time on the samples' source time, so a backlog is
  reduced the way the live stream would have been instead of collapsing to two values. The OT
  Connector Contract gains rule 7 in §5.3.

AVEVA, PI System and PI Web API are trademarks of AVEVA Group Limited or its subsidiaries.
