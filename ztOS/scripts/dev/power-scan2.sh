#!/bin/sh
echo "== audio"; cat /proc/asound/cards 2>&1 | head -4; for s in /proc/asound/card*/pcm*/sub*/status; do echo "$s: $(head -1 $s)"; done 2>/dev/null | head -6
echo "== usb gadget"; for u in /sys/kernel/config/usb_gadget/*/UDC; do echo "$u: $(cat $u)"; done; ls /sys/class/udc
echo "== wifi"; nmcli -t -f DEVICE,STATE,CONNECTION dev 2>&1 | head -4; iw dev wlp1s0 get power_save 2>&1; iw dev wlp1s0 link 2>&1 | head -3
cat /sys/bus/pci/devices/0000:01:00.0/power/runtime_status 2>&1; cat /sys/bus/pci/devices/0000:01:00.0/power/control 2>&1
echo "== ufs"; for f in rpm_lvl spm_lvl; do echo "$f $(cat /sys/devices/platform/soc@0/11120000.ufs/$f 2>&1)"; done
for d in /sys/devices/platform/soc@0/11120000.ufs/host0/target0:0:0/0:0:0:0/power; do echo "scsi0 control $(cat $d/control) autosuspend $(cat $d/autosuspend_delay_ms 2>&1)"; done
echo "== modem"; cat /run/zrn-modem/status | tr '\n' ' '; echo; ps -eo pid,pcpu,comm | grep -E "cbd|zrn-modemd|lhd" 
echo "== regulators from the PMIC? (any s2mps/max)"; ls /sys/class/regulator | wc -l; grep -l . /sys/class/regulator/*/name 2>/dev/null | head -0
echo "== wakeups/s (top)"; grep -E "^[ 0-9]+:" /proc/interrupts | awk '{t=0; for(i=2;i<=6;i++) t+=$i; print t, $0}' | sort -rn | head -5 | cut -c1-110
echo "== meminfo/ddr"; grep -E "MemFree|Cached" /proc/meminfo | head -2; ls /sys/class/devfreq /sys/devices/system/cpu/cpufreq
