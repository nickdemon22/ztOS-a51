#!/bin/bash
# Write the Arch root image into USERDATA from the running zboot rescue shell,
# over the USB network, in verified chunks.
#
#   scripts/flash-root.sh [image] [chunk MiB]
#
# Each chunk is compared against the partition first and only sent when it
# differs, then read back and compared again. That makes the whole run
# idempotent and resumable: a transfer killed halfway is finished by running
# the script again, and an unchanged chunk costs one hash on each side.
#
# Two things about busybox on the phone shape this:
#   * `nc -l` never notices the peer closing, so the phone dials out to a
#     listener on the host instead of listening itself;
#   * `dd` counts a short pipe read as a whole record, so `count=` without
#     `iflag=fullblock` stops after 256 32 KB reads (8 MB), not 256 MiB.
# The receiver is started detached and waited for by watching its dd, because
# the phone's shell will not return while nc is still holding the pipeline.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG=${1:-$ROOT/out/arch-root.img}
CHUNK=${2:-256}
PART=/dev/sda25
HOST=172.16.42.2
PORT=5557
PSH=$ROOT/scripts/psh.py
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -s "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
SIZE=$(stat -c %s "$IMG")
(( SIZE % (CHUNK * 1048576) == 0 )) || { echo "image size is not a multiple of ${CHUNK}M" >&2; exit 1; }
N=$(( SIZE / (CHUNK * 1048576) ))
echo "image $IMG: $SIZE bytes, $N chunks of ${CHUNK}M -> $PART"

psize=$("$PSH" -t 20 "blockdev --getsize64 $PART" | tr -dc 0-9)
[ -n "$psize" ] && [ "$psize" -ge "$SIZE" ] ||
	{ echo "partition too small or unreachable ($psize)" >&2; exit 1; }
"$PSH" -t 10 "touch /run/zboot.hold" >/dev/null 2>&1 || true

# sha256 of one chunk of the partition, as the phone reads it
part_hash() {
	"$PSH" -t 300 "dd if=$PART bs=1M skip=$1 count=$CHUNK iflag=fullblock 2>/dev/null | sha256sum" |
		awk 'NF {print $1; exit}'
}

writer_gone() {
	[ -z "$("$PSH" -t 30 'ps | grep "dd of=\|dd of " | grep -v grep')" ]
}

cleanup_phone() {
	"$PSH" -t 30 'killall nc gunzip dd 2>/dev/null; sync; true' >/dev/null 2>&1
}

sent=0 skipped=0
for ((i = 0; i < N; i++)); do
	off=$(( i * CHUNK ))
	want=$(dd if="$IMG" bs=1M skip="$off" count="$CHUNK" status=none | sha256sum | cut -d' ' -f1)
	have=$(part_hash "$off")
	if [ "$want" = "$have" ]; then
		skipped=$((skipped + 1))
		printf '%3d/%d  skip (identical)  %s\n' "$((i + 1))" "$N" "$(date +%T)"
		continue
	fi
	dd if="$IMG" bs=1M skip="$off" count="$CHUNK" status=none | gzip -1 -c > "$TMP/chunk.gz"
	back=
	for try in 1 2 3; do
		cleanup_phone
		socat -u "OPEN:$TMP/chunk.gz" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" 2>/dev/null &
		server=$!
		sleep 1
		"$PSH" -t 60 "setsid sh -c 'nc $HOST $PORT | gunzip | dd of=$PART bs=1M seek=$off count=$CHUNK iflag=fullblock conv=notrunc,fsync' >/dev/null 2>&1 & echo started" >/dev/null
		for ((w = 0; w < 60; w++)); do
			sleep 5
			writer_gone && break
		done
		wait $server 2>/dev/null
		kill $server 2>/dev/null
		cleanup_phone
		back=$(part_hash "$off")
		[ "$back" = "$want" ] && break
		echo "   chunk $i readback mismatch (try $try)" >&2
	done
	[ "$back" = "$want" ] || { echo "chunk $i failed three times, stopping" >&2; exit 1; }
	sent=$((sent + 1))
	printf '%3d/%d  written and verified  %s\n' "$((i + 1))" "$N" "$(date +%T)"
done
echo "FLASH_ROOT_OK $sent chunk(s) written, $skipped already identical"
