"""OPC-UA simulator for the tedge-dot e2e harness.

Exposes a handful of nodes with stable string NodeIds under namespace index 2
(urn:tedge:opcua-sim) so the connector can address them as `ns=2;s=<name>`:

    ns=2;s=Temperature   Double  21.5            (read)
    ns=2;s=Count         UInt32  617001          (read)
    ns=2;s=Setpoint      Int32   0   (writable)  (read/write round-trip)
    ns=2;s=Running       Boolean false (writable)(read/write round-trip)
    ns=2;s=Ticks         UInt32  incremented every second (subscription/push tests)
    ns=2;s=Stamped       Double  incremented every second, always with the source timestamp
                                 2026-01-01T00:00:00.913Z (source-time tests, contract §5.4)

Reading ns=2;s=DoesNotExist yields a Bad status, exercising bad-quality handling.

Methods on the ns=2;s=Machine object (openspec change opcua-method-calls):

    ns=2;s=Machine.Multiply  (Int32, Int32) -> Int32   the product
    ns=2;s=Machine.Ping      () -> String              "pong"
    ns=2;s=Machine.Fail      () -> ()                  always fails with BadInvalidState

The endpoint host is taken from OPCUA_ENDPOINT_HOST so the advertised endpoint URL
matches the docker service name (avoids OPC-UA hostname-rewrite connection failures).

OPCUA_SIM_DYNAMIC=1 makes Temperature drift (21.5 +/- 2.5 over a 5 minute cycle) and
Count increment every second. The demo stacks enable it: a subscription only notifies
on change, so with static values a push-delivered device goes silent after its first
notification and the cloud marks it unavailable. Off by default because the e2e and
smoke tests assert the static values above.

OPCUA_SIM_BULK=<n> (the load-simulator of docker-compose.yaml) adds n static Double nodes
ns=2;s=Bulk0 .. Bulk<n-1> (value = i), for a device with thousands of subscribed points, and
ns=2;s=Burst, a UInt32 written five times in a row every second (10n .. 10n + 4): faster than
any publishing interval, so the monitored item's queue size decides how many arrive.

NAMESPACE ORDER (openspec change opcua-namespace-table, docker-compose.namespaces.yaml): the
simulator's namespace index is normally 2. Two switches change it, for the namespace-table e2e
suite:

    OPCUA_SIM_EXTRA_NAMESPACES  comma-separated URIs registered BEFORE urn:tedge:opcua-sim, so
                                its nodes move to a higher index (one per URI)
    OPCUA_SIM_CONTROL_DIR       a directory watched by the running simulator:
                                  shift      present at start: register urn:tedge:opcua-sim:shift
                                             first (the nodes move from ns=2 to ns=3) -- create
                                             it, then restart the container
                                  add-late   once present: register urn:tedge:opcua-sim:late
                                             while running, with ns=<new>;s=Late (Double 7.5)

SECURED MODE (connectors/opcua/docker-compose.secure.yaml): with OPCUA_SECURE_SERVERS set,
the container instead serves one server per PKI scenario, each on its own port, all with the
address space above:

    OPCUA_PKI_VECTORS      directory for the genpki.py tree (generated when it has no
                           expected.json yet; a volume shared with the connector)
    OPCUA_CERT_HOSTNAMES   comma-separated SAN host names of the valid server certificates
    OPCUA_SECURE_SERVERS   comma-separated <port>=<scenario>: the server on <port> presents
                           scenarios/<scenario>/server/cert.der
    OPCUA_SECURE_POLICIES  comma-separated asyncua SecurityPolicyType names they offer
    OPCUA_PLAIN_PORT       an extra server offering only NoSecurity (a password sent to it
                           travels in plaintext)
    OPCUA_USERS            comma-separated <user>:<password> accepted by every server; the
                           X.509 user users/operator.der is accepted too
"""

import asyncio
import math
import os
import uuid
from datetime import datetime, timezone

from asyncua import Server, ua, uamethod
from asyncua.common.structures104 import new_enum, new_struct, new_struct_field
from asyncua.crypto import uacrypto
from asyncua.server.user_managers import UserManager
from asyncua.crypto.permission_rules import User, UserRole

# The source timestamp of every write to ns=2;s=Stamped.
STAMPED_SOURCE_TIME = datetime(2026, 1, 1, 0, 0, 0, 913000, tzinfo=timezone.utc)

ENDPOINT_HOST = os.environ.get("OPCUA_ENDPOINT_HOST", "0.0.0.0")
CONTROL_DIR = os.environ.get("OPCUA_SIM_CONTROL_DIR", "")
SHIFT_URI = "urn:tedge:opcua-sim:shift"
LATE_URI = "urn:tedge:opcua-sim:late"
LATE = 7.5
DYNAMIC = os.environ.get("OPCUA_SIM_DYNAMIC", "").strip().lower() in ("1", "true", "yes", "on")
BULK = int(os.environ.get("OPCUA_SIM_BULK", "0") or 0)
NS_URI = "urn:tedge:opcua-sim"
# The application URI of a secured server (its certificate's URI SAN). Distinct from NS_URI,
# which would otherwise take namespace index 1 and move the nodes off ns=2.
SERVER_URI = "urn:tedge:opcua-sim:server"

TEMPERATURE = 21.5
COUNT = 617001


class SimUserManager(UserManager):
    """Anonymous sessions, the OPCUA_USERS passwords and one trusted X.509 user."""

    def __init__(self, users, user_certificate=None):
        self.users = users
        self.user_certificate = user_certificate

    def get_user(self, iserver, username=None, password=None, certificate=None):
        # asyncua passes the channel's CLIENT certificate for every token type on a secured
        # channel, and the verified user certificate for an X.509 token -- so a certificate
        # that is not the user's is an anonymous (or username) session.
        if username is not None:
            if self.users.get(username) == password:
                return User(role=UserRole.Admin)
            return None
        return User(role=UserRole.Admin)


def parse_users(text):
    users = {}
    for entry in filter(None, (e.strip() for e in text.split(","))):
        user, _, password = entry.partition(":")
        users[user] = password
    return users


async def build_server(port, policies, user_manager=None, cert=None, key=None):
    """A server with the simulator's address space; returns (server, nodes)."""
    server = Server(user_manager=user_manager)
    await server.init()
    server.set_endpoint(f"opc.tcp://{ENDPOINT_HOST}:{port}/")
    server.set_server_name("tedge OPC-UA simulator")
    if cert and key:
        await server.set_application_uri(SERVER_URI)
        await server.load_certificate(cert)
        await server.load_private_key(key)
    server.set_security_policy(policies)
    if user_manager is not None:
        server.set_identity_tokens(
            [ua.AnonymousIdentityToken, ua.UserNameIdentityToken, ua.X509IdentityToken]
        )

    extra = [u.strip() for u in os.environ.get("OPCUA_SIM_EXTRA_NAMESPACES", "").split(",") if u.strip()]
    if CONTROL_DIR and os.path.exists(os.path.join(CONTROL_DIR, "shift")):
        extra.append(SHIFT_URI)
    for uri in extra:
        await server.register_namespace(uri)
    idx = await server.register_namespace(NS_URI)
    plc = await server.nodes.objects.add_object(
        ua.NodeId("Plc", idx), ua.QualifiedName("Plc", idx)
    )

    temperature = await plc.add_variable(
        ua.NodeId("Temperature", idx), ua.QualifiedName("Temperature", idx), TEMPERATURE
    )
    count = await plc.add_variable(
        ua.NodeId("Count", idx),
        ua.QualifiedName("Count", idx),
        ua.Variant(COUNT, ua.VariantType.UInt32),
    )
    setpoint = await plc.add_variable(
        ua.NodeId("Setpoint", idx),
        ua.QualifiedName("Setpoint", idx),
        ua.Variant(0, ua.VariantType.Int32),
    )
    running = await plc.add_variable(
        ua.NodeId("Running", idx), ua.QualifiedName("Running", idx), False
    )
    await setpoint.set_writable()
    await running.set_writable()

    # Changes every second so OPC-UA subscriptions (monitored items) have data-change
    # notifications to deliver; the static nodes above only ever notify once.
    ticks = await plc.add_variable(
        ua.NodeId("Ticks", idx),
        ua.QualifiedName("Ticks", idx),
        ua.Variant(0, ua.VariantType.UInt32),
    )
    # A source timestamp the connector cannot have made up: every write carries the same one.
    stamped = await plc.add_variable(
        ua.NodeId("Stamped", idx), ua.QualifiedName("Stamped", idx), 0.0
    )
    nodes = {"idx": idx, "temperature": temperature, "count": count, "ticks": ticks,
             "stamped": stamped}
    if BULK:
        for i in range(BULK):
            await plc.add_variable(ua.NodeId(f"Bulk{i}", idx), ua.QualifiedName(f"Bulk{i}", idx), float(i))
        nodes["burst"] = await plc.add_variable(
            ua.NodeId("Burst", idx),
            ua.QualifiedName("Burst", idx),
            ua.Variant(0, ua.VariantType.UInt32),
        )
    nodes.update(await add_structures(server, plc, idx))
    await add_methods(server, idx)
    return server, nodes


@uamethod
def multiply(parent, a, b):
    return ua.Variant(a * b, ua.VariantType.Int32)


@uamethod
def ping(parent):
    return "pong"


def fail(parent):
    # Returned, not raised: asyncua answers any exception from a method with BadUnexpectedError,
    # but passes a returned StatusCode through as the call's status. Hence no @uamethod either.
    return ua.StatusCode(ua.StatusCodes.BadInvalidState)


async def add_methods(server, idx):
    """The Machine object and the methods the call-method e2e cases call."""
    machine = await server.nodes.objects.add_object(
        ua.NodeId("Machine", idx), ua.QualifiedName("Machine", idx)
    )
    method = lambda name: (ua.NodeId(f"Machine.{name}", idx), ua.QualifiedName(name, idx))
    await machine.add_method(
        *method("Multiply"), multiply, [ua.VariantType.Int32, ua.VariantType.Int32], [ua.VariantType.Int32]
    )
    await machine.add_method(*method("Ping"), ping, [], [ua.VariantType.String])
    await machine.add_method(*method("Fail"), fail, [], [])


# The structured values read by the `opc3` device of connector.toml (openspec change
# opcua-custom-datatypes). Every type is created with new_struct / new_enum, which publish its
# DataTypeDefinition: what the connector reads to decode a field without compiled-in types.
STAMP = datetime(2026, 10, 6, 8, 15, 30, 250000, tzinfo=timezone.utc)
UID = uuid.UUID("72962b91-fa75-4ae6-8d28-b404dc7daf63")


async def add_structures(server, plc, idx):
    motor, _ = await new_struct(server, idx, "SimMotor", [
        new_struct_field("Current", ua.VariantType.Float),
        new_struct_field("Temp", ua.VariantType.Float),
    ])
    mode = await new_enum(server, idx, "SimMode", ["Stopped", "Running", "Fault"])
    item, _ = await new_struct(server, idx, "SimItem", [
        new_struct_field("Name", ua.VariantType.String),
        new_struct_field("Value", ua.VariantType.Double),
    ])
    await new_struct(server, idx, "SimPumpStatus", [
        new_struct_field("Running", ua.VariantType.Boolean),
        new_struct_field("Speed", ua.VariantType.Double),
        new_struct_field("Motor", motor),
        new_struct_field("Label", ua.VariantType.String),
        new_struct_field("Mode", mode),
        new_struct_field("Samples", ua.VariantType.Double, array=True),
        new_struct_field("Stamp", ua.VariantType.DateTime),
        new_struct_field("Items", item, array=True),
        new_struct_field("Comment", ua.VariantType.String, optional=True),
        new_struct_field("Tail", ua.VariantType.UInt16),
    ])
    await new_struct(server, idx, "SimChoice", [
        new_struct_field("Name", ua.VariantType.String),
        new_struct_field("Count", ua.VariantType.UInt32),
    ], is_union=True)
    await server.load_data_type_definitions()

    async def add(name, value, variant_type=None, data_type=None):
        variant = ua.Variant(value, variant_type) if variant_type else ua.Variant(value)
        kwargs = {"datatype": data_type} if data_type else {}
        return await plc.add_variable(ua.NodeId(name, idx), ua.QualifiedName(name, idx), variant, **kwargs)

    pump = ua.SimPumpStatus(
        Running=True, Speed=1450.0, Motor=ua.SimMotor(Current=3.25, Temp=41.5), Label="P1",
        Mode=ua.SimMode.Running, Samples=[1.0, 2.0, 4.5], Stamp=STAMP,
        Items=[ua.SimItem(Name="first", Value=1.5), ua.SimItem(Name="second", Value=7.0)],
        Comment=None, Tail=7,
    )
    await add("Pump", pump, ua.VariantType.ExtensionObject, ua.SimPumpStatus.data_type)
    choice = ua.SimChoice()
    choice.Count = 5
    await add("Choice", choice, ua.VariantType.ExtensionObject, ua.SimChoice.data_type)
    drive = await add("Drive", ua.SimMotor(Current=0.0, Temp=40.0), ua.VariantType.ExtensionObject,
                      ua.SimMotor.data_type)
    await add("Temperatures", [20.0, 21.0, 22.5, 23.0], ua.VariantType.Double)
    levels = await add("Levels", [0.0, 0.0, 0.0], ua.VariantType.Double)
    await add("Serviced", STAMP, ua.VariantType.DateTime)
    await add("State", ua.LocalizedText("Betrieb", "de-DE"), ua.VariantType.LocalizedText)
    await add("LastError", ua.StatusCode(0x80340000), ua.VariantType.StatusCode)
    await add("DeviceUid", UID, ua.VariantType.Guid)
    await add("Ref", ua.NodeId("Pump", idx), ua.VariantType.NodeId)
    await add("Blob", b"\xde\xad\xbe\xef", ua.VariantType.ByteString)
    return {"drive": drive, "levels": levels}


def secured_servers():
    """The (port, policies, cert, key) of every server of the secured mode."""
    import subprocess
    import sys

    vectors = os.environ["OPCUA_PKI_VECTORS"]
    if not os.path.exists(os.path.join(vectors, "expected.json")):
        hosts = [h for h in os.environ.get("OPCUA_CERT_HOSTNAMES", "").split(",") if h]
        cmd = [
            sys.executable,
            os.path.join(os.path.dirname(__file__), "genpki.py"),
            vectors,
            "--application-uri",
            SERVER_URI,
        ]
        for h in hosts:
            cmd += ["--hostname", h]
        subprocess.run(cmd, check=True)
        print(f"generated the PKI vectors in {vectors}", flush=True)
    policies = [
        getattr(ua.SecurityPolicyType, name.strip())
        for name in os.environ.get("OPCUA_SECURE_POLICIES", "Basic256Sha256_SignAndEncrypt").split(",")
        if name.strip()
    ]
    servers = []
    for entry in filter(None, (e.strip() for e in os.environ["OPCUA_SECURE_SERVERS"].split(","))):
        port, _, scenario = entry.partition("=")
        base = os.path.join(vectors, "scenarios", scenario, "server")
        servers.append((int(port), policies, os.path.join(base, "cert.der"), os.path.join(base, "key.pem")))
    plain = os.environ.get("OPCUA_PLAIN_PORT")
    if plain:
        servers.append((int(plain), [ua.SecurityPolicyType.NoSecurity], None, None))
    return vectors, servers


async def main():
    users = parse_users(os.environ.get("OPCUA_USERS", ""))
    if os.environ.get("OPCUA_SECURE_SERVERS"):
        vectors, specs = secured_servers()
        user_cert = await uacrypto.load_certificate(os.path.join(vectors, "users", "operator.der"))
        manager = SimUserManager(users, user_cert)
    else:
        specs = [(4840, [ua.SecurityPolicyType.NoSecurity], None, None)]
        manager = SimUserManager(users) if users else None

    servers = []
    for port, policies, cert, key in specs:
        servers.append(await build_server(port, policies, manager, cert, key))
    for server, nodes in servers:
        await server.start()
        print(
            f"OPC-UA simulator listening on {server.endpoint.geturl()} "
            f"(namespace idx={nodes['idx']}, dynamic={DYNAMIC})",
            flush=True,
        )
    late_added = False
    try:
        n = 0
        while True:
            await asyncio.sleep(1)
            n += 1
            if CONTROL_DIR and not late_added and os.path.exists(os.path.join(CONTROL_DIR, "add-late")):
                # A namespace that appears while clients are connected: OPC UA allows a server
                # to add entries to its NamespaceArray during a session (10000-5 6.3.1).
                server, _nodes = servers[0]
                late = await server.register_namespace(LATE_URI)
                await server.nodes.objects.add_variable(
                    ua.NodeId("Late", late), ua.QualifiedName("Late", late), LATE
                )
                late_added = True
                print(f"registered {LATE_URI} at runtime (namespace idx={late})", flush=True)
            for _server, nodes in servers:
                await nodes["ticks"].write_value(ua.Variant(n, ua.VariantType.UInt32))
                await nodes["stamped"].write_value(
                    ua.DataValue(
                        Value=ua.Variant(float(n), ua.VariantType.Double),
                        SourceTimestamp=STAMPED_SOURCE_TIME,
                    )
                )
                # Structured values that change, for the push tests of the `opc3` device.
                await nodes["drive"].write_value(
                    ua.Variant(ua.SimMotor(Current=float(n), Temp=40.0), ua.VariantType.ExtensionObject)
                )
                await nodes["levels"].write_value(ua.Variant([0.0, float(n), 0.0], ua.VariantType.Double))
                if "burst" in nodes:
                    for k in range(5):
                        await nodes["burst"].write_value(ua.Variant(10 * n + k, ua.VariantType.UInt32))
                if DYNAMIC:
                    drift = 2.5 * math.sin(2 * math.pi * n / 300)
                    await nodes["temperature"].write_value(round(TEMPERATURE + drift, 2))
                    await nodes["count"].write_value(
                        ua.Variant((COUNT + n) & 0xFFFFFFFF, ua.VariantType.UInt32)
                    )
    finally:
        for server, _nodes in servers:
            await server.stop()


if __name__ == "__main__":
    asyncio.run(main())
