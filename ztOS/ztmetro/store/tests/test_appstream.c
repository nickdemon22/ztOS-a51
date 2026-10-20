#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "../appstream.h"

int main(void) {
	const char *root = "/tmp/ztstore-fixture/as";
	mkdir(root, 0755);
	mkdir("/tmp/ztstore-fixture/as/xml", 0755);
	mkdir("/tmp/ztstore-fixture/as/icons", 0755);
	mkdir("/tmp/ztstore-fixture/as/icons/archlinux-arch-extra", 0755);
	mkdir("/tmp/ztstore-fixture/as/icons/archlinux-arch-extra/128x128", 0755);
	FILE *f = fopen("/tmp/ztstore-fixture/as/icons/archlinux-arch-extra/128x128/mpv_mpv.png", "w");
	fputs("png", f);
	fclose(f);
	gzFile gz = gzopen("/tmp/ztstore-fixture/as/xml/archlinux-arch-extra.xml.gz", "wb");
	gzputs(gz,
		"<?xml version=\"1.0\"?>\n<components version=\"0.16\" origin=\"archlinux-arch-extra\">\n"
		"<component type=\"desktop-application\">\n  <id>io.mpv.Mpv</id>\n  <pkgname>mpv</pkgname>\n"
		"  <name xml:lang=\"de\">mpv-Medien</name>\n  <name>mpv Media Player</name>\n"
		"  <summary>Watch   your\n videos &amp; more</summary>\n"
		"  <icon type=\"stock\">mpv</icon>\n  <icon type=\"cached\" width=\"64\" height=\"64\">mpv_mpv.png</icon>\n"
		"  <icon type=\"cached\" width=\"128\" height=\"128\">mpv_mpv.png</icon>\n"
		"  <categories>\n    <category>AudioVideo</category>\n    <category>Video</category>\n  </categories>\n"
		"</component>\n<component type=\"font\"><pkgname>glibc</pkgname><name>nope</name></component>\n"
		"<component type=\"desktop-application\"><pkgname>firefox</pkgname><name>Firefox</name>"
		"<summary>Browse</summary><categories><category>Network</category></categories></component>\n</components>\n");
	gzclose(gz);
	struct repo_index ri;
	assert(repo_load(&ri, "/tmp/ztstore-fixture/pacman.conf"));
	const char *roots[] = { root, NULL };
	assert(appstream_apply(&ri, roots) == 2);
	struct pkg *m = repo_find_sync(&ri, "mpv");
	assert(!strcmp(m->app_name, "mpv Media Player"));
	assert(!strcmp(m->summary, "Watch your videos & more"));
	assert(m->icon && strstr(m->icon, "128x128/mpv_mpv.png"));
	assert(!strcmp(m->categories, "AudioVideo Video"));
	assert(!repo_find_sync(&ri, "glibc")->app_name);
	assert(!strcmp(repo_find_local(&ri, "firefox")->app_name, "Firefox"));
	repo_free(&ri);
	puts("appstream: ok");
	return 0;
}
