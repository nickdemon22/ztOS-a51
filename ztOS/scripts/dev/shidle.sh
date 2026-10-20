#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
HZ=$(getconf CLK_TCK); S=$(pgrep -x ztmetro)
m() { a=$(awk '{print $14+$15}' /proc/$S/stat); sleep 8; b=$(awk '{print $14+$15}' /proc/$S/stat); echo "$1: shell $(( (b-a)*1000/HZ )) ms CPU in 8 s"; }
s "screen on"; sleep 1
m "Notes in front"
s tap 60 722; sleep 2; m "Start"
s launch ztsettings; sleep 4; m "Settings in front"
s tap 60 722; sleep 1
grep -E "loading screen|ready|kept behind" ~/.local/state/ztmetro.log | tail -3
echo SH-DONE
