#!/bin/sh
# Portable post-install script for deb/rpm/apk.
set -e

systemctl_available() {
    [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1
}

tedge refresh-bridges || true

# The measurement-series alarm and event moved out of ot-alarm and ot-event into flows of their
# own, ot-alarm-series and ot-event-series, which subscribe to every measurement and so ship
# inactive in /usr/share/tedge-dot/flows (contract §5.5, doc/measurement-output.md). A site whose
# ot-alarm or ot-event params file names a series gets the new flow installed with a copy of that
# params file, so its alarm or event keeps working. Once installed, an upgrade refreshes the
# flow's logic and leaves its params.toml alone, as for the other flows.
FLOWS_DIR=${TEDGE_DOT_FLOWS_DIR:-/etc/tedge/mappers/c8y/flows}
SHARE_DIR=${TEDGE_DOT_SHARE_FLOWS_DIR:-/usr/share/tedge-dot/flows}
install_series_flow() {
    source_params="$FLOWS_DIR/$1/params.toml"
    target="$FLOWS_DIR/$1-series"
    [ -d "$SHARE_DIR/$1-series" ] || return 0
    if [ ! -d "$target" ]; then
        [ -f "$source_params" ] || return 0
        grep -Eq '^[[:space:]]*series[[:space:]]*=[[:space:]]*"[^"]+"' "$source_params" || return 0
        mkdir -p "$target"
        cp "$source_params" "$target/params.toml"
        echo "tedge-dot: the measurement series of $1 is now the $1-series flow; installed it with the series from $source_params"
    fi
    for f in "$SHARE_DIR/$1-series/"*; do
        [ "$(basename "$f")" = params.toml ] || cp "$f" "$target/"
    done
    chown -R tedge:tedge "$target" 2>/dev/null || true
}
install_series_flow ot-alarm || true
install_series_flow ot-event || true

if systemctl_available; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable tedge-dot.service >/dev/null 2>&1 || true
    systemctl restart tedge-dot.service >/dev/null 2>&1 || true
fi
