#!/bin/sh
# Wait for the OPC-UA simulators and broker to be reachable, then start the connector.
# The connector opens its OPC-UA session at startup, so the simulator must be accepting
# connections before it launches.
set -e

echo "waiting for simulator:4840 ..."
until nc -z simulator 4840; do sleep 1; done
echo "waiting for sampling-simulator:4840 ..."
until nc -z sampling-simulator 4840; do sleep 1; done

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

echo "starting tedge-dot (opcua)"
# Every config of the stack runs as its own connector instance: connector.toml, plus
# connector-structures.toml (the structured-value device). Both builds take a directory, and the
# C build only that form for more than one file.
mkdir -p /etc/ot
cp /etc/connector.toml /etc/ot/
for f in /etc/connector-*.toml; do
    if [ -f "$f" ]; then cp "$f" /etc/ot/; fi
done
exec /usr/bin/tedge-dot /etc/ot
