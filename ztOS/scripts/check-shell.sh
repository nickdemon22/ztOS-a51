#!/bin/sh
# Syntax-check ztmetro.c on the laptop, which has no wlroots: its headers are
# taken from the Arch package into a scratch directory given as $1 (the
# package unpacked: $1/x/usr/include/wlroots-0.20, and $1/gen for the
# generated xdg-shell header).
W=${1:?where the wlroots package was unpacked}
cd "$(dirname "$0")/../ztmetro" || exit 1
gcc -fsyntax-only -std=c11 -Wall -Wno-unused-parameter -DWLR_USE_UNSTABLE -I"$W/gen" \
	-I"$W/x/usr/include/wlroots-0.20" \
	$(pkg-config --cflags wayland-server xkbcommon cairo pangocairo librsvg-2.0 pixman-1 libdrm egl glesv2) \
	ztmetro.c 2>&1 | grep -E "error|warning" | head -30
echo "checked"
