#!/bin/sh
# drive.sh SCENARIO: wake and unlock the shell, record, and act one scenario
# out through its control pipe (see ztmetro.c, ctl_command). For checking
# Start's physics, the app list and rearranging on the phone itself.
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { echo "$*" > $C; }
# a drag from (x0,y0) to (x1,y1) in N steps of 16 ms; "hold" keeps the finger down
drag() { id=$1; x0=$2; y0=$3; x1=$4; y1=$5; n=$6
	s down $id $x0 $y0; i=1
	while [ $i -le $n ]; do
		s move $id $((x0 + (x1 - x0) * i / n)) $((y0 + (y1 - y0) * i / n)); sleep 0.016; i=$((i + 1))
	done
	[ "$7" = hold ] || s up $id; }
s "screen on"; sleep 1
drag 1 180 620 180 160 6; sleep 1.5
# back to the tiles, at the top: out of a menu, the list, editing
s tap 60 722; sleep 0.4; s tap 60 722; sleep 0.4; s tap 60 722; sleep 0.6; s tap 180 722; sleep 1
rm -rf /tmp/ztrec; s "record ${2:-6000}"; sleep 0.3
case $1 in
flick) drag 1 180 600 180 300 8; sleep 2.5; drag 1 180 250 180 650 6; sleep 2.5 ;;
list) drag 1 300 300 60 300 8; sleep 1.2; drag 1 180 600 180 200 6; sleep 2.5 ;;
listtap) drag 1 300 300 60 300 8; sleep 1.2; s tap 150 ${3:-110}; sleep 2.5; s tap 180 722; sleep 2.5 ;;
hold) s down 1 ${3:-180} ${4:-90}; sleep 0.8; drag 1 ${3:-180} ${4:-90} ${5:-80} ${6:-320} 25 hold; sleep 1; s up 1; sleep 1.2 ;;
menu) drag 1 300 300 60 300 8; sleep 1.2; s down 1 120 ${3:-110}; sleep 0.8; s up 1; sleep 1.2 ;;
launch) s down 5 ${3:-180} ${4:-90}; sleep 0.15; s up 5; sleep 3; s tap 60 722; sleep 3 ;;
shot) sleep 0.3 ;;
# for timing, in nominal: the action centre pulled down and pushed up slowly, twice
pull) drag 6 180 10 180 560 45; sleep 0.8; drag 6 180 640 180 60 45; sleep 0.8; drag 6 180 10 180 560 25; sleep 0.8; drag 6 180 640 180 60 25; sleep 0.8 ;;
# Settings (its tile is at $3 $4) opened and its list dragged up and down slowly, then flung
setscroll) s launch ztsettings; sleep 3; drag 2 180 600 180 200 45; sleep 0.6; drag 2 180 200 180 600 45; sleep 0.6; drag 2 180 600 180 300 8; sleep 1.5; drag 2 180 300 180 620 8; sleep 1.5; s tap 60 722; sleep 1 ;;
# the app list's search: the button, the box, two letters; and the list scrolled a little
search) drag 1 300 300 60 300 8; sleep 1.2; s tap 51 67; sleep 1.3; s tap 112 394; sleep 0.4; s tap 300 394; sleep 1.2 ;;
listscroll) drag 1 300 300 60 300 8; sleep 1.2; drag 1 180 400 180 340 6 hold; sleep 0.8; s up 1; sleep 1 ;;
# the action centre: Wi-Fi off and on (the status icons come and go), the data tile, a notification
shade) drag 6 180 10 180 500 12; sleep 1.2; s tap 45 80; sleep 1.6; s tap 135 80; sleep 1.2; s "notify Test|A notification, as a component would send it."; sleep 1.5; s tap 45 80; sleep 1; s tap 135 80; sleep 0.6 ;;
peek) s down 6 180 10; sleep 1.2; s up 6; sleep 1; s down 6 180 10; sleep 0.1; drag 6 180 10 180 500 12; sleep 1.5 ;;
settings) s down 5 180 90; sleep 0.1; s up 5; sleep 2.5; drag 2 180 600 180 250 8; sleep 1.5; drag 2 180 250 180 620 8; sleep 1.5; s down 3 120 190; sleep 0.1; s up 3; sleep 1.5; s tap 60 722; sleep 1.5; drag 2 300 400 40 400 10; sleep 1.2; drag 2 180 600 180 300 8; sleep 1.5; drag 2 60 400 320 400 10; sleep 1.2; s tap 60 722; sleep 1.5 ;;
pivot) s tap 180 90; sleep 2.5; drag 2 300 400 60 400 10; sleep 1.2; drag 2 300 400 260 400 5; sleep 1; s tap 300 60; sleep 1.2; s tap 60 722; sleep 1.5 ;;
arrow) s tap 330 293; sleep 1; s tap 51 67; sleep 0.8; s tap 100 ${3:-560}; sleep 0.5; s tap 260 ${3:-560}; sleep 1 ;;
resize) s down 1 180 208; sleep 0.8; s up 1; sleep 0.5; s tap 216 260; sleep 0.6; cat ~/.config/ztmetro/tiles | grep Console; s tap 60 722; sleep 0.6 ;;
esac
s state; sleep 0.3
ls /tmp/ztrec | wc -l
grep -E "state:|start:|launched|pinned|unpinned|animation:" ~/.local/state/ztmetro.log | tail -4
