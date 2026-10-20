#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
mkdir -p ~/.config/ztos; touch ~/.config/ztos/profile
s "screen on"; sleep 0.8
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.2
pkill -f "share/ztos/apps/[c]om.example.notes"; sleep 1.5
# once to warm up (after an install the warm runtime starts fresh), then the one recorded
s launch $HOME/.local/share/ztos/apps/com.example.notes; sleep 7
pkill -f "share/ztos/apps/[c]om.example.notes"; s tap 60 722; sleep 2
rm -rf /tmp/ztrec /tmp/ztrec-open; s "record 6000"; sleep 0.1
s launch "$HOME/.local/share/ztos/apps/com.example.notes"; sleep 6.5
mv /tmp/ztrec /tmp/ztrec-open
# to the settings tab, then a tap on the name box
s down 1 300 400; i=1; while [ $i -le 8 ]; do s move 1 $((300 - 30 * i)) 402; sleep 0.016; i=$((i + 1)); done; s up 1; sleep 1.2
s record 1800; sleep 0.1
s down 2 150 152; sleep 0.08; s up 2; sleep 1.5
rm -f ~/.config/ztos/profile
grep -E "ztos start" ~/.local/state/ztos/com.example.notes.log
grep -E "showing app|launched|keyboard|ready|Loading|loading" ~/.local/state/ztmetro.log | tail -10 | cut -c1-150
echo REC3-DONE
