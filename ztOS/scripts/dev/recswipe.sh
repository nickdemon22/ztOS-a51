#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
s "screen on"; sleep 0.8
s down 9 180 620; for y in 540 460 380 300 220 160; do s move 9 180 $y; sleep 0.016; done; s up 9; sleep 1.2
s launch $HOME/.local/share/ztos/apps/com.example.notes; sleep 1.5
rm -rf /tmp/ztrec; s "record 1500"; sleep 0.1
s down 1 300 400; i=1; while [ $i -le 14 ]; do s move 1 $((300 - 14 * i)) 402; sleep 0.03; i=$((i + 1)); done; s up 1
sleep 2
echo REC-DONE
