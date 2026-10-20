#!/bin/bash
# Write a boot image to RECOVERY from the running zboot rescue shell, over the
# USB network. Used when the phone is already running our kernel and TWRP (on
# BOOT) is the fallback, so there is no adb to flash through.
#
#   scripts/flash-from-rescue.sh <image>
#
# BOOT is never touched: it holds TWRP, which is the way back.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG=${1:?image}
PART=/dev/sda11          # RECOVERY
HOST=172.16.42.2
PORT=5558
PSH=$ROOT/scripts/psh.py
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ "$(head -c 8 "$IMG")" = "ANDROID!" ] || { echo "not an Android boot image" >&2; exit 1; }
SIZE=$(stat -c %s "$IMG")
SUM=$(sha256sum "$IMG" | cut -d' ' -f1)
psize=$("$PSH" -t 20 "blockdev --getsize64 $PART" | tr -dc 0-9)
[ -n "$psize" ] && [ "$SIZE" -le "$psize" ] || { echo "does not fit RECOVERY ($psize)" >&2; exit 1; }

gzip -1 -c "$IMG" > "$TMP/img.gz"
"$PSH" -t 30 'killall nc gunzip 2>/dev/null; rm -f /tmp/boot.img; true' >/dev/null
socat -u "OPEN:$TMP/img.gz" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" 2>/dev/null &
server=$!
sleep 1
"$PSH" -t 300 "nc $HOST $PORT | gunzip > /tmp/boot.img; sha256sum /tmp/boot.img" | awk 'NF{print "   received:", $1}'
wait $server 2>/dev/null

got=$("$PSH" -t 60 'sha256sum /tmp/boot.img' | awk 'NF{print $1; exit}')
[ "$got" = "$SUM" ] || { echo "transfer mismatch: $got != $SUM" >&2; exit 1; }
"$PSH" -t 120 "dd if=/tmp/boot.img of=$PART bs=1M conv=fsync 2>/dev/null; sync; echo written" >/dev/null
back=$("$PSH" -t 120 "head -c $SIZE $PART | sha256sum" | awk 'NF{print $1; exit}')
[ "$back" = "$SUM" ] || { echo "READBACK MISMATCH on RECOVERY" >&2; exit 1; }
"$PSH" -t 30 'rm -f /tmp/boot.img; true' >/dev/null
echo "FLASHED RECOVERY <- $(basename "$IMG") sha256=$SUM bytes=$SIZE (readback verified)"
