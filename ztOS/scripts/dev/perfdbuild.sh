#!/bin/sh
cd /home/alarm/ztmetro || exit 1
cp /tmp/zrn/zrnperfd.c perfd/zrnperfd.c && chown alarm: perfd/zrnperfd.c && touch perfd/zrnperfd.c
su alarm -c "make -s perfd/zrnperfd" 2>&1 | grep -E "error|Error" -A3
dest=$(systemctl show -p ExecStart zrnperfd | sed -n 's/.*path=\([^ ;]*\).*/\1/p')
echo "service runs $dest"
if [ -x perfd/zrnperfd ] && [ -n "$dest" ]; then
	cp -a "$dest" "$dest.prev"; install -m755 perfd/zrnperfd "$dest"; systemctl restart zrnperfd; sleep 1
	systemctl is-active zrnperfd
	printf 'policy 1 eco\n' > /run/zrnperfd.ctl; sleep 0.3
	journalctl -u zrnperfd -n 3 --no-pager -q | tail -2
fi
echo PERFD-END
