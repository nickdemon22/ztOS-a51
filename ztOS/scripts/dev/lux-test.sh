#!/bin/sh
# lux-test.sh: is the panel really lit? The light sensor sees the screen's own light.
# Run as root. Prints lux with the screen off, then on, then off again.
CTL=/run/user/1000/ztmetro.ctl
rd() { awk '{print $1}' /run/zrn-sensors/light; }
printf 'auto off\n' > /run/zrn-sensord.ctl; printf 'brightness 100\n' > $CTL
printf 'screen off\n' > $CTL; sleep 6; echo "off:  $(rd)"
printf 'screen on\n' > $CTL; sleep 6; echo "on:   $(rd)"
printf 'screen on\n' > $CTL; sleep 3; echo "on:   $(rd)"
printf 'screen off\n' > $CTL; sleep 6; echo "off:  $(rd)"
printf 'auto on\n' > /run/zrn-sensord.ctl
