#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Launch with WINIOS_CMD=1 first (scripts/duo_perf.sh launch '{"WINIOS_CMD":"1"}').
# Usage: scripts/duo_cmd.sh 'verb args' ['verb args' ...]
#        scripts/duo_cmd.sh --get NAME
set -euo pipefail

BUNDLE="com.RRGSyndicate.WindowsiOS"
UDID="${WINIOS_DUO_UDID:-$(cat "$HOME/.config/windowsios/duo-udid" 2>/dev/null || true)}"
[ -n "$UDID" ] || { echo "duo_cmd: set WINIOS_DUO_UDID or write the UDID to ~/.config/windowsios/duo-udid" >&2; exit 1; }

copy_from() { # SRC DEST: a file in the app's container; quiet when it does not exist
    xcrun devicectl device copy from --device "$UDID" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
        --source "$1" --destination "$2" >/dev/null 2>&1 || rm -f "$2"
}

[ "$#" -gt 0 ] || { echo "Usage: scripts/duo_cmd.sh 'verb args' ['verb args' ...] | --get NAME" >&2; exit 1; }
work="$(mktemp -d /tmp/winios-cmd.XXXXXX)"
trap 'rm -rf "$work"' EXIT
if [ "$1" = --get ]; then
    [ "$#" -eq 2 ] || { echo "duo_cmd: --get needs one basename" >&2; exit 1; }
    case "$2" in ''|*/*|.|..) echo "duo_cmd: --get needs a basename" >&2; exit 1;; esac
    copy_from "Documents/$2" "$work/download"
    [ -f "$work/download" ] || { echo "duo_cmd: could not copy Documents/$2" >&2; exit 1; }
    cp "$work/download" "./$2"
    exit 0
fi

timeout="${DUO_CMD_TIMEOUT:-120}"
case "$timeout" in ''|*[!0-9]*) echo "duo_cmd: DUO_CMD_TIMEOUT must be a positive integer" >&2; exit 1;; esac
[ "$timeout" -gt 0 ] || { echo "duo_cmd: DUO_CMD_TIMEOUT must be positive" >&2; exit 1; }
deadline=$(($(date +%s) + timeout))
copy_from Documents/winvm_cmds.done "$work/done"
previous="$(cat "$work/done" 2>/dev/null || true)"
case "$previous" in ''|*[!0-9]*) previous=0;; esac
seq="$(date +%s)"
# Two consecutive invocations in one second must still have increasing sequences.
while [ "$seq" -le "$previous" ]; do
    [ "$seq" -lt "$deadline" ] || { echo "duo_cmd: timed out waiting for a new sequence" >&2; exit 1; }
    sleep 1
    seq="$(date +%s)"
done
copy_from Documents/winvm_cmds.log "$work/before"
bytes=0
[ ! -f "$work/before" ] || bytes="$(wc -c < "$work/before")"
{ printf '#seq %s\n' "$seq"; printf '%s\n' "$@"; } > "$work/commands"
if ! xcrun devicectl device copy to --device "$UDID" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
    --source "$work/commands" --destination Documents/winvm_cmds.txt >/dev/null 2>&1; then
    echo "duo_cmd: could not copy commands to the app" >&2
    exit 1
fi
while [ "$(date +%s)" -lt "$deadline" ]; do
    copy_from Documents/winvm_cmds.done "$work/done"
    if [ "$(cat "$work/done" 2>/dev/null || true)" = "$seq" ]; then
        copy_from Documents/winvm_cmds.log "$work/after"
        [ -f "$work/after" ] || { echo "duo_cmd: commands finished but log could not be copied" >&2; exit 1; }
        [ "$(wc -c < "$work/after")" -ge "$bytes" ] || bytes=0
        tail -c "+$((bytes + 1))" "$work/after"
        exit 0
    fi
    sleep 1
done
echo "duo_cmd: timed out after ${timeout}s waiting for commands (launch with WINIOS_CMD=1)" >&2
exit 1
