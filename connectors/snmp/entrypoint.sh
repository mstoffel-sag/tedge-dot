#!/bin/sh
# Wait for the broker, then start the connector. The simulators only send when the suite tells
# them to, so there is nothing else to wait for: the connector resolves their names itself.
set -e

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

# The measurement-output device (connector-measure.toml) is a connector instance of its own, in
# a second process, so the main instance keeps running from its one file as PID 1.
if [ -f /etc/connector-measure.toml ]; then
    echo "starting tedge-dot (measurement output)"
    /usr/bin/tedge-dot /etc/connector-measure.toml &
fi

echo "starting tedge-dot"
exec /usr/bin/tedge-dot /etc/connector.toml
