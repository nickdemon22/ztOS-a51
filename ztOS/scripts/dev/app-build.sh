#!/bin/sh
# build the three apps and Settings, put them and the icons in place
cd /home/alarm/ztmetro || exit 1
tar xf /tmp/pending/ztm.tar && chown -R alarm: . && touch core/*.c phone/*.c messages/*.c contacts/*.c settings/*.c
for t in settings/ztsettings phone/ztphone messages/ztmessages contacts/ztcontacts; do
	make -s $t 2>&1 | grep -E "error|Error" -A3
done
for b in settings/ztsettings phone/ztphone messages/ztmessages contacts/ztcontacts; do
	install -Dm755 $b /usr/local/bin/$(basename $b)
done
install -Dm644 -t /usr/local/share/applications comm/ztphone.desktop comm/ztmessages.desktop comm/ztcontacts.desktop
install -Dm644 -t /usr/local/share/icons/hicolor/scalable/apps comm/ztphone.svg comm/ztmessages.svg comm/ztcontacts.svg
rm -f /home/alarm/.cache/ztmetro/icons/ztphone.a8 /home/alarm/.cache/ztmetro/icons/ztmessages.a8 /home/alarm/.cache/ztmetro/icons/ztcontacts.a8
sync
ls -la /usr/local/bin/ztphone /usr/local/bin/ztmessages /usr/local/bin/ztcontacts /usr/local/bin/ztsettings
echo BUILD-END
