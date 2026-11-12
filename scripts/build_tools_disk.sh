#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds work/tools.img: a small raw disk (MBR, one FAT32 volume labeled WINIOSTOOLS) that Windows mounts
# with the guest pieces on it: WinIOSAgent.exe, WinIOSBrowser.exe and "Install WindowsiOS Tools.cmd"
# (one UAC prompt installs all).
#
#   scripts/build_tools_disk.sh [OUT]        OUT defaults to work/tools.img
#   TOOLS_IMG_MB=... image size in MiB (default 64; FAT32 needs about 33 MiB at the very least)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="${1:-work/tools.img}"
SIZE_MB="${TOOLS_IMG_MB:-64}"
LABEL="WINIOSTOOLS"
CMD_SRC="scripts/install_windowsios_tools.cmd"

AGENT="build/winios_agent_arm64.exe"
BROWSER="build/winios_browser_arm64.exe"

if [ ! -f "$AGENT" ] || [ ! -f "$BROWSER" ]; then
    make -s guest-tools
fi
for f in "$AGENT" "$BROWSER" "$CMD_SRC"; do
    [ -f "$f" ] || { echo "build_tools_disk: missing $f" >&2; exit 1; }
done

IMG_DEV=""

cleanup() {
    [ -n "$IMG_DEV" ] && hdiutil detach -quiet -force "$IMG_DEV" 2>/dev/null || true
}
trap cleanup EXIT

# hdiutil attach is deprecated in favor of diskutil image attach but still works; drop only that notice.
attach() {
    hdiutil attach "$@" 2> >(grep -v "is deprecated" >&2)
}

need=0
for f in "$AGENT" "$BROWSER" "$CMD_SRC"; do need=$((need + $(stat -f %z "$f"))); done
if [ $((need * 2)) -gt $((SIZE_MB * 1024 * 1024 / 2)) ]; then
    echo "build_tools_disk: $need bytes of files do not leave headroom in $SIZE_MB MiB; raise TOOLS_IMG_MB" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "${SIZE_MB}m" "$OUT"

echo "build_tools_disk: partitioning and formatting $OUT ($SIZE_MB MiB, MBR, FAT32, $LABEL)"
IMG_DEV="$(attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "$OUT" | awk 'NR==1 {print $1}')"
diskutil partitionDisk "$IMG_DEV" MBR "MS-DOS FAT32" "$LABEL" 100% >/dev/null
MNT="$(diskutil info -plist "${IMG_DEV}s1" | plutil -extract MountPoint raw -o - -)"
[ -d "$MNT" ] || { echo "build_tools_disk: the new volume did not mount" >&2; exit 1; }

echo "build_tools_disk: copying files to $MNT"
cp -X "$AGENT" "$MNT/WinIOSAgent.exe"
cp -X "$BROWSER" "$MNT/WinIOSBrowser.exe"
# cmd.exe wants CRLF, whatever the source has
sed 's/\r$//; s/$/\r/' "$CMD_SRC" > "$MNT/Install WindowsiOS Tools.cmd"

# What macOS leaves on a FAT volume: AppleDouble files, Spotlight and FSEvents state
find "$MNT" -name '._*' -delete
rm -rf "$MNT/.fseventsd" "$MNT/.Spotlight-V100" "$MNT/.Trashes" "$MNT/.DS_Store"
sync
diskutil unmount "$MNT" >/dev/null
hdiutil detach -quiet "$IMG_DEV"
IMG_DEV=""

# An all-zero MBR disk signature makes Windows treat the disk as unidentified.
if [ "$(dd if="$OUT" bs=1 skip=440 count=4 2>/dev/null | xxd -p)" = "00000000" ]; then
    head -c 4 /dev/urandom | dd of="$OUT" bs=1 seek=440 conv=notrunc 2>/dev/null
fi

echo "build_tools_disk: checking $OUT read-only"
IMG_DEV="$(attach -imagekey diskimage-class=CRawDiskImage -readonly -nobrowse "$OUT" | awk 'NR==1 {print $1}')"
CHECK="$(diskutil info -plist "${IMG_DEV}s1" | plutil -extract MountPoint raw -o - -)"
(cd "$CHECK" && find . -type f | sort)
if find "$CHECK" -name '._*' -o -name '.fseventsd' | grep -q .; then
    echo "build_tools_disk: macOS metadata is on the disk" >&2
    exit 1
fi
hdiutil detach -quiet "$IMG_DEV"
IMG_DEV=""
echo "build_tools_disk: wrote $OUT"
