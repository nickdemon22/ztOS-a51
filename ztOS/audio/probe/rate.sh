#!/bin/sh
# rate.sh: play for a moment and report how fast the DSP reads the buffer,
# as a multiple of 48 kHz stereo 16 bit (1.00 is right).
(aplay -q -D hw:0,0 --period-size=1024 --buffer-size=8192 /home/alarm/test-tone.wav >/dev/null 2>&1) &
sleep 0.1
python3 /tmp/zrn/watch.py 0x17C50000 0x1030 12 20 | awk '
	{ pos = (strtonum("0x" substr($4,5)) + strtonum("0x" substr($5,5))) * 16
	  if (NR > 1) { d = pos - last; if (d < 0) d += 32768; if (d > 0) { total += d; time += $1 - lt } }
	  last = pos; lt = $1 }
	END { if (time > 0) printf "%.2f x 48 kHz\n", total / (time / 1000) / 192000; else print "not moving" }'
wait
