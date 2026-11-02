#!/bin/sh
set -e

# Load the vcan kernel module (idempotent on failure — may already be loaded).
modprobe vcan || true

# Create vcan0 if it does not already exist (the simulator may have done this).
ip link add dev vcan0 type vcan 2>/dev/null || true
ip link set up vcan0

echo "vcan0 is up"

# Wait for the MQTT broker to be reachable.
echo "Waiting for broker at localhost:13884 ..."
until nc -z localhost 13884; do
    sleep 0.5
done
echo "Broker is ready"

# Start the connector.
# The measurement-output device (connector-measure.toml) is a connector instance of its own, in
# a second process, so the main instance keeps running from its one file as PID 1.
if [ -f /etc/connector-measure.toml ]; then
    echo "starting tedge-dot (measurement output)"
    tedge-dot /etc/connector-measure.toml &
fi

exec tedge-dot run /etc/connector.toml
