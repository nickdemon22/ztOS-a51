#!/bin/sh
# power-scan.sh: what is powered, as the kernel knows it (read-only, as root).
echo "== devices with runtime PM, active"
for d in $(find /sys/devices -name runtime_status 2>/dev/null); do
	s=$(cat $d 2>/dev/null); dir=$(dirname $(dirname $d))
	[ "$s" = active ] && printf '%s  (%s)\n' "${dir#/sys/devices/}" "$(cat $dir/power/control 2>/dev/null)"
done | cut -c1-120 | head -70
echo "== suspended (for contrast): $(for d in $(find /sys/devices -name runtime_status 2>/dev/null); do cat $d; done | sort | uniq -c | tr '\n' ' ')"
echo "== genpd"
cat /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>&1 | head -20
echo "== i2c devices and their drivers"
for d in /sys/bus/i2c/devices/*; do printf '%s %s\n' "$(basename $d)" "$(readlink $d/driver 2>/dev/null | xargs -r basename)"; done
echo "== platform devices bound (count): $(ls /sys/bus/platform/drivers | wc -l) drivers"
echo "== regulators (all, with state)"
for r in /sys/class/regulator/regulator.*; do printf '%s %s %s\n' "$(cat $r/name)" "$(cat $r/state 2>/dev/null)" "$(cat $r/microvolts 2>/dev/null)"; done | sort | head -60
echo "== ufs"
for f in /sys/devices/platform/*ufs*/power/control /sys/devices/platform/*ufs*/power/runtime_status; do echo "$f $(cat $f 2>/dev/null)"; done
echo "== dram/bus clocks in /proc"
grep -E "MemTotal|Dirty" /proc/meminfo | head -2
