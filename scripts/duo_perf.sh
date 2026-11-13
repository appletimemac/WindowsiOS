#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Measuring WindowsiOS on a paired iPhone without Xcode.
#
#   scripts/duo_perf.sh launch ['{"HV_QOS":"ui","HV_ENGINE_STATS":"1"}']   (re)launch the app with environment
#                                    variables; the JIT then comes from StikDebug as with a Home Screen launch
#   scripts/duo_perf.sh env '{"WINIOS_CMD":"1"}'   the same variables for every later launch, however the app is
#                                    started (after an install StikDebug restarts it without the launch's own):
#                                    written to Documents/launch_env.json. '{}' turns them off again
#   scripts/duo_perf.sh pull [DIR]   copy perf.csv, perf.csv.1, boot_status.txt and Shared/diag-*.txt to DIR
#                                    (default work/duo_perf/<date-time>) and print the report
#   scripts/duo_perf.sh watch MINUTES [DIR]   pull every minute for MINUTES, then report
#
# The device comes from $WINIOS_DUO_UDID or ~/.config/windowsios/duo-udid (kept out of the repository).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="com.RRGSyndicate.WindowsiOS"
UDID="${WINIOS_DUO_UDID:-$(cat "$HOME/.config/windowsios/duo-udid" 2>/dev/null || true)}"
[ -n "$UDID" ] || { echo "duo_perf: set WINIOS_DUO_UDID or write the UDID to ~/.config/windowsios/duo-udid" >&2; exit 1; }

copy_from() { # SRC DEST: a file in the app's container; quiet when it does not exist
    # A copy of a file the app is appending to (perf.csv every 2 s) can fail; a retry lands between writes.
    local try
    for try in 1 2 3; do
        xcrun devicectl device copy from --device "$UDID" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
            --source "$1" --destination "$2" >/dev/null 2>&1 && return 0
        sleep 1
    done
    rm -f "$2"
}

pull() {
    local dir="$1"
    mkdir -p "$dir"
    copy_from Documents/perf.csv.1 "$dir/perf.csv.1"
    copy_from Documents/perf.csv "$dir/perf.csv"
    copy_from Documents/boot_status.txt "$dir/boot_status.txt"
    local listing
    listing="$(xcrun devicectl device info files --device "$UDID" --domain-type appDataContainer \
        --domain-identifier "$BUNDLE" --subdirectory Documents/Shared 2>/dev/null || true)"
    for f in $(printf '%s\n' "$listing" | grep -oE 'diag-[0-9-]+\.txt' | sort -u); do
        [ -f "$dir/$f" ] || copy_from "Documents/Shared/$f" "$dir/$f"
    done
}

report() {
    local dir="$1" files=()
    [ -f "$dir/perf.csv.1" ] && files+=("$dir/perf.csv.1")
    [ -f "$dir/perf.csv" ] && files+=("$dir/perf.csv")
    [ ${#files[@]} -gt 0 ] || { echo "duo_perf: no perf.csv pulled into $dir" >&2; return 1; }
    python3 -I "$ROOT/scripts/perf_report.py" --skip 300 --minutes "${files[@]}" | tee "$dir/report.txt"
}

cmd="${1:-}"
case "$cmd" in
launch)
    env_json="${2:-}"
    args=(--terminate-existing --device "$UDID")
    [ -n "$env_json" ] && args+=(--environment-variables "$env_json")
    xcrun devicectl device process launch "${args[@]}" "$BUNDLE" 2>&1 | tail -1
    ;;
env)
    tmp="$(mktemp -d)"
    printf '%s\n' "${2:?a JSON object of variables, or '{}'}" > "$tmp/launch_env.json"
    python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); assert isinstance(d, dict) and all(isinstance(v, str) for v in d.values())' \
        "$tmp/launch_env.json" || { echo "duo_perf: not a JSON object of strings" >&2; exit 1; }
    xcrun devicectl device copy to --device "$UDID" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
        --source "$tmp/launch_env.json" --destination Documents/launch_env.json 2>&1 | tail -1
    rm -f "$tmp/launch_env.json"; rmdir "$tmp"
    ;;
pull)
    dir="${2:-$ROOT/work/duo_perf/$(date +%Y%m%d-%H%M%S)}"
    pull "$dir"
    report "$dir"
    ;;
watch)
    minutes="${2:?minutes}"
    dir="${3:-$ROOT/work/duo_perf/$(date +%Y%m%d-%H%M%S)}"
    for ((i = 0; i < minutes; i++)); do
        pull "$dir"
        echo "duo_perf: $(date +%H:%M:%S) pulled ($((i + 1))/$minutes)"
        sleep 60
    done
    pull "$dir"
    report "$dir"
    ;;
*)
    sed -n '3,12p' "$0"
    exit 1
    ;;
esac
