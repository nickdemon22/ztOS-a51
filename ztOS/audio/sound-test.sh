#!/bin/sh
# sound-test.sh: two sounds, to tell where sound is lost.
#   1. a chord (440 + 660 Hz, 2 s) played through PipeWire, the DSP and the amplifiers
#   2. tones made inside the amplifiers themselves (no DSP data involved), six
#      settings of their tone generator, one second each
# Hearing 2 but not 1: the amplifiers and speakers work, the data path does not.
# Hearing neither: the amplifier set-up or its power is at fault.
export XDG_RUNTIME_DIR=/run/user/$(id -u)
w() { for a in 0x39 0x3a; do sudo i2ctransfer -y 98 w3@$a 0x00 "$1" "$2"; done; }
r() { sudo i2ctransfer -y 98 w2@0x39 0x00 "$1" r1; }
boost() { w 0x86 0x05; w 0x87 0x02; w 0x88 0x01; w 0x80 0x0a; w 0x84 0x01; w 0x3a 0x03; }
wpctl set-volume @DEFAULT_AUDIO_SINK@ 100%
echo "1: chord through the whole path"
(pw-play ~/test-tone.wav; pw-play ~/test-tone.wav) &
sleep 0.7; boost
echo "   amp: global=$(sudo i2ctransfer -y 98 w2@0x39 0x04 0x00 r1) amp=$(r 0x38) raw=$(r 0x01) $(r 0x02) $(r 0x03) bypass=$(r 0x87)"
wait
sleep 1
echo "2: the amplifiers' own tone generator"
(for i in 1 2 3 4 5; do pw-play ~/test-tone.wav; done) &	# only to keep the clocks running
sleep 0.7; boost
w 0x39 0x02		# speaker source: the tone generator
for t in 0x01 0x02 0x03 0x04 0x05 0x06; do
	w 0x37 $t; echo "   tone setting $t (source=$(r 0x39))"; sleep 1
done
w 0x39 0x00; w 0x37 0x00
wait
echo done
