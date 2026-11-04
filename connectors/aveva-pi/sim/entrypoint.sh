#!/bin/sh
# Generate the test PKI into $SIM_CERT_DIR (a volume the connector mounts read-only) unless
# it is already there, then start the simulator. Test fixtures only.
#
#   ca.pem / ca.key              the test CA (the connector's tls.ca_file)
#   server.pem / server.key      server certificate with SANs $SIM_HOSTNAMES (comma-separated)
#   server-nosan.pem / .key      the same without any SAN: what rustls and mbedTLS refuse
#   client.pem / client.key      client certificate for auth.method = "certificate"
#   other-ca.pem                 an unrelated CA, for the "untrusted server" case
set -eu
D="${SIM_CERT_DIR:-/certs}"
HOSTS="${SIM_HOSTNAMES:-simulator,localhost}"
mkdir -p "$D"
if [ "${SIM_TLS:-1}" = "1" ] && [ ! -f "$D/server.pem" ]; then
  cd "$D"
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=tedge-dot PI sim CA" \
    -keyout ca.key -out ca.pem 2>/dev/null
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=unrelated CA" \
    -keyout other-ca.key -out other-ca.pem 2>/dev/null
  san=$(echo "$HOSTS" | tr ',' '\n' | sed 's/^/DNS:/' | paste -sd, -)
  first=$(echo "$HOSTS" | cut -d, -f1)
  sign() { # sign <name> <subject> [extfile]
    openssl req -newkey rsa:2048 -nodes -subj "$2" -keyout "$1.key" -out "$1.csr" 2>/dev/null
    openssl x509 -req -in "$1.csr" -CA ca.pem -CAkey ca.key -CAcreateserial -days 3650 \
      ${3:+-extfile "$3"} -out "$1.pem" 2>/dev/null
    rm -f "$1.csr"
  }
  printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\n' "$san" > server.ext
  sign server "/CN=$first" server.ext
  printf 'extendedKeyUsage=serverAuth\n' > nosan.ext
  sign server-nosan "/CN=$first" nosan.ext
  printf 'extendedKeyUsage=clientAuth\n' > client.ext
  sign client "/CN=tedge-dot" client.ext
  rm -f ./*.ext ./*.srl
  chmod 644 ./*.pem ./*.key
fi
exec python /app/server.py
