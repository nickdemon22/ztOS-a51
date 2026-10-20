#!/bin/bash
# audio/push.sh FILE...: copy files to /tmp/zrn/ on the phone (over Wi-Fi by default).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PHONE=${PHONE:-192.168.8.102} HOST=${HOST:-192.168.8.100} PORT=5587
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for f in "$@"; do cp "$f" "$T/"; done
tar cf "$T.tar" -C "$T" .
timeout 30 socat -u "OPEN:$T.tar" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" &
sleep 1
timeout 40 "$ROOT/scripts/ssh-pw.py" alarm "${ZT_PW:-alarm}" "$PHONE" "mkdir -p /tmp/zrn && cat < /dev/tcp/$HOST/$PORT | tar xf - -C /tmp/zrn 2>/dev/null; ls /tmp/zrn | tr '\n' ' '"
wait || true; rm -f "$T.tar"
