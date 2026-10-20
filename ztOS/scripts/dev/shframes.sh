#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
C=$XDG_RUNTIME_DIR/ztmetro.ctl
s() { printf '%s\n' "$*" > $C; }
s "screen on"; s tap 60 722; sleep 2
rm -rf /tmp/ztrec; s "record 3000"; sleep 3.3
echo "frames drawn in 3 s of idle Start: $(ls /tmp/ztrec | wc -l)"
ls /tmp/ztrec | head -3
echo F-DONE
