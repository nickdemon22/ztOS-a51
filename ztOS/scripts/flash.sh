#!/bin/bash
# Write a boot image to BOOT or RECOVERY of the Galaxy S9 from its running TWRP.
#
#   scripts/flash.sh BOOT|RECOVERY <image>
#
# Refuses to run without the stock backups, checks the device and the
# partition mapping, verifies the copy on the phone before writing and the
# partition after. Never reboots.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PART=${1:?BOOT or RECOVERY}
IMG=${2:?image}
case "$PART" in
BOOT) NODE=/dev/block/sda10 ;;
RECOVERY) NODE=/dev/block/sda11 ;;
*) echo "only BOOT or RECOVERY" >&2; exit 2 ;;
esac
for f in BOOT RECOVERY EFS; do
	[ -s "$ROOT/stock/$f.img" ] || { echo "stock/$f.img missing: back up first" >&2; exit 1; }
done
[ "$(head -c 8 "$IMG")" = "ANDROID!" ] || { echo "not an Android boot image" >&2; exit 1; }
BY=/dev/block/platform/11120000.ufs/by-name/$PART
sh() { adb shell "$@" | tr -d '\r'; }
[ "$(adb get-state)" = recovery ] || { echo "phone is not in recovery" >&2; exit 1; }
[ "$(sh getprop ro.product.device)" = starlte ] || { echo "not a starlte" >&2; exit 1; }
[ "$(sh readlink -f "$BY")" = "$NODE" ] || { echo "$PART is not $NODE" >&2; exit 1; }
SIZE=$(stat -c %s "$IMG")
[ "$SIZE" -le "$(sh blockdev --getsize64 "$BY")" ] || { echo "image does not fit" >&2; exit 1; }
SUM=$(sha256sum "$IMG" | cut -d' ' -f1)
adb push "$IMG" /tmp/zboot-flash.img >/dev/null
[ "$(sh sha256sum /tmp/zboot-flash.img | cut -d' ' -f1)" = "$SUM" ] || { echo "USB copy mismatch" >&2; exit 1; }
sh "dd if=/tmp/zboot-flash.img of=$BY bs=1048576 2>/dev/null && sync"
BACK=$(sh "head -c $SIZE $BY | sha256sum" | cut -d' ' -f1)
sh rm -f /tmp/zboot-flash.img
[ "$BACK" = "$SUM" ] || { echo "READBACK MISMATCH on $PART" >&2; exit 1; }
echo "FLASHED $PART <- $(basename "$IMG") sha256=$SUM bytes=$SIZE (readback verified)"
