#!/bin/sh
# power-survey.sh: where the power goes, read-only (run as root on the phone).
# What is on, how hot, and what wakes the processor, over a few seconds.
S=${1:-10}
echo "== $(date) up $(cut -d' ' -f1 /proc/uptime) s"
echo "== battery / charger"
for p in /sys/class/power_supply/*; do
	printf '%s: ' "$(basename $p)"
	for f in status capacity voltage_now current_now current_avg temp online type input_current_limit; do
		[ -r $p/$f ] && printf '%s=%s ' $f "$(cat $p/$f 2>/dev/null)"
	done; echo
done
echo "== thermal (milli-C)"
for z in /sys/class/thermal/thermal_zone*; do printf '%s %s\n' "$(cat $z/type)" "$(cat $z/temp 2>/dev/null)"; done
echo "== cpu"
echo "online $(cat /sys/devices/system/cpu/online) | cpuidle driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>&1)"
for c in 0 4; do echo "policy$c: $(cat /sys/devices/system/cpu/cpufreq/policy$c/scaling_cur_freq) kHz gov $(cat /sys/devices/system/cpu/cpufreq/policy$c/scaling_governor) max $(cat /sys/devices/system/cpu/cpufreq/policy$c/scaling_max_freq)"; done
ls /sys/devices/system/cpu/cpu0/cpuidle 2>&1 | head -3
echo "== devfreq"
for d in /sys/class/devfreq/*; do printf '%s cur=%s min=%s max=%s gov=%s\n' "$(basename $d)" "$(cat $d/cur_freq 2>/dev/null)" "$(cat $d/min_freq 2>/dev/null)" "$(cat $d/max_freq 2>/dev/null)" "$(cat $d/governor 2>/dev/null)"; done
echo "== regulators on (use_count>0)"
awk 'NR>3 && $2>0 {print}' /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | cut -c1-110 | head -60
echo "== clocks enabled: $(awk 'NR>2 && $3>0' /sys/kernel/debug/clk/clk_summary 2>/dev/null | wc -l) of $(wc -l < /sys/kernel/debug/clk/clk_summary 2>/dev/null)"
echo "== radios"
rfkill list 2>/dev/null | head -12
iw dev 2>/dev/null | grep -E "Interface|type|txpower" | head
iw dev wlan0 get power_save 2>/dev/null
cat /run/zrn-modem/status 2>/dev/null | tr '\n' ' '; echo
echo "== sample $S s"
cp /proc/interrupts /tmp/irq0; cat /proc/stat | head -9 > /tmp/stat0
ps -eo pid,time,comm --sort=-time | head -1 > /dev/null
ps -eo pid,cputime,comm > /tmp/ps0
sleep $S
cp /proc/interrupts /tmp/irq1; cat /proc/stat | head -9 > /tmp/stat1
echo "-- interrupts per second (top 14)"
awk -v s=$S 'NR==FNR { if ($1 ~ /:$/) { k=$1; t=0; for (i=2;i<=9;i++) t+=$i; a[k]=t; } next }
	{ if ($1 ~ /:$/) { k=$1; t=0; for (i=2;i<=9;i++) t+=$i; d=(t-a[k])/s; if (d>0.5) { n=""; for (j=11;j<=NF;j++) n=n" "$j; printf "%8.1f %s%s\n", d, k, n } } }' /tmp/irq0 /tmp/irq1 | sort -rn | head -14
echo "-- cpu busy (percent of the sample, per core)"
awk 'NR==FNR { if ($1 ~ /^cpu[0-9]/) { b[$1]=$2+$3+$4+$7+$8; t[$1]=$2+$3+$4+$5+$6+$7+$8 } next }
	{ if ($1 ~ /^cpu[0-9]/) { db=($2+$3+$4+$7+$8)-b[$1]; dt=($2+$3+$4+$5+$6+$7+$8)-t[$1]; printf "%s %.1f%%  ", $1, dt?100*db/dt:0 } } END { print "" }' /tmp/stat0 /tmp/stat1
echo "-- processes that used cpu in the sample"
ps -eo pid,cputime,comm > /tmp/ps1
awk 'function sec(t,  p,n){ n=split(t,p,":"); return n==3?p[1]*3600+p[2]*60+p[3]:p[1]*60+p[2] }
	NR==FNR { u[$1]=sec($2); next } { d=sec($2)-u[$1]; if (d>0) printf "%6.2f s  %s %s\n", d, $1, $3 }' /tmp/ps0 /tmp/ps1 | sort -rn | head -12
echo "== done"
