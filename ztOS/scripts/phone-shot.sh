#!/bin/bash
# Grab what is on the phone's screen and bring the PNG back here.
#
#   scripts/phone-shot.sh [output.png]
#
# The screenshot tool depends on the compositor: KWin has no wlr-screencopy, so
# Plasma is captured with spectacle, and phoc / Hyprland with grim. The session
# environment is read out of the running compositor's /proc entry, because an
# ssh login has no Wayland display of its own.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-$ROOT/out/screen.png}
PHONE=172.16.42.1
HOST=172.16.42.2
PORT=5561
SSH="$ROOT/scripts/ssh-pw.py alarm "${ZT_PW:-alarm},root" $PHONE"

$SSH "su -c '
	for c in kwin_wayland Hyprland phoc; do
		pid=\$(pgrep -x \$c | head -1) && [ -n \"\$pid\" ] && break
	done
	[ -n \"\$pid\" ] || { echo no-compositor; exit 1; }
	eval \$(tr \"\\0\" \"\\n\" < /proc/\$pid/environ | grep -E \"^(WAYLAND_DISPLAY|XDG_RUNTIME_DIR|DBUS_SESSION_BUS_ADDRESS|XDG_SESSION_TYPE)=\" | sed \"s/^/export /;s/=/=\\\"/;s/\$/\\\"/\")
	rm -f /tmp/shot.png
	case \$c in
	kwin_wayland) sudo -u alarm env WAYLAND_DISPLAY=\$WAYLAND_DISPLAY XDG_RUNTIME_DIR=\$XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS=\$DBUS_SESSION_BUS_ADDRESS spectacle -b -n -f -o /tmp/shot.png >/dev/null 2>&1 ;;
	*)            sudo -u alarm env WAYLAND_DISPLAY=\$WAYLAND_DISPLAY XDG_RUNTIME_DIR=\$XDG_RUNTIME_DIR grim /tmp/shot.png >/dev/null 2>&1 ;;
	esac
	sleep 2
	ls -l /tmp/shot.png 2>/dev/null || echo no-shot
'" | tail -2

socat -u "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" "OPEN:$OUT,creat,trunc" &
server=$!
sleep 1
$SSH "su -c 'cat /tmp/shot.png > /dev/tcp/$HOST/$PORT'" >/dev/null
wait $server 2>/dev/null || true
ls -l "$OUT"
file "$OUT" 2>/dev/null || true
