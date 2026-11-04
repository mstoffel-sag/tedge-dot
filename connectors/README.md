# Connectors

Each OT protocol connector lives in its own subdirectory here. All connectors
share a common layout, and the `just` recipes pick up any protocol by name —
no justfile changes are ever needed to add a new one.

---

## Directory layout

```
connectors/
  _shared/                      # files shared across all connectors
    stack.resource              # stack lifecycle: DeviceLibrary starts/stops the compose stack
    MqttClient.py               # Robot keyword library (paho-mqtt subscribe/assert)
    mosquitto.conf              # mosquitto config used by the bridged stacks
    mosquitto-anon.conf         #   ... and by the host-networked ones (port from the CLI)
    requirements.txt            # base Robot deps (robotframework, paho-mqtt, DeviceLibrary)
    Dockerfile.flows            # cloud-free flows runner: tedge (main channel) as the
    flows-entrypoint.sh         #   user-defined mapper "ot" running ../flows against the broker
    Dockerfile.connector-c      # the C implementation (impl/c/) built for any stack (ARG PROTOCOL)

  <proto>/                      # one directory per OT protocol
    sim/                        # simulator image (Dockerfile + server code)
      Dockerfile
      ...
    tests/
      <proto>_e2e.robot         # Robot Framework e2e suite
    docker-compose.yaml         # stack: broker, simulator, connector (+ optional flows runner)
    Dockerfile.connector        # builds the Rust connector binary for e2e
    connector.toml              # connector config used inside the container
    entrypoint.sh               # waits for deps, then execs tedge-dot
    conformance.toml            # conformance-suite manifest (optional, see doc/conformance/)
    requirements.txt            # protocol-specific extra Python deps (optional)
```

The configs the package installs live outside this directory:
`packaging/config/<proto>.toml` (empty defaults installed to
`/etc/tedge/plugins/ot/`) and `demo/config/<proto>.toml` (demo configs shipped
to `/usr/share/tedge-dot/demo/`, also used for local CLI exploration).

### The suite owns the stack

Nothing has to be running before the tests: each suite's `Suite Setup` calls
`Setup OT Stack` from [`_shared/stack.resource`](_shared/stack.resource), which hands the
protocol's `docker-compose.yaml` to **DeviceLibrary**. Every setup gets its own compose
project, named after a randomly generated device serial, with its own network and volumes — so
suites are isolated, parallel runs cannot clash, and the stack is torn down when the suite
ends. Moving the keyword to `Test Setup` gives a fresh stack per test case instead (slower;
the per-test containers are reclaimed at suite end).

The same keyword resolves the broker endpoint, so no suite hardcodes a port:

| Stack | Broker | Why |
|---|---|---|
| modbus, opcua, profibus, snmp, aveva-pi | published on an **ephemeral** host port, resolved with `Get Service Port` | parallel-safe |
| canbus, canopen | host network namespace, fixed port (13883 / 13884), no published port | the SocketCAN connector and simulator need the host's `vcan0`, so they reach the broker over the host loopback; one stack per protocol per host |

Host ports are pinned only for manual work, through env vars the compose files interpolate
(`BROKER_PORT`, `<PROTO>_SIM_PORT`): `just sim <proto>` pins the simulator port the demo
configs expect, `just e2e-up <proto>` pins the broker on 1884. Fixed published ports must not
be committed to the compose files — DeviceLibrary rejects them, since they break parallel runs.

### Several devices and connector instances (modbus)

`modbus/docker-compose.multi-device.yaml` is a second modbus stack: one `tedge-dot` process
running a directory of ten connector configs (one instance per file, service `tedge-dot-N`
owning device `plc-N`) against ten simulated devices. `modbus/tests/multi_device_e2e.robot`
starts it with `Setup OT Stack    modbus    compose_file=...` and checks that every command is
acted on by exactly one instance (contract §6.5).

Both halves are switches on the regular images, usable on their own:

| Setting | Where | Effect |
|---|---|---|
| `SIM_DEVICES=N` (1–32) | simulator | serves N independent devices: device N on port 501+N, own datastore, the device number in holding register 2100 |
| `MULTI_DEVICE_COUNT=N` | connector | renders `modbus/multi-device/device.toml.template` once per device into `/etc/tedge-dot/multi-device/` and runs that directory |

---

## `just` recipes

All recipes take the protocol name as their first argument.

```sh
just sim modbus            # start only the simulator, on the demo config's fixed port
just sim-down modbus       # stop the simulator

just test-e2e modbus       # run the robot suite (it starts and stops its own stack)
just test-e2e modbus --include smoke   # pass extra robot args
just test-e2e-c modbus     # the SAME suite against the C connector (impl/c/)

just e2e-up modbus [c]     # start a stack manually (ports pinned) for inspection
just e2e-down modbus [c]   # tear that manual stack down
```

### Rust and C: one suite, two connectors

The Rust crates and the C implementation ([impl/c/](../impl/c/)) implement the same
contract and are maintained to the same coverage. Every stack therefore runs its Robot suite
against both: `test-e2e` builds the stack's `Dockerfile.connector` (Rust), `test-e2e-c` sets
`CONNECTOR_DOCKERFILE` so the same compose file builds the `connector` service from
[`_shared/Dockerfile.connector-c`](_shared/Dockerfile.connector-c) instead — the C build
with only that protocol's module compiled in, installed with the stack's own `connector.toml`
and `entrypoint.sh`. Robot output goes to `output/` and `output-c/` respectively, and the
suite receives `${IMPL}` (`rust`/`c`) should a case ever need to differ (none does today).
CI runs both matrices (`e2e` and `e2e-c`).

The **cloud** suites under [cloud/](../cloud/) work the same way, with one difference: there is a
single `Dockerfile.tedge` whose `IMPL` build argument selects one of two connector-install
stages — the packaged `.deb` from `dist/` (`rust`) or a stage that compiles [impl/c/](../impl/c/)
in the image (`c`). Everything after the install (flows, configs, operation shims) is shared, so
both implementations are exercised by the same tests, and the C path needs no `just build`
because `dist/` is never read. `Setup Cloud Device` asserts that the image really holds the
requested implementation, so an unexported `IMPL` cannot silently produce a green "C" run.

```sh

just cloud-up modbus       # bring up cloud (Cumulocity) stack and bootstrap
just cloud-up modbus c     # ... with the C connector instead
just cloud-down modbus     # tear it down
just test-cloud modbus     # full cloud e2e run (requires C8Y_* env vars)
just test-cloud-c modbus   # the same run against the C connector (output-c/)
```

### One virtualenv, shared with the editor

All system tests (these suites and the [cloud](../cloud/) ones) use a single virtualenv at the
repo root, built from [`requirements-test.txt`](../requirements-test.txt):

```sh
just venv          # create/refresh ./.venv (the test-e2e / test-cloud recipes call it too)
```

[`.vscode/settings.json`](../.vscode/settings.json) points the Python and Robot Framework
extensions at that same `./.venv`, so **Run/Debug Test on a single test case in the editor uses
exactly what the `just` recipes use**. From the command line:

```sh
# one test case (it still starts the stack the suite needs, then tears it down)
./.venv/bin/python -m robot --test "Parameter Twin Follows The Device" connectors/modbus/tests/
# everything tagged `flows` (the command/parameter bridge cases)
./.venv/bin/python -m robot --include flows connectors/modbus/tests/
# keep the stack running afterwards to poke at it (the setup logs the project name)
./.venv/bin/python -m robot --variable KEEP_STACK:true --test "Parameter Twin Follows The Device" connectors/modbus/tests/
```

To run or debug a test against the **C connector** instead of the Rust one, select the `c`
profile from [`robot.toml`](../robot.toml):

- **VS Code**: command palette → *RobotCode: Select Configuration Profiles* → `c`. The choice
  applies to Run/Debug Test in the test explorer and to the editor's gutter actions, so the
  interactive debugger attaches to a stack built from [impl/c/](../impl/c/). Switch back by
  selecting `rust` (or deselecting).
- **CLI**: `robotcode --profile c run -- -t "<test>" connectors/modbus/tests/`, or just
  `just test-e2e-c <proto>` for the whole suite. The same profile works for the cloud suites
  (`robotcode --profile c run -- -t "<test>" cloud/modbus/tests/`), which read `IMPL` only.

The profile only sets `IMPL` and `CONNECTOR_DOCKERFILE`, which the compose file interpolates
into the connector service's image name and build recipe; each implementation therefore has its
own image and the first run after switching rebuilds it. The suite setup logs which one is in
use (`Connector under test: c implementation …`), so the Robot log always says which binary
answered. Both builds report the same capability `version` — they implement the same contract
revision — so the descriptor deliberately does NOT distinguish them; the cloud suites assert
the implementation separately by which package is installed (see
[cloud/_shared/device.resource](../cloud/_shared/device.resource)).

Robot must run from the repo root (the default in VS Code) so the compose files, `robot.toml`
and `.env` are found. If `just venv` reports that it is recreating the environment, the `./.venv` directory had
been copied from another checkout — its `pip` would have installed into *that* project.

---

## Adding a new protocol

> See [doc/connectors/_template-connector-spec.md](../doc/connectors/_template-connector-spec.md)
> for the full connector spec template and a detailed checklist.

1. **Rust crate** — create `impl/rust/crates/connector-<proto>/` and implement the
   [`Connector` trait](../doc/sdk/connector-sdk.md).  Add a cargo feature flag
   in `impl/rust/Cargo.toml`.

2. **Simulator** — create `connectors/<proto>/sim/` with a `Dockerfile` and
   whatever server code the protocol needs.

3. **Docker stack** — create `connectors/<proto>/docker-compose.yaml`,
   `Dockerfile.connector`, `connector.toml`, and `entrypoint.sh`. Copy an existing
   compose file: publish ports **without** a fixed host port (interpolate
   `${...:-}` for the manual overrides), and label the connector service
   `device-test-core.role: main` so DeviceLibrary knows which service is the device
   under test. Add the `flows` service (see `connectors/modbus/docker-compose.yaml`,
   build arg `PROTOCOL`) to run the repo's flows in the stack and test the
   command/parameter bridges end to end; tag those Robot cases `flows`.

4. **Tests** — create `connectors/<proto>/tests/<proto>_e2e.robot`. Import the shared
   resource (it brings DeviceLibrary, the MQTT keywords and the stack lifecycle) and let
   it start the stack:
   ```robotframework
   Resource            ../../_shared/stack.resource
   Suite Setup         Setup OT Stack    <proto>
   Suite Teardown      Teardown OT Stack
   ```

5. **Packaging configs** — create `packaging/config/<proto>.toml` (sane
   defaults: no devices, correct `protocol` and `service_name`) and
   `demo/config/<proto>.toml` (pre-wired to the simulator), and add both
   to the `nfpms.contents` list in `.goreleaser.yaml`.

That's it. `just sim <proto>` and `just test-e2e <proto>` work immediately.
