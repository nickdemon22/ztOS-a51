#!/bin/sh
# the app itself: forked from the warm runtime when there is one, else the launcher's own process
apppid() { z=$(pgrep -f '.local/share/ztos/[z]ygote.py' | head -1); if [ -n "$z" ] && pgrep -n -P $z >/dev/null; then pgrep -n -P $z; else pgrep -f "$1" | head -1; fi; }
# Notes restarted (profiler on); CPU it uses: idle 10 s, then 12 swipes; perfd's boosts meanwhile
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
A=$HOME/.local/share/ztos/apps/com.example.notes
mkdir -p ~/.config/ztos; [ "$1" = noprof ] && rm -f ~/.config/ztos/profile || touch ~/.config/ztos/profile
# wake and unlock (the phone is unattended), as drive.sh does
s "screen on"; sleep 1
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.5
pkill -f "$HOME/.local/share/ztos/apps/[c]om.example.notes"; sleep 1
echo "launch $(date +%T)"; s launch $A; sleep 7
s state; sleep 0.3; grep "state:" ~/.local/state/ztmetro.log | tail -1
P=$(apppid "$HOME/.local/share/ztos/apps/[c]om.example.notes")
tick() { awk '{print $14 + $15}' /proc/$P/stat; }
HZ=$(getconf CLK_TCK)
since=$(date +%s.%N)
t0=$(tick); sleep 10; t1=$(tick)
echo "idle 10 s: $(( (t1 - t0) * 1000 / HZ )) ms of CPU"
drag() { s down 1 $1 400; i=1; while [ $i -le $3 ]; do s move 1 $(( $1 + ($2 - $1) * i / $3 )) 402; sleep 0.016; i=$((i + 1)); done; s up 1; }
echo "swipes from $(date +%T)"; w0=$(date +%s%N); t0=$(tick)
for k in 1 2 3; do drag 300 60 20; sleep 1.2; drag 60 300 20; sleep 1.2; done
for k in 1 2 3; do drag 300 100 6; sleep 1; drag 100 300 6; sleep 1; done
sleep 1
t1=$(tick); w1=$(date +%s%N); echo "swipes to $(date +%T)"
ms=$(( (t1 - t0) * 1000 / HZ )); wall=$(( (w1 - w0) / 1000000 ))
echo "12 swipes over $wall ms: $ms ms of CPU = $(( ms * 100 / wall ))% of one core on average"
echo "boosts:"; journalctl -u zrnperfd --since "@$(( ${since%.*} - 10 ))" --no-pager -q 2>/dev/null | sed -n "s/^.* \([0-9:]*\) starlte.*\(boost [a-z]*\).*/\1 \2/p"
rm -f ~/.config/ztos/profile
echo MEASURE-DONE
