#!/bin/sh
# restart Notes with the profiler on, then six slow swipes and six flicks
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
A=$HOME/.local/share/ztos/apps/com.example.notes
mkdir -p ~/.config/ztos && touch ~/.config/ztos/profile
pkill -f "$HOME/.local/share/ztos/apps/[c]om.example.notes"; sleep 1
s launch $A; sleep 6
drag() { s down 1 $1 400; i=1; while [ $i -le $3 ]; do s move 1 $(( $1 + ($2 - $1) * i / $3 )) 402; sleep 0.016; i=$((i + 1)); done; s up 1; }
for k in 1 2 3; do drag 300 60 20; sleep 1.2; drag 60 300 20; sleep 1.2; done
for k in 1 2 3; do drag 300 100 6; sleep 1; drag 100 300 6; sleep 1; done
sleep 2.5
rm -f ~/.config/ztos/profile
echo DRIVE-DONE
