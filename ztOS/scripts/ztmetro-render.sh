#!/bin/bash
# Draw the shell's own layers (status bar, nav bar, action centre, keyboard,
# tiles) and the settings app's pages into PNGs on the phone, without touching
# the session on its screen, and bring them back to out/render/.
#
#   scripts/ztmetro-render.sh
#
# Uses the binaries built in ~/ztmetro on the phone (scripts/ztmetro-push.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PHONE=172.16.42.1 HOST=172.16.42.2 PORT=5583
mkdir -p "$ROOT/out/render"
timeout 60 socat -u "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" "OPEN:$ROOT/build/render.tar,creat,trunc" &
sleep 1
timeout 90 "$ROOT/scripts/ssh-pw.py" alarm alarm "$PHONE" \
	"rm -rf /tmp/zr && mkdir -p /tmp/zr && cd ~/ztmetro && ./ztmetro --render /tmp/zr 2>&1 | grep -v INFO | head -5; \
	 for p in main theme display wifi battery keyboard about; do ./settings/ztsettings --render \$p /tmp/zr/settings-\$p.png 2>&1 | head -2; done; \
	 cd /tmp/zr && tar cf - . > /dev/tcp/$HOST/$PORT" | grep -v '^Password' || true
wait || true
tar xf "$ROOT/build/render.tar" -C "$ROOT/out/render"
ls "$ROOT/out/render"
