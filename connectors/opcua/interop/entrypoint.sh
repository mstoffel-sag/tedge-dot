#!/bin/sh
# Entrypoint of the connector in the OPC UA INTEROP stack (docker-compose.yaml).
#
# Two jobs before the connector starts:
#
#  1. Create the application certificate up front with `tedge-dot pki create` and export it
#     into the volume ref-strict mounts at /app/certs. That server copies certs/trusted/* into
#     its trust list AT STARTUP and does not auto-accept anything, so our certificate has to
#     exist before it is (re)started -- the suite restarts it to make the trust take effect.
#  2. Run the stack's configuration, whose namespace tables let every server number its
#     namespaces as it likes.
#
# The PKI directory is the packaged default, /var/lib/tedge-dot/opcua/pki.
set -e
config=/opt/interop/connector.toml

echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done

# `create` is idempotent for our purposes: it fails if a certificate already exists, which on a
# fresh volume it does not.
/usr/bin/tedge-dot pki create --config "$config" >/dev/null 2>&1 || true

mkdir -p /refserver-trust/trusted
/usr/bin/tedge-dot pki export --config "$config" --output /refserver-trust/trusted/tedge-dot.der
echo "exported the application certificate for ref-strict:"
/usr/bin/tedge-dot pki show --config "$config" || true

echo "starting tedge-dot (opcua, interop)"
exec /usr/bin/tedge-dot "$config"
