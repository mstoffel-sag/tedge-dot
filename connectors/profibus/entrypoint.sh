#!/bin/sh
# Wait for the MQTT broker, then start the connector. The simulator's TCP
# endpoint needs no probing here: the connector's tcp:// transport retries the
# initial connect itself (and probing would steal the sim's single-connection
# socat listener).
set -e

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

# The measurement-output device (connector-measure.toml) is a connector instance of its own, in
# a second process, so the main instance keeps running from its one file as PID 1.
if [ -f /etc/connector-measure.toml ]; then
    echo "starting tedge-dot (profibus, measurement output)"
    /usr/bin/tedge-dot /etc/connector-measure.toml &
fi

echo "starting tedge-dot (profibus)"
exec /usr/bin/tedge-dot /etc/connector.toml
