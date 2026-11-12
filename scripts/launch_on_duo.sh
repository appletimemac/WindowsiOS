#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Device: $1, else $WINIOS_DUO_UDID, else ~/.config/windowsios/duo-udid (kept out of the repo).
UDID="${1:-${WINIOS_DUO_UDID:-$(cat "$HOME/.config/windowsios/duo-udid" 2>/dev/null || true)}}"
if [ -z "$UDID" ]; then
    echo "usage: $0 <device-udid> [bundle-id]  (or set WINIOS_DUO_UDID)" >&2
    exit 2
fi
BUNDLE_ID="${2:-com.RRGSyndicate.WindowsiOS}"
LAUNCH_JSON="$ROOT/work/duo_launch.json"

mkdir -p "$ROOT/work"
rm -f "$LAUNCH_JSON"

echo "[duo] Launching $BUNDLE_ID (--start-stopped) on $UDID..."
for attempt in 1 2 3 4 5; do
    if xcrun devicectl device process launch \
        --start-stopped \
        --terminate-existing \
        --device "$UDID" \
        --json-output "$LAUNCH_JSON" \
        "$BUNDLE_ID" || \
       xcrun devicectl device process launch \
        --no-activate \
        --start-stopped \
        --terminate-existing \
        --device "$UDID" \
        --json-output "$LAUNCH_JSON" \
        "$BUNDLE_ID"; then
        break
    fi
    echo "[duo] Attempt $attempt failed; retrying in 2s..."
    sleep 2
done

read -r PID CORE_ID < <(/opt/homebrew/bin/python3 -c "
import json
d = json.load(open('$LAUNCH_JSON'))
pid = d['result']['process']['processIdentifier']
r = d.get('result', {})
cid = r.get('deviceIdentifier') or r.get('device', {}).get('identifier') or '$UDID'
print(pid, cid)
")

echo "[duo] Attaching LLDB to PID=$PID on CoreDevice=$CORE_ID..."
sleep 1
LLDB_LOG="$ROOT/work/duo_lldb.log"
PYTHONPATH="$ROOT/scripts" xcrun lldb -b \
    -o "script lldb.debugger.SetAsync(False)" \
    -o "device list" \
    -o "device select $CORE_ID" \
    -o "device process attach -p $PID" \
    -o "command script import \"$ROOT/scripts/_lldb_txm_jit.py\"" \
    -o "txm_jit_run" 2>&1 | tee "$LLDB_LOG" || true

# `lldb -b` exits 0 even when txm_jit_run raises, so success is the helper's own line.
grep -q "\[txm-jit\] SUCCESS" "$LLDB_LOG" || { echo "[duo] TXM JIT setup failed; see $LLDB_LOG" >&2; exit 1; }
