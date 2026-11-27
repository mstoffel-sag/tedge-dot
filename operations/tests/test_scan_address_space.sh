#!/usr/bin/env bash
# Offline tests for operations/ot-scan-address-space: connector-file lookup, opcua-scan
# arguments, metadata, and every failure of the device-side run, with a fake opcua-scan and a
# fake curl on PATH. No broker, no server, no cloud. Requires bash, jq and gzip.
#
# Run directly:  ./operations/tests/test_scan_address_space.sh
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
repo="$(pwd)"
SHIM="$repo/operations/ot-scan-address-space"
VECTORS="$repo/doc/contract/test-vectors/namespace-file"

pass=0
fail=0
ok() { echo "ok   - $1"; pass=$((pass + 1)); }
bad() { echo "FAIL - $1"; shift; for l in "$@"; do echo "       $l"; done; fail=$((fail + 1)); }

# expect <name> <expected> <actual>
expect() {
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected: $2" "actual:   $3"; fi
}
# expect_contains <name> <needle> <haystack>
expect_contains() {
    case "$3" in *"$2"*) ok "$1" ;; *) bad "$1" "expected to contain: $2" "actual: $3" ;; esac
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
conf="$tmp/ot"
mkdir -p "$conf"

cat >"$conf/opcua.toml" <<'TOML'
# OPC UA line 1
[connector]
protocol = "opcua"   # the protocol

[connection]
pki_dir = "/srv/pki"

[[device]]
name = "plc-7"
protocol_address = { endpoint = "opc.tcp://plc-7:4840" }

[[device.point]]
id = "t"
name = "plc-9"
address = { node_id = "ns=2;s=T" }
TOML
cat >"$conf/opcua-2.toml" <<'TOML'
[connector]
protocol = 'opcua'

[[device]]
name = 'plc-8'
TOML
cat >"$conf/modbus.toml" <<'TOML'
[connector]
protocol = "modbus"

[[device]]
name = "plc-6"
TOML
cat >"$conf/twice.toml" <<'TOML'
[connector]
protocol = "opcua"

[[device]]
name = "plc-dup"
TOML
cp "$conf/twice.toml" "$conf/twice-2.toml"

# --- connector-file lookup ---------------------------------------------------------------
expect "find-device: the file that defines the device" "$conf/opcua.toml" \
    "$("$SHIM" --find-device plc-7 "$conf" 2>&1)"
expect "find-device: single-quoted strings" "$conf/opcua-2.toml" \
    "$("$SHIM" --find-device plc-8 "$conf" 2>&1)"
out="$("$SHIM" --find-device plc-6 "$conf" 2>&1)"; code=$?
expect "find-device: a Modbus device is not an OPC UA device (exit)" 1 "$code"
expect_contains "find-device: a Modbus device is not an OPC UA device (reason)" 'defines the device "plc-6"' "$out"
out="$("$SHIM" --find-device plc-9 "$conf" 2>&1)"; code=$?
expect "find-device: a point's name is not a device" 1 "$code"
out="$("$SHIM" --find-device plc-dup "$conf" 2>&1)"; code=$?
expect "find-device: two files define the device (exit)" 1 "$code"
expect_contains "find-device: two files define the device (reason)" "several connector files" "$out"

# --- opcua-scan arguments ----------------------------------------------------------------
args() { "$SHIM" --scan-args "$1" "$conf/opcua.toml" /out.json 2>&1 | paste -sd' ' -; }
expect "scan-args: defaults" \
    "browse --tedge-dot-config $conf/opcua.toml --device plc-7 --output tedge-dot-namespace --file /out.json --pki-dir /srv/pki --depth unlimited --type Variable,Method" \
    "$(args '{"device":"plc-7"}')"
expect "scan-args: scope" \
    "browse --tedge-dot-config $conf/opcua.toml --device plc-7 --output tedge-dot-namespace --file /out.json --pki-dir /srv/pki --root ns=2;s=Line1 --depth 3 --include Objects/Factory/** --include Objects/Line 2/** --exclude **/Diagnostics/** --type Variable --max-children 5000" \
    "$(args '{"device":"plc-7","root":"ns=2;s=Line1","depth":3,"include":["Objects/Factory/**","Objects/Line 2/**"],"exclude":"**/Diagnostics/**","type":["Variable"],"max_children":5000}')"
expect "scan-args: connection fields are ignored" \
    "$(args '{"device":"plc-7"}')" \
    "$(args '{"device":"plc-7","endpoint":"opc.tcp://evil:4840","password":"x"}')"
expect "scan-args: default PKI directory" "/var/lib/tedge-dot/opcua/pki" \
    "$("$SHIM" --scan-args '{"device":"plc-8"}' "$conf/opcua-2.toml" /o | sed -n '/--pki-dir/{n;p;}')"
out="$("$SHIM" --scan-args '{}' "$conf/opcua.toml" /o 2>&1)"; code=$?
expect "scan-args: no device (exit)" 1 "$code"
expect_contains "scan-args: no device (reason)" '"device"' "$out"

# --- metadata ----------------------------------------------------------------------------
meta="$("$SHIM" --metadata "$VECTORS/valid/minimal.json" gw-1 plc-7 opcua)"
expect "metadata: fields" \
    '{"formatVersion":1,"protocol":"opcua","gateway":"gw-1","device":"plc-7","connectorFile":"opcua","endpoint":"opc.tcp://plc-7:4840","nodes":3,"points":1,"truncated":false}' \
    "$(jq -c '{formatVersion,protocol,gateway,device,connectorFile,endpoint,nodes,points,truncated}' <<<"$meta")"
expect "metadata: namespaces" 3 "$(jq '.namespaces | length' <<<"$meta")"
expect "metadata: truncated scan" true \
    "$("$SHIM" --metadata "$VECTORS/valid/truncated.json" gw-1 plc-7 opcua | jq '.truncated')"

# --- device-side run with fakes ----------------------------------------------------------
bin="$tmp/bin"
mkdir -p "$bin"
log="$tmp/requests.log"
cat >"$bin/opcua-scan" <<EOF2
#!/usr/bin/env bash
# Fake opcua-scan. FAKE_SCAN: ok (default), old, fail, empty, big, slow
mode="\${FAKE_SCAN:-ok}"
if [ "\$1" = "--version" ]; then echo "opcua-scan 0.0.1"; exit 0; fi
if [ "\$1" = browse ] && [ "\${2:-}" = --help ]; then
    [ "\$mode" = old ] && echo "--output <json|ndjson|csv|tedge-dot>" || echo "--output <json|ndjson|csv|tedge-dot|tedge-dot-namespace>"
    exit 0
fi
echo "\$*" >"$tmp/scan-args"
out=""; while [ \$# -gt 0 ]; do [ "\$1" = --file ] && out="\$2"; shift; done
case "\$mode" in
    fail) echo "connecting to opc.tcp://plc-7:4840"; echo "Error: BadTimeout: no answer from opc.tcp://plc-7:4840" >&2; exit 2 ;;
    empty) jq '.nodes = []' "$VECTORS/valid/minimal.json" >"\$out" ;;
    big) head -c 4000 /dev/urandom | od -x >"\$out" ;;
    slow) sleep 5 ;;
    *) cp "$VECTORS/valid/minimal.json" "\$out" ;;
esac
EOF2
cat >"$bin/curl" <<EOF2
#!/usr/bin/env bash
# Fake curl: logs "<METHOD> <url> <data>", answers by URL. FAKE_UPLOAD=fail fails the upload.
method=GET; data=""; url=""; write=""; outfile=""
while [ \$# -gt 0 ]; do
    case "\$1" in
        -X) method="\$2"; shift ;;
        --data-binary) data="\$2"; shift ;;
        --form-string|-F) data="\$data \$2"; shift ;;
        -H) shift ;;
        -w) write="\$2"; shift ;;
        -o) outfile="\$2"; shift ;;
        -s) ;;
        *) url="\$1" ;;
    esac
    shift
done
echo "\$method \$url \$data" >>"$log"
code=200; body='{}'
case "\$url" in
    */inventory/binaries) if [ "\${FAKE_UPLOAD:-}" = fail ]; then code=413; body='{"error":"too large"}'; else code=201; body='{"id":"4711"}'; fi ;;
    */identity/externalIds/c8y_Serial/gw-1:device:plc-7) body='{"managedObject":{"id":"77"}}' ;;
    */identity/externalIds/c8y_Serial/gw-1) body='{"managedObject":{"id":"1"}}' ;;
    */identity/externalIds/c8y_Serial/*) code=404 ;;
    */inventory/managedObjects\?*query=*)
        # Namespace files the gateway query finds: the new one, an earlier one of the same device,
        # one of another device, and (if the query were too wide) one of another gateway.
        body='{"managedObjects":[
          {"id":"4711","tedgeDotNamespace":{"gateway":"gw-1","device":"plc-7"}},
          {"id":"4000","tedgeDotNamespace":{"gateway":"gw-1","device":"plc-7"}},
          {"id":"4001","tedgeDotNamespace":{"gateway":"gw-1","device":"plc-8"}},
          {"id":"4002","tedgeDotNamespace":{"gateway":"gw-2","device":"plc-7"}}]}' ;;
esac
[ "\$method" = DELETE ] && code=204
if [ -n "\$outfile" ]; then echo "\$body" >"\$outfile"; else echo "\$body"; fi
[ -n "\$write" ] && printf '%s' "\${write//%\{http_code\}/\$code}" | sed 's/\\\\n/\n/g'
exit 0
EOF2
chmod +x "$bin/opcua-scan" "$bin/curl"

run() {
    rm -f "$log"
    PATH="$bin:$PATH" OT_CONFIG_DIR="$conf" TEDGE_DOT_DEVICE_ID=gw-1 C8Y_PROXY=http://proxy/c8y \
        "$SHIM" "$1" 99 2>&1
}

out="$(run '{"device":"plc-7"}')"; code=$?
expect "run: success (exit)" 0 "$code"
expect_contains "run: success (message)" "namespace file 4711: 3 nodes" "$out"
expect_contains "run: uploads a tedge-dot-namespace binary" '"type":"tedge-dot-namespace"' "$(grep 'POST http://proxy/c8y/inventory/binaries' "$log")"
expect_contains "run: stores the metadata" '"tedgeDotNamespace"' "$(grep 'PUT http://proxy/c8y/inventory/managedObjects/4711' "$log")"
expect_contains "run: records the gateway's managed object" '"gatewayId":"1"' "$(grep 'PUT http://proxy/c8y/inventory/managedObjects/4711' "$log")"
expect_contains "run: links the binary to the device" '"id":"4711"' "$(grep 'POST http://proxy/c8y/inventory/managedObjects/77/childAdditions' "$log")"
expect_contains "run: writes the result into the operation" '"binaryId":"4711","nodes":3' "$(grep 'PUT http://proxy/c8y/devicecontrol/operations/99' "$log")"
# Cumulocity refuses an operation update without a status (HTTP 422).
expect_contains "run: the operation update carries a status" '"status":"EXECUTING"' "$(grep 'PUT http://proxy/c8y/devicecontrol/operations/99' "$log")"
expect_contains "run: queries this gateway's namespace files" 'tedgeDotNamespace.gateway%20eq%20%27gw-1%27' "$(grep 'GET http://proxy/c8y/inventory/managedObjects?' "$log")"
expect "run: deletes the device's earlier file only" "DELETE http://proxy/c8y/inventory/binaries/4000" "$(grep '^DELETE' "$log" | sed 's/ *$//')"
expect_contains "run: scans with the device's connector file" "--tedge-dot-config $conf/opcua.toml --device plc-7" "$(cat "$tmp/scan-args")"

out="$(PATH="/usr/bin:/bin" OT_CONFIG_DIR="$conf" OPCUA_SCAN=/nonexistent/opcua-scan "$SHIM" '{"device":"plc-7"}' 99 2>&1)"; code=$?
expect "run: opcua-scan missing (exit)" 1 "$code"
expect_contains "run: opcua-scan missing (reason)" "opcua-scan is not installed" "$out"
out="$(FAKE_SCAN=old run '{"device":"plc-7"}')"; code=$?
expect "run: opcua-scan too old (exit)" 1 "$code"
expect_contains "run: opcua-scan too old (reason)" "cannot write namespace files" "$out"
out="$(run '{}')"; code=$?
expect "run: no device" 1 "$code"
out="$(run '{"device":"plc-6"}')"; code=$?
expect_contains "run: device not in an OPC UA file" 'no OPC UA connector file' "$out"
out="$(FAKE_SCAN=fail run '{"device":"plc-7"}')"; code=$?
expect "run: server down (exit)" 1 "$code"
expect_contains "run: server down (reason)" "BadTimeout: no answer from opc.tcp://plc-7:4840" "$out"
expect "run: server down uploads nothing" "" "$(grep -c binaries "$log" 2>/dev/null | grep -v '^0$')"
expect "run: server down keeps the earlier file" "" "$(grep "^DELETE" "$log" 2>/dev/null)"
out="$(FAKE_SCAN=empty run '{"device":"plc-7"}')"; code=$?
expect_contains "run: empty scan" "found no node" "$out"
out="$(FAKE_SCAN=ok TEDGE_DOT_SCAN_MAX_BYTES=10 run '{"device":"plc-7"}')"; code=$?
expect "run: too large (exit)" 1 "$code"
expect_contains "run: too large (reason)" "above the limit" "$out"
out="$(FAKE_SCAN=slow TEDGE_DOT_SCAN_TIMEOUT=1 run '{"device":"plc-7"}')"; code=$?
expect_contains "run: scan timeout" "did not finish within 1s" "$out"
out="$(FAKE_UPLOAD=fail run '{"device":"plc-7"}')"; code=$?
expect "run: upload fails (exit)" 1 "$code"
expect_contains "run: upload fails (reason)" "HTTP 413" "$out"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
