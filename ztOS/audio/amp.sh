#!/bin/sh
# audio/amp.sh on|off|status [BUS]: the two MAX98512 speaker amplifiers
# (0x39 and 0x3a) on the bit-banged bus zrn_amp_i2c provides (98).
#
# "on" is deliberately quiet. On the stock system the DSP runs Maxim's speaker
# protection (DSM) against the current and voltage these chips report back;
# none of that is running here, so the gain is low (digital -8 dB, analogue
# the lowest step), the boost converter is left at its lowest voltage and
# the brown-out and current limits are Samsung's. Do not raise the volume
# registers without protection in place.
#
# Registers and values are from Samsung's driver
# (third_party/vendor-kernel/sound/soc/codecs/max98512.c, GPL).
BUS=${2:-98}
w() { i2ctransfer -y "$BUS" w3@"$1" "$(printf '0x%02x' $(( $2 >> 8 )))" "$(printf '0x%02x' $(( $2 & 255 )))" "$3" || echo "write $1 $2 failed"; }
r() { i2ctransfer -y "$BUS" w2@"$1" "$(printf '0x%02x' $(( $2 >> 8 )))" "$(printf '0x%02x' $(( $2 & 255 )))" r1; }
both() { w 0x39 "$1" "$2"; w 0x3a "$1" "$2"; }

case "$1" in
on)
	both 0x0401 0x01		# software reset
	sleep 0.05
	both 0x003c 0x00		# no current/voltage measurement: nothing reads it
	both 0x001c 0xff; both 0x001d 0xff	# their own data output: high impedance
	both 0x0020 0x40		# I2S, 16 bits a channel
	both 0x0021 0x00		# clocks come from the SoC
	both 0x0022 0x02		# 32 bit clocks a frame
	both 0x0023 0x08		# 48 kHz
	both 0x0024 0x88
	w 0x39 0x0018 0x01; w 0x39 0x0025 0x00; w 0x39 0x0026 0x00	# left: channel 0
	w 0x3a 0x0018 0x02; w 0x3a 0x0025 0x41; w 0x3a 0x0026 0x01	# right: channel 1
	both 0x0035 0x20		# digital volume: -8 dB (0x40 is 0 dB)
	both 0x003a 0x01		# speaker gain: lowest step
	both 0x0036 0x01		# DC blocker
	both 0x003e 0x00		# boost: lowest output voltage
	both 0x0040 0x3e; both 0x003f 0x01
	both 0x004e 0x10		# squelch
	both 0x0050 0x00		# brown-out limiting off: half configured, it silenced the output
	both 0x0011 ${CMON:-0x00}	# clock monitor (Samsung leaves it off)
	both 0x0038 0x81		# amplifier on
	both 0x0400 0x01		# out of shutdown
	sleep 0.2
	sh "$0" status "$BUS"
	;;
off)
	both 0x0400 0x00
	both 0x0038 0x80
	;;
status)
	for a in 0x39 0x3a; do
		echo "$a: global=$(r $a 0x0400) amp=$(r $a 0x0038) raw=$(r $a 0x0001) $(r $a 0x0002) $(r $a 0x0003) state=$(r $a 0x0004) $(r $a 0x0005) $(r $a 0x0006) vol=$(r $a 0x0035) gain=$(r $a 0x003a) brownout=$(r $a 0x004f)"
	done
	;;
*)
	echo "usage: $0 on|off|status [BUS]"; exit 2
	;;
esac
