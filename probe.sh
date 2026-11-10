#!/bin/sh
# Probe every AppleHPM (USB-C) port on this host and report which one sees the target.
# Usage: ./probe.sh
set -u
HERE="$(dirname "$0")"
BIN="$HERE/macvdmtool"

[ -x "$BIN" ] || (cd "$HERE" && make)

echo "== Host model =="
ioreg -c IOPlatformExpertDevice -d 2 | grep -m1 '"model"'
echo

echo "== AppleHPM ports (RID -> USB-C port) =="
ioreg -c AppleHPMARMSPMI -r | grep -E 'class AppleHPMARMSPMI|"RID"|Port-USB-C@'
echo

RIDS="$(ioreg -c AppleHPMARMSPMI -r | sed -n 's/.*"RID" = \([0-9]*\).*/\1/p' | sort -n)"
echo "== Trying each RID =="
for rid in $RIDS; do
    printf 'RID %s: ' "$rid"
    sudo "$BIN" -v -p "$rid" status 2>&1 | sed -n 's/^Connection: /Connection: /p;s/^Status: /Status: /p'
done

echo
echo "Pick the RID whose line says 'Connection: Source' or 'Sink' (not 'None')."
