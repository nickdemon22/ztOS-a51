#!/bin/sh
# record Notes opening (profiled): from the tap until it has settled
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
s "screen on"; sleep 0.8
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.2
pkill -f "share/ztos/apps/[c]om.example.notes"; s tap 60 722; sleep 1.5
mkdir -p ~/.config/ztos; touch ~/.config/ztos/profile
rm -rf /tmp/ztrec; s "record 3500"; sleep 0.1
s launch $HOME/.local/share/ztos/apps/com.example.notes; sleep 4
rm -f ~/.config/ztos/profile
grep -E "ztos start|GPU" ~/.local/state/ztos/com.example.notes.log
grep -E "showing app|launched" ~/.local/state/ztmetro.log | tail -2
echo REC-DONE
