/* Run on the host: cc -O2 -Wno-unused-parameter -o /tmp/wifi-radio-test
 * ztmetro/perfd/tests/wifi-radio.c ztmetro/perfd/tickle.c && /tmp/wifi-radio-test
 * No daemon is started; shell commands are replaced by controlled responses. */
#define main perfd_program_main
#define system fake_system
#define popen fake_popen
#define pclose fake_pclose
#include "../zrnperfd.c"
#undef main
#undef system
#undef popen
#undef pclose
#include <assert.h>

static const char *radio_state = "enabled\n";
static int command_result, query_result, commands;

int fake_system(const char *command) {
	assert(!strcmp(command, "nmcli radio wifi on") ||
	       !strcmp(command, "nmcli radio wifi off"));
	commands++;
	return command_result;
}

FILE *fake_popen(const char *command, const char *mode) {
	assert(!strcmp(command, "nmcli -t -f WIFI general"));
	assert(!strcmp(mode, "r"));
	FILE *f = tmpfile();
	assert(f);
	fputs(radio_state, f);
	rewind(f);
	return f;
}

int fake_pclose(FILE *f) {
	fclose(f);
	return query_result;
}

int main(void) {
	/* A user's disabled radio must stay disabled across the sleep cycle. */
	radio_state = "disabled\n";
	wifi_sleep_at = 123;
	wifi_radio(false);
	assert(!wifi_asleep && !wifi_sleep_at && commands == 0);

	/* Failed commands and failed queries must not claim a power change. */
	radio_state = "enabled\n";
	wifi_sleep_at = 123;
	command_result = 1;
	wifi_radio(false);
	assert(!wifi_asleep && wifi_sleep_at == 123 && commands == 1);
	query_result = 1;
	wifi_radio(false);
	assert(!wifi_asleep && commands == 1);

	query_result = command_result = 0;
	wifi_radio(false);
	assert(wifi_asleep && commands == 2);
	command_result = 1;
	wifi_radio(true);
	assert(wifi_asleep && commands == 3);
	command_result = 0;
	wifi_radio(true);
	assert(!wifi_asleep && commands == 4);
	puts("Wi-Fi radio ownership and failure handling passed");
	return 0;
}
