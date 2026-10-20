#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../repo.h"

int main(void) {
	struct repo_index ri;
	assert(repo_load(&ri, "/tmp/ztstore-fixture/pacman.conf"));
	assert(ri.repos.n == 2 && !strcmp(ri.repos.v[0], "core"));
	assert(ri.sync.n == 4);	/* the second firefox dropped */
	struct pkg *f = repo_find_sync(&ri, "firefox");
	assert(f && !strcmp(f->repo, "core"));	/* first in pacman.conf wins */
	struct pkg *m = repo_find_sync(&ri, "mpv");
	assert(m && !strcmp(m->version, "1:0.40.0-1") && m->csize == 2048000 && m->builddate == 1750000000);
	assert(!strcmp(m->license, "MPL-2.0 GPL-2.0") && !strcmp(m->depends, "gtk3 nss"));
	assert(!strcmp(repo_find_sync(&ri, "xfce4-panel")->groups, "xfce4"));
	assert(f->installed && !strcmp(f->installed_version, "139.0-1") && f->explicit_install);
	assert(f->desktop && !strcmp(f->desktop, "/usr/share/applications/firefox.desktop"));
	struct vec foreign = {0};
	repo_foreign(&ri, &foreign);
	assert(foreign.n == 1 && !strcmp(((struct pkg *)foreign.v[0])->name, "yay"));
	vec_free(&foreign, NULL);
	repo_free(&ri);
	puts("repo: ok");
	return 0;
}
