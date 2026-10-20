#!/bin/bash
# apply-pending.sh: put everything written while the phone was away onto it.
# Run on the laptop with the phone on USB (172.16.42.1): the ztMETRO sources
# are pushed and built there, the new boot-time scripts and units installed,
# the vibration and idle modules copied into place. Nothing is restarted and
# nothing risky is switched on: the shell and daemons pick the new builds up
# at the next reboot, and idle power-down stays off until `zrn-cpuidle try`.
set -e
cd "$(dirname "$0")/.."
P=172.16.42.1
# the alarm user's password comes from the environment (ZT_PW), never from this file
ssh() { python3 scripts/ssh-pw.py alarm "${ZT_PW:-alarm},root" $P "$1" < /dev/null; }
ping -c1 -W2 $P > /dev/null || { echo "the phone is not on USB"; exit 1; }

tar cf build/ztm.tar -C ztmetro --exclude=ref .
tar cf build/overlay.tar -C rootfs/overlay usr/local/sbin/zboot-charge-hold usr/local/sbin/zrn-cpuidle \
	etc/systemd/system/zboot-charge-hold.service etc/systemd/system/zrn-cpuidle.service \
	etc/tmpfiles.d/99-ztos-compat.conf etc/sudoers.d/99-ztos-alarm
cp haptics/zrn-haptic.ko power/idle/zrn-cpuidle.ko build/
tar rf build/overlay.tar -C build zrn-haptic.ko zrn-cpuidle.ko
for f in ztm overlay; do
	setsid -f timeout 40 socat -u OPEN:build/$f.tar TCP-LISTEN:5581,bind=172.16.42.2,reuseaddr > /dev/null 2>&1 < /dev/null
	sleep 1
	ssh "mkdir -p /tmp/pending && cat < /dev/tcp/172.16.42.2/5581 > /tmp/pending/$f.tar" > /dev/null 2>&1
done
ssh 'su -c "date -s @'$(date +%s)' > /dev/null
cd /home/alarm/ztmetro && tar xf /tmp/pending/ztm.tar && chown -R alarm: . && touch *.c */*.c
mkdir -p /tmp/pending/o && tar xf /tmp/pending/overlay.tar -C /tmp/pending/o
install -m755 /tmp/pending/o/usr/local/sbin/* /usr/local/sbin/
install -m644 /tmp/pending/o/etc/systemd/system/*.service /etc/systemd/system/
[ -d /tmp/pending/o/etc/tmpfiles.d ] && install -m644 /tmp/pending/o/etc/tmpfiles.d/* /etc/tmpfiles.d/ && systemd-tmpfiles --create /etc/tmpfiles.d/99-ztos-compat.conf 2>/dev/null || true
[ -d /tmp/pending/o/etc/sudoers.d ] && install -m440 /tmp/pending/o/etc/sudoers.d/* /etc/sudoers.d/ || true
install -Dm644 -t /usr/lib/modules/\$(uname -r)/extra /tmp/pending/o/zrn-haptic.ko /tmp/pending/o/zrn-cpuidle.ko
depmod -a 2>/dev/null
systemctl daemon-reload
systemctl enable zboot-charge-hold.service zrn-cpuidle.service 2>&1 | tail -2
setsid -f sh -c \"(make -s 2>&1 | grep -E \\\"error|Error\\\" -A3; make -s install 2>&1 | grep -E \\\"error|Error\\\"; sync; echo BUILD-END) > /tmp/pending/build.log 2>&1\" > /dev/null 2>&1 < /dev/null"' | tail -3
echo "building on the phone: /tmp/pending/build.log ends with BUILD-END when done; then reboot it."
