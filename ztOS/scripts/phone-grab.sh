#!/bin/bash
# Save what the phone's display is scanning out right now as a PNG.
#
#   scripts/phone-grab.sh <output.png>
#
# Reads the live scanout buffer out of /dev/mem on the phone (the address is
# in DECON's IDMA register), so it shows exactly what is on the glass whatever
# compositor is running. Needs /root/fbgrab.py on the phone (build/fbgrab.py).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:?output.png}
RAW=$(mktemp "$ROOT/build/grab.XXXXXX")
trap 'rm -f "$RAW"' EXIT
timeout 60 socat -u TCP-LISTEN:5576,bind=172.16.42.2,reuseaddr "OPEN:$RAW,creat,trunc" &
sleep 1
timeout 50 "$ROOT/scripts/ssh-pw.py" alarm "${ZT_PW:-alarm},root" 172.16.42.1 \
	"su -c 'python3 /root/fbgrab.py 0 2>/dev/null'" >/dev/null
wait || true
[ "$(stat -c %s "$RAW")" = 17049600 ] || { echo "incomplete grab" >&2; exit 1; }
python3 -I - "$RAW" "$OUT" <<'PY'
import sys
from PIL import Image
im = Image.frombuffer('RGBA', (1440, 2960), open(sys.argv[1], 'rb').read(), 'raw', 'BGRA', 0, 1)
im.convert('RGB').save(sys.argv[2])
PY
