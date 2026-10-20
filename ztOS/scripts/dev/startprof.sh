#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
mkdir -p ~/.config/ztos; touch ~/.config/ztos/profile
s "screen on"; sleep 1
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.5
pat="$HOME/.local/share/ztos/apps/[c]om.example.notes"; pkill -f "$pat"; sleep 2
s launch $HOME/.local/share/ztos/apps/com.example.notes; sleep 9
rm -f ~/.config/ztos/profile
grep -E "ztos start" ~/.local/state/ztos/com.example.notes.log
grep -A12 "ztos profile" ~/.local/state/ztos/com.example.notes.log | head -16
s tap 60 722; pkill -f "$pat"
echo PROF-DONE
