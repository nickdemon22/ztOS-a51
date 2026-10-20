#!/bin/sh
# one minute of per-process CPU, each second, with perfd's boost state
out=/tmp/zrn/cpusample.log; : > $out
for i in $(seq 60); do
	t=$(date +%T)
	top -b -n 1 -d 0.5 -w 200 | awk -v t=$t '/^ *[0-9]+ /{ if ($9 > 15) print t, $9"%", $12 }' >> $out
	journalctl -u zrnperfd --since "-1s" --no-pager -q 2>/dev/null | grep -o "boost [a-z]*" | sed "s/^/$t /" >> $out
	sleep 0.5
done
echo DONE >> $out
