#!/bin/bash
# Write a boot image to BOOT from the running Arch system, over the USB
# network. Used to test a new kernel without going through TWRP.
#
#   scripts/flash-boot-from-arch.sh <image>
#
# RECOVERY (TWRP) is never touched: a kernel that does not come up resets into
# it through the watchdog, which is the way back. The current BOOT content is
# saved to out/boot-previous.img first.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG=${1:?image}
PART=/dev/sda10          # BOOT
HOST=172.16.42.2
PHONE=172.16.42.1
PORT=5559
SSH="$ROOT/scripts/ssh-pw.py alarm "${ZT_PW:-alarm},root" $PHONE"

[ "$(head -c 8 "$IMG")" = "ANDROID!" ] || { echo "not an Android boot image" >&2; exit 1; }
SIZE=$(stat -c %s "$IMG")
SUM=$(sha256sum "$IMG" | cut -d' ' -f1)

# RECOVERY has to hold a bootable TWRP before BOOT is overwritten.
rec=$($SSH "su -c 'head -c 8 /dev/sda11'" | tail -1)
case "$rec" in
*ANDROID!*) ;;
*) echo "RECOVERY does not start with ANDROID!: $rec" >&2; exit 1 ;;
esac

# Listener first: the phone's /dev/tcp connect fails outright when nothing is
# listening yet, and the listener would then wait for a client that never comes.
socat -u "OPEN:$IMG" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" 2>/dev/null &
server=$!
sleep 1
$SSH "su -c 'cat < /dev/tcp/$HOST/$PORT > /tmp/boot.img; sync'" >/dev/null
wait $server 2>/dev/null || true

# su prints its own "Password:" prompt into the stream; take the hash itself.
got=$($SSH "su -c 'sha256sum /tmp/boot.img'" | grep -Eo '[0-9a-f]{64}' | tail -1)
[ "$got" = "$SUM" ] || { echo "transfer mismatch: $got != $SUM" >&2; exit 1; }
$SSH "su -c 'head -c $(stat -c %s "$IMG") $PART > /tmp/boot-previous.img 2>/dev/null; dd if=/tmp/boot.img of=$PART bs=1M conv=fsync status=none; sync'" >/dev/null
back=$($SSH "su -c 'head -c $SIZE $PART | sha256sum'" | grep -Eo '[0-9a-f]{64}' | tail -1)
[ "$back" = "$SUM" ] || { echo "READBACK MISMATCH on BOOT" >&2; exit 1; }
$SSH "su -c 'rm -f /tmp/boot.img'" >/dev/null
echo "FLASHED BOOT <- $(basename "$IMG") sha256=$SUM bytes=$SIZE (readback verified)"
