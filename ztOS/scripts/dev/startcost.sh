#!/bin/sh
apppid() { z=$(pgrep -f '.local/share/ztos/[z]ygote.py' | head -1); if [ -n "$z" ] && pgrep -n -P $z >/dev/null; then pgrep -n -P $z; else pgrep -f "$1" | head -1; fi; }
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
HZ=$(getconf CLK_TCK)
rm -f ~/.config/ztos/profile
s "screen on"; sleep 1
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.5
A=$HOME/.local/share/ztos/apps/com.example.notes; pat="$HOME/.local/share/ztos/apps/[c]om.example.notes"
pkill -f '.local/share/ztos/[z]ygote.py'; pkill -f "$pat"; sleep 1.5
for run in cold warm; do
	s launch $A; sleep 8
	P=$(apppid "$pat")
	echo "notes ($run): $(awk -v hz=$HZ '{printf "%d ms of CPU to start", ($14 + $15) * 1000 / hz}' /proc/$P/stat) (pid $P)"
	s tap 60 722; sleep 1; pkill -f "$pat"; sleep 4
done
z=$(pgrep -f '.local/share/ztos/[z]ygote.py' | head -1); [ -n "$z" ] && echo "warm runtime: pid $z, $(awk '{print $24 * 4 / 1024}' /proc/$z/stat) MB resident"
pkill -x ztphone; sleep 1; s launch ztphone; sleep 8
echo "ztphone: $(awk -v hz=$HZ '{printf "%d ms of CPU to start", ($14 + $15) * 1000 / hz}' /proc/$(pgrep -x ztphone)/stat)"
s tap 60 722; pkill -x ztphone
echo START-DONE
