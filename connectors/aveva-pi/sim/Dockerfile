# AVEVA PI Web API simulator (our own code, aiohttp) for the e2e harness. Build context is the
# repository root. See server.py for the API, the environment and the /_sim control API.
# CI overrides BASE_REGISTRY with the GHCR mirror of .github/mirror-images.txt.
ARG BASE_REGISTRY=public.ecr.aws/docker/library
FROM ${BASE_REGISTRY}/python:3.12-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends openssl curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY connectors/aveva-pi/sim/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY connectors/aveva-pi/sim/server.py connectors/aveva-pi/sim/seed.json connectors/aveva-pi/sim/entrypoint.sh ./
RUN chmod +x entrypoint.sh
ENV SIM_CERT_DIR=/certs SIM_TLS=1
EXPOSE 8443 8080
# The control API answers whatever the auth and fault settings are. The client certificate is
# presented for SIM_AUTH=certificate (a server that does not ask for one ignores it).
HEALTHCHECK --interval=2s --timeout=2s --retries=15 \
    CMD curl -skf --cert /certs/client.pem --key /certs/client.key "http$( [ "$SIM_TLS" = 1 ] && echo s)://localhost:${SIM_PORT:-$([ "$SIM_TLS" = 1 ] && echo 8443 || echo 8080)}/_sim/counters" >/dev/null || exit 1
ENTRYPOINT ["/app/entrypoint.sh"]
