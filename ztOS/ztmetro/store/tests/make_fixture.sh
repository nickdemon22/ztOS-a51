#!/bin/sh
# A tiny pacman layout: two repos (firefox in both), two installed packages.
set -e
R=${1:-/tmp/ztstore-fixture}
rm -rf "$R"; mkdir -p "$R/db/sync" "$R/db/local" "$R/src"
pkg() { # repo name version desc builddate [groups]
	d="$R/src/$1/$2-$3"; mkdir -p "$d"
	{ printf '%%FILENAME%%\n%s-%s-aarch64.pkg.tar.xz\n\n%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n' "$2" "$3" "$2" "$3"
	  printf '%%DESC%%\n%s\n\n%%CSIZE%%\n2048000\n\n%%ISIZE%%\n8192000\n\n%%ARCH%%\naarch64\n\n%%BUILDDATE%%\n%s\n\n' "$4" "$5"
	  [ -n "${6:-}" ] && printf '%%GROUPS%%\n%s\n\n' "$6"
	  printf '%%LICENSE%%\nMPL-2.0\nGPL-2.0\n\n%%DEPENDS%%\ngtk3\nnss\n\n'; } > "$d/desc"
}
pkg core glibc 2.41-1 "GNU C Library" 1700000000
pkg core firefox 1.0-1 "old copy, should lose" 1600000000
pkg extra firefox 140.0-1 "Fast, Private & Safe Web Browser" 1760000000
pkg extra mpv 1:0.40.0-1 "a free, open source, and cross-platform media player" 1750000000
pkg extra xfce4-panel 4.20-1 "Panel for Xfce" 1740000000 xfce4
for r in core extra; do (cd "$R/src/$r" && tar -czf "$R/db/sync/$r.db" *); done
mkdir -p "$R/db/local/firefox-140.0-1" "$R/db/local/yay-12.5.0-1"
printf '%%NAME%%\nfirefox\n\n%%VERSION%%\n139.0-1\n\n%%DESC%%\nbrowser\n\n%%REASON%%\n\n' > "$R/db/local/firefox-140.0-1/desc"
printf '%%FILES%%\nusr/\nusr/lib/firefox/firefox\nusr/share/applications/\nusr/share/applications/firefox.desktop\n\n' > "$R/db/local/firefox-140.0-1/files"
printf '%%NAME%%\nyay\n\n%%VERSION%%\n12.5.0-1\n\n%%DESC%%\nAUR helper\n\n' > "$R/db/local/yay-12.5.0-1/desc"
cat > "$R/pacman.conf" <<CONF
[options]
DBPath = $R/db/   # a comment
Architecture = auto
[core]
Include = /etc/pacman.d/mirrorlist
[extra]
Include = /etc/pacman.d/mirrorlist
CONF
