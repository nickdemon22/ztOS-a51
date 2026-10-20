#!/bin/sh
# rec.sh on|off: record one launch of Settings and one return to Start, driven
# through the shell's control pipe. With "on", in slow motion (5x).
C=$XDG_RUNTIME_DIR/ztmetro.ctl
CFG=$HOME/.config/ztmetro/config
sed -i "s/^slow_motion = .*/slow_motion = $1/" $CFG; pkill -HUP -x ztmetro; sleep 1
echo "screen on" > $C; sleep 1
# past the lock screen, if it is up: a swipe from low to high
echo "down 1 180 620" > $C; for y in 560 480 400 320 240 160; do echo "move 1 180 $y" > $C; sleep 0.03; done; echo "up 1" > $C; sleep 2
rm -rf /tmp/ztrec; echo "record 40000" > $C; sleep 0.5
echo "tap 270 80" > $C          # the Settings tile (2-column grid: top right)
sleep ${2:-7}
echo "tap 60 722" > $C          # back, in the nav bar
sleep ${2:-7}
ls /tmp/ztrec | wc -l
grep -E "launched|showing|nav:" ~/.local/state/ztmetro.log | tail -3
