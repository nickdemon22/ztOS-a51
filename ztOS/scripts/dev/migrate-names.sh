#!/bin/sh
# migrate-names.sh: move the phone's running system to ztOS's component names
# (run as root on the phone, after the renamed builds are installed:
# ztmetro's `make install` and the rootfs overlay).
#
#   zrn-audio        -> ztoscirrusaudio              (ztOSCirrusAudio)
#   zrn-sensorhub    -> ztoscsf-sensorhub            (ztOSCoreSensorFramework)
#   zrnsensord       -> ztoscsf-sensord              (ztOSCoreSensorFramework)
#   zrn-modem        -> ztoscorenetworking-modem     (ztOSCoreNetworking)
#   zrn-modemd       -> ztoscorenetworking[d]        (ztOSCoreNetworking)
#   zboot-wifi       -> ztoscorenetworking-wifi      (ztOSCoreNetworking)
#
# The modem and sound are restarted on the way (a call in progress drops).
# Messages, calls and the access point setting move with the networking daemon
# (it moves /var/lib/zrn-modem and /etc/zrn-modem itself on its first start).
set -eu
[ "$(id -u)" = 0 ] || { echo "as root" >&2; exit 1; }
for new in ztoscirrusaudio ztoscsf-sensorhub ztoscsf-sensord ztoscorenetworking-modem ztoscorenetworking; do
	[ -f "/etc/systemd/system/$new.service" ] || { echo "$new.service is not installed yet" >&2; exit 1; }
done
pairs="zrn-audio:ztoscirrusaudio zrn-sensorhub:ztoscsf-sensorhub zrnsensord:ztoscsf-sensord zrn-modemd:ztoscorenetworking zrn-modem:ztoscorenetworking-modem"
for p in $pairs; do
	old=${p%%:*}
	systemctl disable --now "$old.service" 2>/dev/null || true
done
for p in $pairs; do
	new=${p#*:}
	systemctl enable "$new.service"
done
systemctl daemon-reload
for p in $pairs; do
	old=${p%%:*}
	rm -f "/etc/systemd/system/$old.service" "/usr/local/sbin/$old"
done
rm -f /usr/local/sbin/zboot-wifi
# the old load-in-progress marks (each new service keeps its own now)
rm -rf /var/lib/zrn-audio /var/lib/zrn-sensorhub
[ -d /var/lib/zrn-sensord ] && [ ! -d /var/lib/ztoscsf-sensord ] && mv /var/lib/zrn-sensord /var/lib/ztoscsf-sensord
systemctl start ztoscorenetworking-modem.service ztoscorenetworking.service ztoscirrusaudio.service \
	ztoscsf-sensorhub.service ztoscsf-sensord.service
systemctl restart ztoscoresensorframework.service
echo "done: $(systemctl is-active ztoscorenetworking.service) networking, $(systemctl is-active ztoscirrusaudio.service) audio"
