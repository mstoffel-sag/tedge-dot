#!/bin/sh
# Namespace-table e2e stack (docker-compose.namespaces.yaml): wait for both simulators and the
# broker, then run the connector on this directory's configuration.
set -e
for peer in ns-sim-a:4840 ns-sim-b:4840 broker:1883; do
    echo "waiting for $peer ..."
    until nc -z "${peer%:*}" "${peer#*:}"; do sleep 1; done
done
echo "starting tedge-dot (opcua, namespace tables)"
exec /usr/bin/tedge-dot /opt/namespaces/connector.toml
