#!/bin/sh
# Log what could make the phone warm with the screen off, every 2 minutes, to
# /var/lib/ztos-power-test/heat-<boot time>.log. Run as root:
#   systemd-run --unit=ztos-heatlog /usr/local/libexec/ztos-power/heat-log.sh
# One line per sample: time, screen, charging, battery temp (0.1 C),
# current (uA, + charging), voltage (uV), load, online CPUs, CPU clocks
# (kHz), the busiest process, Wi-Fi, modem.
B=/sys/class/power_supply/max170xx_battery
LOG=/var/lib/ztos-power-test/heat-$(date +%Y%m%dT%H%M%S).log
mkdir -p "$(dirname "$LOG")"
echo "# time screen charging temp current voltage load online clocks top wifi modem" > "$LOG"
while :; do
	screen=$(sed -n 's/^screen=//p' /run/zrnperfd.status 2>/dev/null)
	cond=$(sed -n 's/^condition=//p' /run/zrnperfd.status 2>/dev/null)
	clocks=$(cat /sys/devices/system/cpu/cpu[04]/cpufreq/scaling_cur_freq 2>/dev/null | tr '\n' '/')
	top=$(top -b -n 2 -d 2 | awk '/^ *PID/ {n++; next} n == 2 && NF > 11 {print $12 ":" $9; exit}')
	wifi=$(cat /sys/class/net/wlp1s0/operstate 2>/dev/null)
	modem=$(systemctl is-active ztoscorenetworking-modem.service 2>/dev/null)
	echo "$(date +%T) $screen $cond $(cat $B/temp) $(cat $B/current_now) $(cat $B/voltage_now) $(cut -d' ' -f1 /proc/loadavg) $(cat /sys/devices/system/cpu/online) $clocks $top $wifi $modem" >> "$LOG"
	sleep 118
done
