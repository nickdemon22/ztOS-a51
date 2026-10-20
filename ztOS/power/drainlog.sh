#!/bin/sh
# power/drainlog.sh SECONDS [STEP]: log the battery every STEP seconds to
# /var/tmp/drain.log: time, %, voltage mV, current mA (now, average), whether
# a cable is in, the screen state zrnperfd last heard.
B=/sys/class/power_supply/max170xx_battery
end=$(( $(date +%s) + ${1:-1800} ))
while [ "$(date +%s)" -lt "$end" ]; do
	usb=0; for o in /sys/class/power_supply/*/online; do [ "$(cat $o)" = 1 ] && usb=1; done
	scr=$(sed -n 's/^screen=//p' /run/zrnperfd.status)
	echo "$(date +%s) $(cat $B/capacity) $(( $(cat $B/voltage_now) / 1000 )) $(( $(cat $B/current_now) / 1000 )) $(( $(cat $B/current_avg) / 1000 )) usb=$usb screen=$scr"
	sleep "${2:-10}"
done
