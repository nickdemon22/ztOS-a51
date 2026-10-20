#!/bin/sh
# the shell and the toolkit apps, built and installed; the running shell kept as ztmetro.prev
cd /home/alarm/ztmetro || exit 1
tar xf /tmp/pending/ztm.tar && chown -R alarm: . && touch ztmetro.c core/*.c phone/*.c messages/*.c contacts/*.c settings/*.c
for t in ztmetro settings/ztsettings phone/ztphone messages/ztmessages contacts/ztcontacts; do
	su alarm -c "make -s $t" 2>&1 | grep -E "error|Error" -A3
done
cp -a /usr/local/bin/ztmetro /usr/local/bin/ztmetro.prev
install -m755 ztmetro /usr/local/bin/ztmetro
for b in settings/ztsettings phone/ztphone messages/ztmessages contacts/ztcontacts; do install -m755 $b /usr/local/bin/; done
sync
ls -la /usr/local/bin/ztmetro /usr/local/bin/ztsettings
echo ALL-END
