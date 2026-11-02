#!/bin/sh
# Wait for the simulator and broker to be reachable, then start the connector.
# The connector connects to Modbus devices once at startup, so the simulator must
# be accepting connections before it launches.
#
# With MULTI_DEVICE_COUNT=N (docker-compose.multi-device.yaml) the process instead runs N
# connector instances, one config file per simulated device, rendered from
# /usr/share/tedge-dot/multi-device/device.toml.template: instance N is service tedge-dot-N and
# owns device plc-N on simulator port 501+N.
set -e

echo "waiting for simulator:502 ..."
until nc -z simulator 502; do sleep 1; done

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

if [ -n "${MULTI_DEVICE_COUNT:-}" ]; then
    dir=/etc/tedge-dot/multi-device
    mkdir -p "$dir"
    n=1
    while [ "$n" -le "$MULTI_DEVICE_COUNT" ]; do
        # Only when missing: management commands rewrite these files, and a container restart
        # must not undo what they persisted.
        [ -f "$dir/plc-$n.toml" ] ||
            sed -e "s/@N@/$n/g" -e "s/@PORT@/$((501 + n))/g" \
                /usr/share/tedge-dot/multi-device/device.toml.template >"$dir/plc-$n.toml"
        n=$((n + 1))
    done
    echo "starting tedge-dot with $MULTI_DEVICE_COUNT connector configs from $dir"
    exec /usr/bin/tedge-dot run -c "$dir"
fi

# The measurement-output device (connector-measure.toml) is a connector instance of its own, in
# a second process: the suite edits /etc/connector.toml in place and signals PID 1, so the main
# instance keeps running from that one file.
if [ -f /etc/connector-measure.toml ]; then
    echo "starting tedge-dot (measurement output)"
    /usr/bin/tedge-dot /etc/connector-measure.toml &
fi

echo "starting tedge-dot"
exec /usr/bin/tedge-dot /etc/connector.toml
