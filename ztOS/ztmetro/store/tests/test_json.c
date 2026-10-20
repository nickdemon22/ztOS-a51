#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "../json.h"

static int seen;
static double votes;
static void each(const struct json *j, void *d) { seen++; votes += json_num(j, "NumVotes"); }

int main(void) {
	const char *t = "{\"resultcount\":2,\"results\":[{\"Name\":\"yay\",\"NumVotes\":2500,\"Description\":\"a \\\"q\\\" \\u00e9 \\ud83d\\ude00\",\"Keywords\":[]},{\"Name\":\"paru\",\"OutOfDate\":null,\"x\":true}]}";
	struct json *j = json_parse(t, strlen(t));
	assert(j && json_num(j, "resultcount") == 2);
	struct json *r = json_get(j, "results");
	assert(r && r->type == J_ARR && r->n == 2);
	assert(!strcmp(json_str(r->items[0], "Name"), "yay"));
	assert(!strcmp(json_str(r->items[0], "Description"), "a \"q\" \xc3\xa9 \xf0\x9f\x98\x80"));
	assert(json_num(r->items[1], "x") == 1 && json_get(r->items[1], "OutOfDate")->type == J_NULL);
	json_free(j);
	assert(!json_parse("{\"a\":}", 6) && !json_parse("[1,2", 4) && !json_parse("{} x", 4));
	gzFile gz = gzopen("/tmp/dump.json.gz", "wb");
	gzputs(gz, "[\n{\"Name\":\"a]{\",\"NumVotes\":3},{\"Name\":\"b\",\"NumVotes\":4,\"D\":[1,{\"x\":\"}\"}]}\n]");
	gzclose(gz);
	assert(json_each_gz("/tmp/dump.json.gz", each, NULL) == 2 && seen == 2 && votes == 7);
	puts("json: ok");
	return 0;
}
