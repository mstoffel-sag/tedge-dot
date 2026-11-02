#!/bin/sh
# Wait for the OPC-UA simulators and broker to be reachable, then start the connector.
# The connector opens its OPC-UA session at startup, so the simulator must be accepting
# connections before it launches.
set -e

echo "waiting for simulator:4840 ..."
until nc -z simulator 4840; do sleep 1; done
echo "waiting for sampling-simulator:4840 ..."
until nc -z sampling-simulator 4840; do sleep 1; done

echo "waiting for load-simulator:4840 ..."
until nc -z load-simulator 4840; do sleep 1; done

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

echo "starting tedge-dot (opcua)"
# Every config of the stack runs as its own connector instance: connector.toml, plus
# connector-structures.toml (the structured-value device), connector-load.toml (queue size and a
# 5000-point device) and connector-measure.toml (measurement output). Both builds take a directory, and the
# C build only that form for more than one file.
mkdir -p /etc/ot
cp /etc/connector.toml /etc/ot/
for f in /etc/connector-*.toml; do
    if [ -f "$f" ]; then cp "$f" /etc/ot/; fi
done
# connector-load.toml's opc6 subscribes the load-simulator's 5000 Bulk nodes: generated here
# rather than committed as 5000 point tables.
if [ -f /etc/ot/connector-load.toml ]; then
    i=0
    while [ "$i" -lt 5000 ]; do
        printf '\n  [[device.point]]\n  id = "bulk%d"\n  datatype = "float64"\n  address = { node_id = "ns=2;s=Bulk%d" }\n' "$i" "$i"
        i=$((i + 1))
    done >> /etc/ot/connector-load.toml
    # opc7 polls the first 1200 of them: three Read blocks (500, 500, 200). Every 5 s, not
    # hourly like the file's default, so a test that subscribes after start-up sees a pass.
    printf '\n[[device]]\nname = "opc7"\nprotocol_address = { endpoint = "opc.tcp://load-simulator:4840/" }\ndefault_mode = "typed"\npoll_interval = "5s"\n' \
        >> /etc/ot/connector-load.toml
    i=0
    while [ "$i" -lt 1200 ]; do
        printf '\n  [[device.point]]\n  id = "bulk%d"\n  datatype = "float64"\n  subscribe = false\n  address = { node_id = "ns=2;s=Bulk%d" }\n' "$i" "$i"
        i=$((i + 1))
    done >> /etc/ot/connector-load.toml
fi
exec /usr/bin/tedge-dot /etc/ot
