# The front of every app binary, for the phone and for this computer.
TOOLCHAIN ?= ../../build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
CFLAGS = -O2 -s -Wall

all: prebuilt/ztos-launch-aarch64 prebuilt/ztos-launch-x86_64

prebuilt/ztos-launch-aarch64: launcher.c
	mkdir -p prebuilt
	$(TOOLCHAIN)/aarch64-none-linux-gnu-gcc $(CFLAGS) -o $@ $<

prebuilt/ztos-launch-x86_64: launcher.c
	mkdir -p prebuilt
	cc $(CFLAGS) -o $@ $<
