#!/bin/bash
# Copy ztmetro/ to the phone, build it there, and install it.
#
#   scripts/ztmetro-push.sh [--start]     --start also switches the session to it
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# over USB by default; over Wi-Fi: PHONE=192.168.8.102 HOST=192.168.8.100 scripts/ztmetro-push.sh
PHONE=${PHONE:-172.16.42.1} HOST=${HOST:-172.16.42.2} PORT=5581
tar cf "$ROOT/build/ztm.tar" -C "$ROOT/ztmetro" --exclude=ref .
timeout 25 socat -u "OPEN:$ROOT/build/ztm.tar" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" &
sleep 1
timeout 240 "$ROOT/scripts/ssh-pw.py" alarm alarm "$PHONE" \
	"mkdir -p ~/ztmetro && cd ~/ztmetro && cat < /dev/tcp/$HOST/$PORT | tar xf - 2>/dev/null; make 2>&1 | grep -E 'error|warning: |Error' | head -40; ls -l ztmetro"
wait || true
timeout 60 "$ROOT/scripts/ssh-pw.py" alarm alarm,root "$PHONE" \
	"su -c 'make -s -C /home/alarm/ztmetro install && ls -l /usr/local/bin/ztmetro'" | tail -1
if [ "${1:-}" = --start ]; then
	timeout 40 "$ROOT/scripts/ssh-pw.py" alarm alarm,root "$PHONE" "su -c 'zboot-session ztmetro | tail -1'"
fi
