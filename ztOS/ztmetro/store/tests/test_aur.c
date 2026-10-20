#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../aur.h"

int main(void) {
	/* the cases pacman's own vercmp tests check */
	struct { const char *a, *b; int want; } c[] = {
		{ "1.5.0", "1.5.0", 0 }, { "1.5.1", "1.5.0", 1 }, { "1.5.0-1", "1.5.0-2", -1 },
		{ "1.5.0-1", "1.5.1-1", -1 }, { "1.0", "1.0.0", -1 }, { "1.0a", "1.0", -1 },
		{ "1.0alpha", "1.0b", -1 }, { "1.0b", "1.0beta", -1 }, { "1.0beta", "1.0pre", -1 },
		{ "1.0pre", "1.0rc", -1 }, { "1.0rc", "1.0", -1 }, { "1.0", "1.0.a", -1 },
		{ "1:1.0", "2.0", 1 }, { "0:1.0", "1.0", 0 }, { "1.0-1", "1.0", 0 },
		{ "1.1-1", "1.1", 0 }, { "1.01", "1.1", 0 }, { "12.5.0-1", "12.4.2-1", 1 },
		{ "1.0.0", "1.0-1", 1 }, { "1..0", "1.0", 1 },
	};
	for (size_t i = 0; i < sizeof(c) / sizeof(*c); i++) {
		int got = vercmp(c[i].a, c[i].b);
		got = got < 0 ? -1 : got > 0;
		if (got != c[i].want) {
			printf("vercmp(%s, %s) = %d, want %d\n", c[i].a, c[i].b, got, c[i].want);
			return 1;
		}
		int back = vercmp(c[i].b, c[i].a);
		back = back < 0 ? -1 : back > 0;
		assert(back == -c[i].want);
	}
	const char *t = "{\"Name\":\"yay\",\"Version\":\"12.5.0-1\",\"NumVotes\":2500,\"Popularity\":40.5,"
		"\"Depends\":[\"pacman>6.1\",\"git\"],\"MakeDepends\":[\"go\"],\"License\":[\"GPL-3.0-or-later\"],\"Maintainer\":\"jguer\"}";
	struct json *j = json_parse(t, strlen(t));
	struct pkg *p = aur_pkg_from_json(j);
	assert(p && pkg_is_aur(p) && p->votes == 2500 && !strcmp(p->depends, "pacman>6.1 git"));
	assert(!strcmp(p->makedepends, "go") && !strcmp(p->packager, "jguer"));
	pkg_free(p);
	json_free(j);
	puts("aur: ok");
	return 0;
}
