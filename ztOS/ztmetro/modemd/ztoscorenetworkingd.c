// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztoscorenetworkingd: the modem's state for the shell, and the few things asked of it.
 *
 * The S9's modem answers AT commands on /dev/umts_router once the vendor's cbd
 * has started it (ztoscorenetworking-modem). This daemon owns that port, keeps the command
 * and file channels (umts_ipc0, umts_rfs0) open as the modem expects of a
 * radio daemon, and:
 *
 *  - asks every PERIOD_S, and listens for the modem's own reports of,
 *    the SIM, the operator, the signal, the registration and its technology;
 *  - writes them to /run/ztoscorenetworking/status (key=value lines), and tells the
 *    shell ("modem" on its control pipe) whenever any of it changes;
 *  - also notes the Wi-Fi link's strength (from `iw`, so that the shell
 *    itself never waits on a program);
 *  - takes, on /run/ztoscorenetworking.ctl (anyone may write):
 *	airplane on|off		the radio off (AT+CFUN=4) or on (AT+CFUN=1)
 *	data on|off		whether mobile data is wanted. Kept, and shown by
 *				the shell, but no connection is made yet: see
 *				"packet data" below
 *	sms NUMBER TEXT		one message, in PDU mode (text mode is refused
 *				by this modem)
 *
 * Nothing is written to EFS: the modem's file channel is drained, unanswered.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AT_DEV		"/dev/umts_router"
#define CTL		"/run/ztoscorenetworking.ctl"
#define STATUS_DIR	"/run/ztoscorenetworking"
#define STATUS		STATUS_DIR "/status"
#define PERIOD_S	15
#define AT_TIMEOUT_MS	12000

static int at_fd = -1, ipc_fd = -1, rfs_fd = -1, ctl_fd = -1;

/* ---- what is known ------------------------------------------------------- */

struct state {
	char sim[16];		/* ready, absent, pin, puk, error, ? */
	char operator[48];
	int bars;		/* 0..5, -1 unknown */
	int cs_reg, ps_reg, eps_reg;	/* 3GPP stat: 1 home, 5 roaming, 2 searching, 3 denied, 0 none */
	int cs_act, ps_act, eps_act;	/* 3GPP access technology, -1 none */
	bool airplane;
	bool data;		/* a packet context is up, with an address */
	bool data_on;		/* and whether the user wants one */
	char ip[48];
	int wifi_bars;		/* -1 no link, 0..3 */
	char call[12];		/* "", dialing, ringing (theirs), incoming, active */
	char call_number[32];
};
static struct state now = { "?", "", -1, 0, 0, 0, -1, -1, -1, false, false, false, "", -1 };
static struct state written;
static bool written_once;

static const char *reg_name(int reg)
{
	return reg == 1 ? "home" : reg == 5 ? "roaming" : reg == 2 ? "searching" :
		reg == 3 ? "denied" : "none";
}

/* The data icon: what packet service there is, by its technology. */
static const char *tech(void)
{
	if (now.eps_reg == 1 || now.eps_reg == 5)
		return "4G";
	if (now.ps_reg == 1 || now.ps_reg == 5) {
		switch (now.ps_act) {
		case 7: return "4G";
		case 6: return "H+";
		case 4: case 5: return "H";
		case 2: return "3G";
		case 3: return "E";
		case 0: case 1: return "G";
		}
		return "G";
	}
	return "";
}

static void tell_shell(const char *line)
{
	/* the shell's control pipe, wherever its session is */
	char path[64];
	for (int uid = 1000; uid < 1010; uid++) {
		snprintf(path, sizeof(path), "/run/user/%d/ztmetro.ctl", uid);
		int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			if (write(fd, line, strlen(line)) < 0) {
				/* it reads the file at its next redraw anyway */
			}
			close(fd);
		}
	}
}

static void publish(void)
{
	if (written_once && memcmp(&now, &written, sizeof(now)) == 0)
		return;
	mkdir(STATUS_DIR, 0755);
	FILE *f = fopen(STATUS ".new", "w");
	if (!f)
		return;
	int reg = now.eps_reg == 1 || now.eps_reg == 5 ? now.eps_reg :
		now.cs_reg ? now.cs_reg : now.ps_reg;
	int bars = now.bars;
	if (bars <= 0 && (reg == 1 || reg == 5))
		bars = 3;
	fprintf(f, "call=%s\ncall_number=%s\n", now.call[0] ? now.call : "idle", now.call_number);
	fprintf(f, "sim=%s\noperator=%s\nbars=%d\nregistered=%s\ntech=%s\nairplane=%d\ndata=%d\ndata_on=%d\n"
		"ip=%s\nwifi_bars=%d\n",
		now.sim, now.operator, bars, reg_name(reg), tech(),
		/* (this modem's AT+CFUN? has been seen to say "off" while it is
		 * registered: a registered radio is not in airplane mode) */
		now.airplane && reg != 1 && reg != 5, now.data, now.data_on,
		now.ip, now.wifi_bars);
	fclose(f);
	chmod(STATUS ".new", 0644);
	rename(STATUS ".new", STATUS);
	written = now;
	written_once = true;
	tell_shell("modem\n");
}

/* ---- the AT port: one command at a time, a queue behind it --------------- */

#define MAX_QUEUE	32
static char queue[MAX_QUEUE][600];
static int q_head, q_len;
static bool busy;		/* a command is out, its final result not yet back */
static char sent[600];
static long sent_at;
static char pdu_pending[512];	/* sent after the '>' prompt of AT+CMGS */
static char line[2048];
static size_t line_len;

static long ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void queue_at(const char *fmt, ...)
{
	if (q_len == MAX_QUEUE)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(queue[(q_head + q_len) % MAX_QUEUE], sizeof(queue[0]), fmt, ap);
	va_end(ap);
	q_len++;
}

static void next_at(void)
{
	if (busy || !q_len || at_fd < 0)
		return;
	snprintf(sent, sizeof(sent), "%s", queue[q_head]);
	q_head = (q_head + 1) % MAX_QUEUE;
	q_len--;
	char out[620];
	int n = snprintf(out, sizeof(out), "%s\r", sent);
	if (write(at_fd, out, n) != n) {
		fprintf(stderr, "ztoscorenetworkingd: write %s: %s\n", sent, strerror(errno));
		return;
	}
	busy = true;
	sent_at = ms();
}

/* ---- what the modem says ------------------------------------------------- */

/* +CREG / +CGREG / +CEREG: as an answer "n,stat[,lac,ci[,act]]", as a report
 * "stat[,lac,ci[,act]]" */
static void parse_reg(const char *p, bool answer, int *reg, int *act)
{
	int v[5], n = 0;
	char copy[160];
	snprintf(copy, sizeof(copy), "%s", p);
	for (char *tok = strtok(copy, ","); tok && n < 5; tok = strtok(NULL, ",")) {
		while (*tok == ' ' || *tok == '"')
			tok++;
		int base = answer ? (n == 2 || n == 3 ? 16 : 10) : (n == 1 || n == 2 ? 16 : 10);
		v[n] = (int)strtol(tok, NULL, base);	/* (the area and cell are hex) */
		n++;
	}
	int first = answer && n >= 2 ? 1 : 0;
	if (n <= first)
		return;
	*reg = v[first];
	*act = n > first + 3 ? v[first + 3] : (*reg == 1 || *reg == 5) ? *act : -1;
}

static int csq_bars(int rssi)
{
	if (rssi == 99 || rssi < 0)
		return 0;
	return rssi >= 20 ? 5 : rssi >= 15 ? 4 : rssi >= 10 ? 3 : rssi >= 6 ? 2 : rssi >= 2 ? 1 : 0;
}

static void data_deactivate(void);
static long data_tried_at;
static char data_dns[2][20];
/* The context in the modem, apart from whether it is in use (now.data): on
 * LTE it is the default bearer, and taking it down takes the phone off LTE
 * (seen: down to 2G, and no context to be had after). So switching data off
 * only stops using it. */
static bool ctx_up;
static char ctx_ip[48];

static bool airplane_asked;	/* "airplane on" came down the control pipe */
static bool screen_off;		/* asked less often then: every wake-up costs battery */
#define PERIOD_OFF_S	120

/* ---- messages in ----------------------------------------------------------
 *
 * The modem keeps a new message on the SIM and says where (+CMTI). It is read
 * in PDU mode (AT+CMGR; text mode is refused by this modem), put into
 * SMS_LOG for the messaging app, shown as a notification, and deleted from
 * the SIM, which holds few. Whatever is on the SIM at start is taken the
 * same way (AT+CMGL=4).
 *
 * SMS_LOG, one line each, tab separated, readable by all:
 *	in   <unix time>  <number>  <text, with \n for a line end>
 *	out  <unix time>  <number>  ok | failed	(what came of a "sms" command)
 */
#define SMS_DIR		"/var/lib/ztoscorenetworking/sms"
#define SMS_LOG		SMS_DIR "/log"
#define CALL_LOG	"/var/lib/ztoscorenetworking/calls"

static int sms_index = -1;	/* the message whose PDU is the next line */
static char sms_out_number[32];
static void tell_shell(const char *line);
static void queue_at(const char *fmt, ...);

static void log_line(const char *path, const char *fmt, ...)
{
	va_list ap;
	mkdir(SMS_DIR, 0755);
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
	chmod(path, 0644);
}

/* The GSM default alphabet, as UTF-8. */
static const char *const GSM7[128] = {
	"@", "\xc2\xa3", "$", "\xc2\xa5", "\xc3\xa8", "\xc3\xa9", "\xc3\xb9", "\xc3\xac",
	"\xc3\xb2", "\xc3\x87", "\n", "\xc3\x98", "\xc3\xb8", "\r", "\xc3\x85", "\xc3\xa5",
	"\xce\x94", "_", "\xce\xa6", "\xce\x93", "\xce\x9b", "\xce\xa9", "\xce\xa0", "\xce\xa8",
	"\xce\xa3", "\xce\x98", "\xce\x9e", "", "\xc3\x86", "\xc3\xa6", "\xc3\x9f", "\xc3\x89",
	" ", "!", "\"", "#", "\xc2\xa4", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
	"\xc2\xa1", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
	"P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "\xc3\x84", "\xc3\x96", "\xc3\x91", "\xc3\x9c", "\xc2\xa7",
	"\xc2\xbf", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
	"p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "\xc3\xa4", "\xc3\xb6", "\xc3\xb1", "\xc3\xbc", "\xc3\xa0",
};

static int hex_byte(const char *h)
{
	unsigned v;
	return sscanf(h, "%2x", &v) == 1 ? (int)v : -1;
}

/* An SMS-DELIVER as the modem gives it (hex, the SMS centre first): who it is
 * from and what it says. Seven-bit and UCS-2 texts; of a message in several
 * parts each part comes as a message of its own. */
static bool sms_decode(const char *hex, char *number, size_t number_size, char *text, size_t text_size)
{
	unsigned char b[400];
	size_t n = 0;
	for (; hex[0] && hex[1] && n < sizeof(b); hex += 2) {
		int v = hex_byte(hex);
		if (v < 0)
			break;
		b[n++] = (unsigned char)v;
	}
	size_t at = 0;
	if (n < 12)
		return false;
	at += 1 + b[0];				/* the SMS centre */
	if (at >= n || (b[at] & 3) != 0)
		return false;			/* not a delivery */
	bool header = b[at++] & 0x40;
	size_t digits = b[at++], type = b[at++], bytes = (digits + 1) / 2;
	if (at + bytes + 10 > n)
		return false;
	size_t o = 0;
	if ((type & 0x70) == 0x50) {
		/* a name in the seven-bit alphabet instead of a number */
		unsigned bits = 0, have = 0;
		for (size_t i = 0; i < bytes && o + 4 < number_size; i++) {
			bits |= (unsigned)b[at + i] << have;
			have += 8;
			while (have >= 7 && o + 4 < number_size) {
				const char *c = GSM7[bits & 0x7f];
				if ((bits & 0x7f) && c[0] >= ' ')
					o += (size_t)snprintf(number + o, number_size - o, "%s", c);
				bits >>= 7;
				have -= 7;
			}
		}
	} else {
		if ((type & 0x70) == 0x10 && o + 1 < number_size)
			number[o++] = '+';
		for (size_t i = 0; i < digits && o + 1 < number_size; i++) {
			int d = i % 2 ? b[at + i / 2] >> 4 : b[at + i / 2] & 0xf;
			if (d <= 9)
				number[o++] = (char)('0' + d);
		}
	}
	number[o] = 0;
	at += bytes;
	unsigned dcs = b[at + 1];
	at += 2 + 7;				/* protocol, coding, the centre's time stamp */
	size_t udl = b[at++], skip = 0;
	if (header && at < n)
		skip = 1 + b[at];		/* a header: for a message in parts, mostly */
	o = 0;
	text[0] = 0;
	if ((dcs & 0x0c) == 0x08) {
		/* UCS-2, big-endian */
		for (size_t i = at + skip; i + 1 < n && i < at + udl && o + 4 < text_size; i += 2) {
			unsigned c = (unsigned)b[i] << 8 | b[i + 1];
			if (c < 0x80)
				text[o++] = (char)c;
			else if (c < 0x800) {
				text[o++] = (char)(0xc0 | c >> 6);
				text[o++] = (char)(0x80 | (c & 0x3f));
			} else {
				text[o++] = (char)(0xe0 | c >> 12);
				text[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
				text[o++] = (char)(0x80 | (c & 0x3f));
			}
		}
	} else if ((dcs & 0x0c) == 0x00) {
		/* seven bits each, packed; after a header, from the next septet */
		size_t first = (skip * 8 + 6) / 7;
		bool escape = false;
		for (size_t k = first; k < udl && o + 4 < text_size; k++) {
			size_t bit = k * 7, byte = at + bit / 8;
			if (byte >= n)
				break;
			unsigned v = b[byte] >> (bit % 8);
			if (bit % 8 > 1 && byte + 1 < n)
				v |= (unsigned)b[byte + 1] << (8 - bit % 8);
			v &= 0x7f;
			if (escape) {
				/* the extension table: the few of it that matter */
				const char *e = v == 0x65 ? "\xe2\x82\xac" : v == 0x28 ? "{" : v == 0x29 ? "}" :
					v == 0x3c ? "[" : v == 0x3e ? "]" : v == 0x2f ? "\\" : v == 0x14 ? "^" :
					v == 0x3d ? "~" : v == 0x40 ? "|" : "";
				o += (size_t)snprintf(text + o, text_size - o, "%s", e);
				escape = false;
			} else if (v == 0x1b) {
				escape = true;
			} else {
				o += (size_t)snprintf(text + o, text_size - o, "%s", GSM7[v]);
			}
		}
	} else {
		snprintf(text, text_size, "(a message that is not text)");
		o = strlen(text);
	}
	text[o < text_size ? o : text_size - 1] = 0;
	return true;
}

static void sms_arrived(const char *pdu)
{
	char number[40], text[700], escaped[1400], note[330];
	if (!sms_decode(pdu, number, sizeof(number), text, sizeof(text))) {
		fprintf(stderr, "ztoscorenetworkingd: a message that could not be read\n");
		return;
	}
	size_t o = 0;
	for (const char *c = text; *c && o + 3 < sizeof(escaped); c++) {
		if (*c == '\n') {
			escaped[o++] = '\\';
			escaped[o++] = 'n';
		} else if (*c != '\r' && *c != '\t') {
			escaped[o++] = *c;
		} else {
			escaped[o++] = ' ';
		}
	}
	escaped[o] = 0;
	log_line(SMS_LOG, "in\t%ld\t%s\t%s\n", (long)time(NULL), number, escaped);
	fprintf(stderr, "ztoscorenetworkingd: a message came in (%zu characters)\n", strlen(text));
	for (char *c = escaped; *c; c++)
		if (*c == '|')
			*c = '/';
	snprintf(note, sizeof(note), "notify Messaging|%s|%.240s\n", number, escaped);
	tell_shell(note);
}

/* ---- calls ----------------------------------------------------------------
 *
 * By AT: ATD<number>; dials, ATA answers, AT+CHUP ends. While there is a
 * call the modem is asked for its list (AT+CLCC) every two seconds; RING and
 * +CLIP announce one coming in. The sound of a call is not routed yet (the
 * audio driver has no voice path), so a call connects and is silent.
 *
 * CALL_LOG, one line each, tab separated: out|in|missed, <unix time>,
 * <number>, <seconds it lasted>.
 */
static long call_since, call_answered_at, call_poll_at;
static bool call_incoming, call_seen;

static void call_set(const char *state, const char *number)
{
	if (state[0] && !now.call[0]) {
		call_since = time(NULL);
		call_answered_at = 0;
		call_incoming = !strcmp(state, "incoming");
	}
	if (!strcmp(state, "active") && !call_answered_at)
		call_answered_at = time(NULL);
	if (!state[0] && now.call[0]) {
		/* over: into the log */
		long lasted = call_answered_at ? time(NULL) - call_answered_at : 0;
		log_line(CALL_LOG, "%s\t%ld\t%s\t%ld\n", !call_incoming ? "out" : call_answered_at ? "in" : "missed",
			call_since, now.call_number[0] ? now.call_number : "unknown", lasted);
		if (call_incoming && !call_answered_at) {
			char note[120];
			snprintf(note, sizeof(note), "notify Phone|Missed call|%s\n",
				now.call_number[0] ? now.call_number : "Unknown number");
			tell_shell(note);
		}
		now.call_number[0] = 0;
	}
	snprintf(now.call, sizeof(now.call), "%s", state);
	if (number && number[0])
		snprintf(now.call_number, sizeof(now.call_number), "%s", number);
}

static void on_line(char *l)
{
	if (!*l)
		return;
	/* the command's own final result */
	bool final = !strcmp(l, "OK") || !strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) ||
		!strncmp(l, "+CMS ERROR", 10);
	if (!strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER") || !strcmp(l, "NO DIALTONE")) {
		call_set("", "");
		if (busy && (!strncmp(sent, "ATD", 3) || !strncmp(sent, "ATA", 3)))
			busy = false;
		return;
	}
	if (final) {
		if (!strncmp(sent, "AT+CLCC", 7) && !strcmp(l, "OK")) {
			if (!call_seen && now.call[0])
				call_set("", "");	/* the list is empty: it is over */
			call_seen = false;
		}
		if (strcmp(l, "OK") && !strncmp(sent, "AT+CMGS", 7)) {
			log_line(SMS_LOG, "out\t%ld\t%s\tfailed\n", (long)time(NULL), sms_out_number);
			sms_out_number[0] = 0;
		}
		if (strcmp(l, "OK") && !strncmp(sent, "ATD", 3))
			call_set("", "");
		if (strcmp(l, "OK"))
			fprintf(stderr, "ztoscorenetworkingd: %s -> %s\n", sent, l);
		busy = false;
		return;
	}
	if (!strncmp(l, "+CPIN:", 6)) {
		const char *s = l + 6 + strspn(l + 6, " ");
		snprintf(now.sim, sizeof(now.sim), "%s", !strcmp(s, "READY") ? "ready" :
			strstr(s, "PUK") ? "puk" : strstr(s, "PIN") ? "pin" : "error");
	} else if (!strncmp(l, "+CSQ:", 5)) {
		int rssi = atoi(l + 5);
		if (rssi >= 0 && rssi != 99) {
			now.bars = csq_bars(rssi);
		} else if (now.bars <= 0 && (now.eps_reg == 1 || now.eps_reg == 5 || now.cs_reg == 1 || now.cs_reg == 5)) {
			now.bars = 3;
		}
	} else if (!strncmp(l, "+CESQ:", 6)) {
		int rxlev = 99, ber = 99, rscp = 255, ecno = 255, rsrq = 255, rsrp = 255;
		if (sscanf(l + 6, "%d,%d,%d,%d,%d,%d", &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) >= 1) {
			if (rsrp != 255 && rsrp >= 0) {
				now.bars = rsrp >= 64 ? 5 : rsrp >= 52 ? 4 : rsrp >= 40 ? 3 : rsrp >= 28 ? 2 : rsrp >= 15 ? 1 : 0;
			} else if (rscp != 255 && rscp >= 0) {
				now.bars = rscp >= 60 ? 5 : rscp >= 48 ? 4 : rscp >= 36 ? 3 : rscp >= 24 ? 2 : rscp >= 10 ? 1 : 0;
			} else if (rxlev != 99 && rxlev >= 0) {
				now.bars = rxlev >= 45 ? 5 : rxlev >= 35 ? 4 : rxlev >= 25 ? 3 : rxlev >= 15 ? 2 : rxlev >= 5 ? 1 : 0;
			}
		}
		if (now.bars <= 0 && (now.eps_reg == 1 || now.eps_reg == 5 || now.cs_reg == 1 || now.cs_reg == 5)) {
			now.bars = 3;
		}
	} else if (!strncmp(l, "+COPS:", 6)) {
		/* mode,format,"name",act */
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (q && e) {
			*e = 0;
			snprintf(now.operator, sizeof(now.operator), "%s", q + 1);
		} else {
			now.operator[0] = 0;
		}
	} else if (!strncmp(l, "+CREG:", 6)) {
		parse_reg(l + 6, !strncmp(sent, "AT+CREG?", 8), &now.cs_reg, &now.cs_act);
	} else if (!strncmp(l, "+CGREG:", 7)) {
		parse_reg(l + 7, !strncmp(sent, "AT+CGREG?", 9), &now.ps_reg, &now.ps_act);
	} else if (!strncmp(l, "+CEREG:", 7)) {
		parse_reg(l + 7, !strncmp(sent, "AT+CEREG?", 9), &now.eps_reg, &now.eps_act);
	} else if (!strncmp(l, "+CFUN:", 6)) {
		/* 4 (and 0) are the radio off; after a start this modem answers
		 * with other values while it is registered and working */
		/* ...and while it is between networks. So airplane mode is what was
		 * asked for here, and the modem's word only confirms it. */
		int fun = atoi(l + 6);
		now.airplane = airplane_asked && (fun == 4 || fun == 0);
	} else if (!strncmp(l, "+CMGS:", 6)) {
		fprintf(stderr, "ztoscorenetworkingd: SMS sent (reference %d)\n", atoi(l + 6));
		log_line(SMS_LOG, "out\t%ld\t%s\tok\n", (long)time(NULL), sms_out_number);
		sms_out_number[0] = 0;
	} else if (!strncmp(l, "+CMTI:", 6)) {
		/* +CMTI: "SM",3: a new message, and where it is */
		const char *comma = strrchr(l, ',');
		if (comma) {
			queue_at("AT+CMGF=0");
			queue_at("AT+CMGR=%d", atoi(comma + 1));
		}
	} else if (!strncmp(l, "+CMGR:", 6)) {
		sms_index = !strncmp(sent, "AT+CMGR=", 8) ? atoi(sent + 8) : -2;
	} else if (!strncmp(l, "+CMGL:", 6)) {
		sms_index = atoi(l + 6);
	} else if (sms_index != -1 && strlen(l) > 20 && strspn(l, "0123456789ABCDEFabcdef") == strlen(l)) {
		/* the message itself; then it is taken off the SIM */
		sms_arrived(l);
		if (sms_index >= 0)
			queue_at("AT+CMGD=%d", sms_index);
		sms_index = -1;
	} else if (!strcmp(l, "RING") || !strncmp(l, "+CRING", 6)) {
		if (strcmp(now.call, "incoming")) {
			call_set("incoming", "");
			tell_shell("notify Phone|Incoming call|Open Phone to answer\n");
		}
		call_poll_at = 0;
	} else if (!strncmp(l, "+CLIP:", 6)) {
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (q && e && e > q + 1) {
			*e = 0;
			snprintf(now.call_number, sizeof(now.call_number), "%s", q + 1);
		}
	} else if (!strncmp(l, "+CLCC:", 6)) {
		/* id,dir,stat,mode,mpty,"number",type */
		int id, dir, stat;
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (sscanf(l + 6, "%d,%d,%d", &id, &dir, &stat) == 3) {
			if (q && e)
				*e = 0;
			call_seen = true;
			call_set(stat == 0 ? "active" : stat == 2 ? "dialing" : stat == 3 ? "ringing" :
				stat == 4 || stat == 5 ? "incoming" : "active", q && e ? q + 1 : "");
		}
	}
}

static void at_readable(void)
{
	char buf[1024];
	ssize_t n = read(at_fd, buf, sizeof(buf));
	for (ssize_t i = 0; i < n; i++) {
		char c = buf[i];
		/* the SMS prompt has no line end */
		if (c == '>' && line_len == 0 && pdu_pending[0] && !strncmp(sent, "AT+CMGS", 7)) {
			char out[530];
			int m = snprintf(out, sizeof(out), "%s\x1a", pdu_pending);
			if (write(at_fd, out, m) != m)
				fprintf(stderr, "ztoscorenetworkingd: SMS body: %s\n", strerror(errno));
			pdu_pending[0] = 0;
			continue;
		}
		if (c == '\r' || c == '\n') {
			line[line_len] = 0;
			on_line(line);
			line_len = 0;
		} else if (line_len < sizeof(line) - 1) {
			line[line_len++] = c;
		}
	}
}

/* ---- packet data on rmnet0 -----------------------------------------------
 *
 * The context is made through IPC (see above), never by AT: on this modem
 * AT+CGDCONT with AT+CGACT=1,1 answers OK, brings nothing up and makes it
 * leave LTE ("denied", then 2G) until the radio is cycled.
 */
#define DATA_WISH	"/var/lib/ztoscorenetworking/data_on"
#define DATA_MARK	"/var/lib/ztoscorenetworking/data_trying"	/* see data_keep */
#define APN_FILE	"/etc/ztoscorenetworking/apn"

static void data_wish(bool on, bool say)
{
	(void)say;
	now.data_on = on;
	if (on) {
		int fd = open(DATA_WISH, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
		data_tried_at = 0;	/* now, not at the next half minute */
		if (ctx_up) {
			/* still there from before it was switched off */
			snprintf(now.ip, sizeof(now.ip), "%s", ctx_ip);
			now.data = true;
		}
	} else {
		unlink(DATA_WISH);
		unlink(DATA_MARK);
		now.data = false;	/* the interface comes down; the context stays */
		now.ip[0] = 0;
	}
}

static void run(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd) != 0)
		fprintf(stderr, "ztoscorenetworkingd: %s: failed\n", cmd);
}

/* The context's name servers, while nothing else (Wi-Fi) has given any:
 * NetworkManager empties the file when its last link goes. Looked at with
 * the poll, not more often. */
static void data_names(void)
{
	if (!now.data || !data_dns[0][0] || !strcmp(data_dns[0], "0.0.0.0"))
		return;
	run("grep -q '^nameserver' /etc/resolv.conf 2>/dev/null || "
		"printf 'nameserver %s\\nnameserver %s\\n' >> /etc/resolv.conf", data_dns[0],
		strcmp(data_dns[1], "0.0.0.0") ? data_dns[1] : data_dns[0]);
}

static void data_route(void)
{
	static char routed[48];
	if (now.data && now.ip[0] && strcmp(routed, now.ip)) {
		/* an IPv4 context: nothing of IPv6 may go down it (the kernel
		 * would send a router solicitation the moment the link is up) */
		run("sysctl -qw net.ipv6.conf.rmnet0.disable_ipv6=1; "
			"ip addr flush dev rmnet0; ip addr add %s/32 dev rmnet0 && ip link set rmnet0 up && "
			"ip route replace default dev rmnet0 metric 700", now.ip);
		snprintf(routed, sizeof(routed), "%s", now.ip);
		fprintf(stderr, "ztoscorenetworkingd: data on rmnet0\n");
		data_names();
	} else if (!now.data && routed[0]) {
		run("ip route del default dev rmnet0 metric 700 2>/dev/null; ip addr flush dev rmnet0; "
			"ip link set rmnet0 down");
		routed[0] = 0;
	}
}

/* ---- SMS-SUBMIT in PDU mode --------------------------------------------- */

static void send_sms(const char *number, const char *text)
{
	unsigned char tpdu[200];
	size_t n = 0, len = strlen(text);
	const char *d = number[0] == '+' ? number + 1 : number;
	size_t digits = strlen(d);
	if (len > 160 || digits > 20)
		return;
	tpdu[n++] = 0x11;		/* SMS-SUBMIT, relative validity period */
	tpdu[n++] = 0x00;		/* message reference: the modem's */
	tpdu[n++] = (unsigned char)digits;
	tpdu[n++] = number[0] == '+' ? 0x91 : 0x81;
	for (size_t i = 0; i < digits; i += 2) {
		int lo = d[i] - '0', hi = i + 1 < digits ? d[i + 1] - '0' : 0xf;
		tpdu[n++] = (unsigned char)(hi << 4 | lo);
	}
	tpdu[n++] = 0x00;		/* protocol */
	tpdu[n++] = 0x00;		/* the default alphabet */
	tpdu[n++] = 0xa7;		/* a day */
	tpdu[n++] = (unsigned char)len;
	unsigned bits = 0, have = 0;
	for (size_t i = 0; i < len; i++) {
		bits |= (unsigned)(text[i] & 0x7f) << have;
		have += 7;
		while (have >= 8) {
			tpdu[n++] = bits & 0xff;
			bits >>= 8;
			have -= 8;
		}
	}
	if (have)
		tpdu[n++] = bits & 0xff;
	char *p = pdu_pending;
	p += sprintf(p, "00");		/* the SIM's own SMS centre */
	for (size_t i = 0; i < n; i++)
		p += sprintf(p, "%02X", tpdu[i]);
	queue_at("AT+CMGF=0");
	queue_at("AT+CMGS=%zu", n);
}

/* ---- Samsung's IPC on umts_ipc0 -------------------------------------------
 *
 * A message: length (u16, this header included), message and ack sequence
 * numbers, main and sub command, type (requests: 1 exec, 2 get, 3 set;
 * replies: 1 indication, 2 response, 3 notification), data.
 */
static bool ipc_log;		/* with /var/lib/ztoscorenetworking/debug: every message, and a probe with the poll */
static unsigned char ipc_seq;

static void ipc_send(int main_cmd, int sub, int type, const unsigned char *data, size_t len)
{
	unsigned char msg[512];
	if (ipc_fd < 0 || len > sizeof(msg) - 7)
		return;
	ipc_seq = ipc_seq % 254 + 1;
	msg[0] = (7 + len) & 0xff;
	msg[1] = (7 + len) >> 8;
	msg[2] = ipc_seq;
	msg[3] = 0xff;
	msg[4] = main_cmd;
	msg[5] = sub;
	msg[6] = type;
	if (len)
		memcpy(msg + 7, data, len);
	if (write(ipc_fd, msg, 7 + len) < 0)
		fprintf(stderr, "ztoscorenetworkingd: ipc write: %s\n", strerror(errno));
	else if (ipc_log)
		fprintf(stderr, "ztoscorenetworkingd: ipc -> %02x%02x t%d, %zu bytes\n", main_cmd, sub, type, len);
}

/* ---- packet data, through IPC ---------------------------------------------
 *
 * The layouts are libsec-ril's (IpcProtocol41::IpcTxDefinePdpContext and
 * IpcTxSetPdpContext, read from its code):
 *
 *	0d01 set, 127 bytes: 1, cid, type (2: IP), apn[101], an IPv6 address[16],
 *		an IPv4 address[4], u16 0, u8 0
 *	0d04 set, 105 bytes: state (1: up, 0: down), cid, flags u32 (1: user,
 *		2: password, 0x10: auth), user[32], password[32], 32 x 0, auth,
 *		0, the reason for going down
 *
 * Each is answered by 8001 (main, sub, type, u16 result: 0x8000 is success).
 * Once up the modem reports 0d09, the context's addresses (cid, flags u16,
 * the IPv4 address, two name servers, more), and 0d10, its state (1 up,
 * 3 on its way). Context 1 is rmnet0.
 */
#define DATA_CID	1
/* The access point: /etc/ztoscorenetworking/apn, "key = value" lines (apn, user,
 * password, auth: none | pap | chap), as Settings > cellular + SIM writes
 * them. Without the file, Dialog's (Android's own list has it for 413/02). */
static struct { char apn[102], user[33], password[33]; int auth; } apn_cfg;

static void apn_load(void)
{
	char line[200];
	memset(&apn_cfg, 0, sizeof(apn_cfg));
	snprintf(apn_cfg.apn, sizeof(apn_cfg.apn), "dialogbb");
	FILE *f = fopen(APN_FILE, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *key = line + strspn(line, " \t"), *value = eq + 1 + strspn(eq + 1, " \t");
		key[strcspn(key, " \t")] = 0;
		value[strcspn(value, "\r\n")] = 0;
		for (char *e = value + strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t'); e--)
			e[-1] = 0;
		if (!strcmp(key, "apn") && value[0])
			snprintf(apn_cfg.apn, sizeof(apn_cfg.apn), "%s", value);
		else if (!strcmp(key, "user"))
			snprintf(apn_cfg.user, sizeof(apn_cfg.user), "%s", value);
		else if (!strcmp(key, "password"))
			snprintf(apn_cfg.password, sizeof(apn_cfg.password), "%s", value);
		else if (!strcmp(key, "auth"))
			apn_cfg.auth = !strcmp(value, "pap") ? 1 : !strcmp(value, "chap") ? 2 : 0;
	}
	if (f)
		fclose(f);
}

static long data_asked_at;	/* a define is out, its answer not yet back */
static int data_unanswered;

/* Defined first; brought up when the modem has said yes to that (see
 * ipc_message). */
static void data_activate(void)
{
	unsigned char define[127] = { 1, DATA_CID, 2 };
	apn_load();
	memcpy(define + 3, apn_cfg.apn, strlen(apn_cfg.apn));
	fprintf(stderr, "ztoscorenetworkingd: data: asking for context %d (%s)\n", DATA_CID, apn_cfg.apn);
	/* a mark, for as long as it has not been up a while: see data_keep */
	int fd = open(DATA_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	ipc_send(0x0d, 0x01, 3, define, sizeof(define));
	data_tried_at = data_asked_at = ms();
}

static void data_bring_up(void)
{
	unsigned char act[105] = { 1, DATA_CID };
	unsigned flags = 0;
	if (apn_cfg.user[0]) {
		memcpy(act + 6, apn_cfg.user, strlen(apn_cfg.user));
		flags |= 1;
	}
	if (apn_cfg.password[0]) {
		memcpy(act + 38, apn_cfg.password, strlen(apn_cfg.password));
		flags |= 2;
	}
	if (apn_cfg.auth) {
		act[102] = (unsigned char)apn_cfg.auth;
		flags |= 0x10;
	}
	act[2] = (unsigned char)flags;
	ipc_send(0x0d, 0x04, 3, act, sizeof(act));
}

static void data_deactivate(void)
{
	unsigned char act[105] = { 0, DATA_CID };
	ipc_send(0x0d, 0x04, 3, act, sizeof(act));
}

/*
 * Wanted, registered, and not up: asked for, and again every half minute.
 *
 * Two guards, from how this went wrong while it was being made to work:
 *
 *  - A fault in the receive path stops the kernel, and the wish is kept
 *    across restarts: that was a phone restarting for ever. So a mark is
 *    left when a context is first asked for and removed once it has been up
 *    for a minute and a half. A daemon that starts and finds the mark
 *    switches the wish off and says so.
 *  - After this daemon has been restarted (umts_ipc0 closed and opened
 *    again) the modem has been seen to stop answering requests, though its
 *    own reports keep coming. Two requests with no answer, and the modem is
 *    restarted (cbd boots it again; about 20 s without a network).
 */
static long data_up_at, modem_restarted_at;

static long rmnet_count(const char *which)
{
	char path[96];
	long n = 0;
	snprintf(path, sizeof(path), "/sys/class/net/rmnet0/statistics/%s_packets", which);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &n) != 1)
			n = 0;
		fclose(f);
	}
	return n;
}

static void data_keep(void)
{
	static long rx_at_up, tx_at_up;
	static bool checked;
	if (now.data && !data_up_at) {
		data_up_at = ms();
		rx_at_up = rmnet_count("rx");
		tx_at_up = rmnet_count("tx");
		checked = false;
	}
	if (!now.data)
		data_up_at = 0;
	if (data_up_at && !checked && ms() - data_up_at > 20000) {
		/* Up for a while: has anything come back? Only noted. After the
		 * modem has been restarted a context has been seen to receive
		 * nothing for most of a minute and then work; taking it down to
		 * try again is worse (see ctx_up). */
		checked = true;
		if (rmnet_count("tx") - tx_at_up >= 5 && rmnet_count("rx") == rx_at_up)
			fprintf(stderr, "ztoscorenetworkingd: data: nothing received yet on this context\n");
	}
	if (data_up_at && ms() - data_up_at > 90000 && access(DATA_MARK, F_OK) == 0)
		unlink(DATA_MARK);
	if (data_asked_at && ms() - data_asked_at > 10000) {
		data_asked_at = 0;
		if (++data_unanswered >= 2 && (!modem_restarted_at || ms() - modem_restarted_at > 300000)) {
			fprintf(stderr, "ztoscorenetworkingd: data: the modem does not answer; restarting it\n");
			data_unanswered = 0;
			modem_restarted_at = ms();
			if (system("systemctl restart --no-block ztoscorenetworking-modem.service") != 0)
				fprintf(stderr, "ztoscorenetworkingd: could not restart the modem\n");
		}
	}
	bool registered = now.eps_reg == 1 || now.eps_reg == 5 || now.ps_reg == 1 || now.ps_reg == 5;
	if (now.data_on && !ctx_up && registered &&
			(!data_tried_at || ms() - data_tried_at > 30000))
		data_activate();
}

static void ipc_message(int main_cmd, int sub, int type, const unsigned char *data, size_t len)
{
	if (ipc_log) {
		char hex[200] = "";
		/* what is the phone's or the SIM's own is not written down: its
		 * presence only */
		bool private = (main_cmd == 0x0d && sub == 0x09) || main_cmd == 0x0a ||
			(main_cmd == 0x05 && sub == 0x0c) || (main_cmd == 0x08 && sub == 0x06);
		for (size_t i = 0; i < len && i < 64 && !private; i++)
			sprintf(hex + 2 * i, "%02x", data[i]);
		fprintf(stderr, "ztoscorenetworkingd: ipc <- %02x%02x t%d, %zu bytes: %s\n", main_cmd, sub, type, len,
			private ? "(not logged)" : hex);
	}
	if (main_cmd == 0x01 && sub == 0x01) {
		/* the modem has (re)started: whatever was up is not any more */
		fprintf(stderr, "ztoscorenetworkingd: the modem started\n");
		ctx_up = false;
		now.data = false;
		now.ip[0] = 0;
		data_tried_at = 0;
	} else if (main_cmd == 0x80 && sub == 0x01 && len >= 5 && data[0] == 0x0d) {
		int result = data[3] | data[4] << 8;
		if (result != 0x8000) {
			fprintf(stderr, "ztoscorenetworkingd: data: 0d%02x refused (0x%04x)\n", data[1], result);
		} else if (data[1] == 0x01 && now.data_on && !ctx_up) {
			data_bring_up();
		}
		data_asked_at = 0;
		data_unanswered = 0;
	} else if (main_cmd == 0x0d && sub == 0x09 && len >= 6 && data[0] == DATA_CID) {
		/* which bytes are set, for working the rest of the layout out */
		char map[300] = "";
		for (size_t i = 0; i < len && i < sizeof(map) - 1; i++)
			map[i] = data[i] ? 'x' : '.';
		fprintf(stderr, "ztoscorenetworkingd: data: addresses for context %d, flags 0x%02x, set bytes: %s\n",
			data[0], data[1], map);
		/* cid, flags (u16), the address, two name servers */
		if (len >= 15 && (data[3] || data[4] || data[5] || data[6])) {
			snprintf(ctx_ip, sizeof(ctx_ip), "%d.%d.%d.%d", data[3], data[4], data[5], data[6]);
			ctx_up = true;
			snprintf(data_dns[0], sizeof(data_dns[0]), "%d.%d.%d.%d", data[7], data[8], data[9], data[10]);
			snprintf(data_dns[1], sizeof(data_dns[1]), "%d.%d.%d.%d", data[11], data[12], data[13], data[14]);
			if (now.data_on) {
				snprintf(now.ip, sizeof(now.ip), "%s", ctx_ip);
				now.data = true;
			}
		}
	} else if (main_cmd == 0x0d && sub == 0x10 && len >= 2 && data[0] == DATA_CID) {
		fprintf(stderr, "ztoscorenetworkingd: data: context %d state %d\n", data[0], data[1]);
		if (data[1] != 1) {
			ctx_up = false;
			now.data = false;
			now.ip[0] = 0;
		}
	} else if ((main_cmd == 0x07 && (sub == 0x01 || sub == 0x06)) ||
		   (main_cmd == 0x09 && sub == 0x02) ||
		   (main_cmd == 0x08 && (sub == 0x03 || sub == 0x04))) {
		if (len >= 1 && data[0] <= 5 && data[0] > 0)
			now.bars = data[0];
		else if (len >= 1 && data[0] > 5)
			now.bars = csq_bars(data[0]);
	}
}

static void ipc_readable(void)
{
	static unsigned char buf[8192];
	ssize_t n = read(ipc_fd, buf, sizeof(buf));
	for (ssize_t off = 0; n > 0 && off + 7 <= n;) {
		size_t len = buf[off] | buf[off + 1] << 8;
		if (len < 7 || off + (ssize_t)len > n)
			break;
		ipc_message(buf[off + 4], buf[off + 5], buf[off + 6], buf + off + 7, len - 7);
		off += len;
	}
}

/* ---- the control pipe ---------------------------------------------------- */

static void command(char *l)
{
	if (strncmp(l, "sms ", 4))
		fprintf(stderr, "ztoscorenetworkingd: asked: %.40s\n", l);
	if (!strcmp(l, "screen off") || !strcmp(l, "screen on")) {
		/* from zrnperfd: with the screen off nobody is looking at the bars */
		screen_off = !strcmp(l, "screen off");
		return;
	}
	if (!strcmp(l, "airplane on"))
		airplane_asked = true;
	else if (!strcmp(l, "airplane off"))
		airplane_asked = false;
	if (!strcmp(l, "airplane on")) {
		queue_at("AT+CFUN=4");
		queue_at("AT+CFUN?");
	} else if (!strcmp(l, "airplane off")) {
		queue_at("AT+CFUN=1");
		queue_at("AT+CFUN?");
	} else if (!strcmp(l, "data on")) {
		data_wish(true, true);
	} else if (!strcmp(l, "data off")) {
		data_wish(false, false);
	} else if (!strncmp(l, "sms ", 4)) {
		char number[32];
		int skip = 0;
		if (sscanf(l + 4, "%31s %n", number, &skip) == 1 && skip) {
			snprintf(sms_out_number, sizeof(sms_out_number), "%s", number);
			send_sms(number, l + 4 + skip);
		}
	} else if (!strncmp(l, "call ", 5)) {
		char number[32];
		if (sscanf(l + 5, "%31[0-9+*#]", number) == 1 && !now.call[0]) {
			call_set("dialing", number);
			queue_at("ATD%s;", number);
			call_poll_at = ms() + 1500;
		}
	} else if (!strcmp(l, "hangup")) {
		queue_at("AT+CHUP");
		call_poll_at = ms() + 500;
	} else if (!strcmp(l, "answer")) {
		queue_at("ATA");
		call_poll_at = ms() + 500;
	} else if (!strcmp(l, "apn")) {
		/* the access point changed: down, and up again with the new one */
		if (ctx_up)
			data_deactivate();
		ctx_up = false;
		now.data = false;
		now.ip[0] = 0;
		data_tried_at = ms() - 22000;	/* in a few seconds, once it is down */
	} else if (!strncmp(l, "at ", 3)) {
		queue_at("%s", l + 3);	/* one raw AT command, for finding things out */
	} else if (!strncmp(l, "ipc ", 4)) {
		/* ipc MAIN SUB TYPE [HEX]: one raw message, for finding things out */
		unsigned m, sb, t;
		int skip = 0;
		unsigned char data[400];
		size_t len = 0;
		if (sscanf(l + 4, "%x %x %u %n", &m, &sb, &t, &skip) >= 3) {
			for (const char *h = l + 4 + skip; skip && h[0] && h[1] && len < sizeof(data); h += 2) {
				unsigned byte;
				if (sscanf(h, "%2x", &byte) != 1)
					break;
				data[len++] = (unsigned char)byte;
			}
			ipc_send((int)m, (int)sb, (int)t, data, len);
		}
	} else if (!strcmp(l, "refresh")) {
		written_once = false;
	}
}

static void ctl_readable(void)
{
	static char buf[1024];
	static size_t len;
	ssize_t n = read(ctl_fd, buf + len, sizeof(buf) - 1 - len);
	if (n <= 0)
		return;
	len += n;
	buf[len] = 0;
	char *start = buf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = 0;
		command(start);
		start = nl + 1;
	}
	len = strlen(start);
	memmove(buf, start, len + 1);
	if (len == sizeof(buf) - 1)
		len = 0;
}

/* ---- Wi-Fi, for the shell ------------------------------------------------ */

static int wifi_fail_count = 0;
static void wifi_read(void)
{
	int bars = -1;
	char l[256];

	/* 1. NetworkManager active Wi-Fi connection and signal percentage */
	FILE *p = popen("nmcli -t -f IN-USE,SIGNAL dev wifi 2>/dev/null | grep '^\\*:' | head -1", "r");
	if (p) {
		if (fgets(l, sizeof(l), p)) {
			int sig = 0;
			if (sscanf(l, "*:%d", &sig) == 1) {
				bars = sig >= 75 ? 3 : sig >= 50 ? 2 : sig >= 25 ? 1 : 0;
			}
		}
		pclose(p);
	}

	/* 2. iw dev ... link */
	if (bars < 0) {
		p = popen("for d in /sys/class/net/w*; do [ -d \"$d\" ] && iw dev ${d##*/} link 2>/dev/null; done", "r");
		while (p && fgets(l, sizeof(l), p)) {
			int dbm;
			if (!strncmp(l, "Connected", 9) && bars < 0)
				bars = 3;
			if (sscanf(l, " signal: %d", &dbm) == 1)
				bars = dbm >= -60 ? 3 : dbm >= -72 ? 2 : dbm >= -82 ? 1 : 0;
		}
		if (p)
			pclose(p);
	}

	/* 3. NetworkManager active connection state */
	if (bars < 0) {
		p = popen("nmcli -t -f TYPE,STATE dev 2>/dev/null | grep -E '^wifi:connected|^wlan:connected'", "r");
		if (p) {
			if (fgets(l, sizeof(l), p))
				bars = (now.wifi_bars >= 0) ? now.wifi_bars : 3;
			pclose(p);
		}
	}

	/* 4. sysfs link carrier or operstate */
	if (bars < 0) {
		p = popen("for d in /sys/class/net/w*; do "
			"[ \"$(cat $d/carrier 2>/dev/null)\" = 1 ] && echo up && break; "
			"[ \"$(cat $d/operstate 2>/dev/null)\" = up ] && echo up && break; done", "r");
		if (p) {
			if (fgets(l, sizeof(l), p) && strncmp(l, "up", 2) == 0)
				bars = (now.wifi_bars >= 0) ? now.wifi_bars : 3;
			pclose(p);
		}
	}

	if (bars >= 0) {
		wifi_fail_count = 0;
		now.wifi_bars = bars;
	} else {
		/* Transient miss: require 3 consecutive misses (~30s) before reporting disconnected */
		if (++wifi_fail_count >= 3)
			now.wifi_bars = -1;
	}
}

/* ---- the loop ------------------------------------------------------------ */

static void poll_state(void)
{
	queue_at("AT+CPIN?");
	queue_at("AT+CSQ");
	queue_at("AT+CESQ");
	queue_at("AT+COPS?");
	queue_at("AT+CREG?");
	queue_at("AT+CGREG?");
	queue_at("AT+CEREG?");
	queue_at("AT+CFUN?");
}

int main(void)
{
	/* (until 2026 this was zrn-modemd: its messages, calls and settings move to the new names, once) */
	struct stat had;
	if (stat("/var/lib/zrn-modem", &had) == 0 && stat("/var/lib/ztoscorenetworking", &had) != 0) {
		rename("/var/lib/zrn-modem", "/var/lib/ztoscorenetworking");
	}
	if (stat("/etc/zrn-modem", &had) == 0 && stat("/etc/ztoscorenetworking", &had) != 0) {
		rename("/etc/zrn-modem", "/etc/ztoscorenetworking");
	}

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	for (int tries = 0; at_fd < 0; tries++) {
		at_fd = open(AT_DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (at_fd < 0) {
			if (tries == 0)
				fprintf(stderr, "ztoscorenetworkingd: waiting for %s\n", AT_DEV);
			sleep(2);
		}
	}
	ipc_fd = open("/dev/umts_ipc0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	rfs_fd = open("/dev/umts_rfs0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	unlink(CTL);
	mkfifo(CTL, 0666);
	chmod(CTL, 0666);
	ctl_fd = open(CTL, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	queue_at("ATE0");
	queue_at("AT+CMEE=2");
	queue_at("AT+COPS=3,0");	/* the operator by its long name */
	queue_at("AT+CREG=2");
	queue_at("AT+CGREG=2");
	queue_at("AT+CEREG=2");
	queue_at("AT+CNMI=2,1,0,0,0");	/* new messages reported */
	queue_at("AT+CLIP=1");		/* and who is calling */
	queue_at("AT+CMGF=0");
	queue_at("AT+CMGL=4");		/* whatever came while nobody was listening */
	poll_state();
	long next_poll = ms() + PERIOD_S * 1000;
	wifi_read();
	ipc_log = access("/var/lib/ztoscorenetworking/debug", F_OK) == 0;
	/* the access point is the user's to set (Settings writes the file) */
	mkdir("/etc/ztoscorenetworking", 0755);
	int apn_fd = open(APN_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (apn_fd >= 0) {
		fchmod(apn_fd, 0666);
		close(apn_fd);
	}
	if (access(DATA_MARK, F_OK) == 0 && access(STATUS, F_OK) != 0) {
		/* the mark, and no report of ours yet in /run: the system went
		 * down while a context was coming up */
		unlink(DATA_MARK);
		unlink(DATA_WISH);
		fprintf(stderr, "ztoscorenetworkingd: data: the phone restarted while it was coming up; switched off\n");
		tell_shell("notify Mobile network|Mobile data|Switched off: the phone restarted while the connection was "
			"coming up.\n");
	}
	data_wish(access(DATA_WISH, F_OK) == 0, false);
	fprintf(stderr, "ztoscorenetworkingd: up\n");

	for (;;) {
		struct pollfd fds[4] = {
			{ at_fd, POLLIN, 0 }, { ctl_fd, POLLIN, 0 }, { ipc_fd, POLLIN, 0 }, { rfs_fd, POLLIN, 0 },
		};
		next_at();
		long wait = next_poll - ms();
		if (now.call[0] && !busy && !q_len && ms() >= call_poll_at) {
			queue_at("AT+CLCC");
			call_poll_at = ms() + 2000;
			next_at();
		}
		if (now.call[0] && wait > 500)
			wait = 500;
		if (busy && AT_TIMEOUT_MS - (ms() - sent_at) < wait)
			wait = AT_TIMEOUT_MS - (ms() - sent_at);
		if (wait < 0)
			wait = 0;
		if (poll(fds, 4, (int)wait) < 0 && errno != EINTR)
			break;
		if (fds[0].revents & POLLIN)
			at_readable();
		if (fds[1].revents & POLLIN)
			ctl_readable();
		char drain[4096];
		/* the command and file channels: kept open, not answered */
		if (ipc_fd >= 0 && (fds[2].revents & POLLIN))
			ipc_readable();
		if (rfs_fd >= 0 && (fds[3].revents & POLLIN)) {
			/* the file channel: what the modem asks for is noted (its
			 * length and command byte, never its contents), not served */
			static int noted;
			ssize_t got = read(rfs_fd, drain, sizeof(drain));
			if (got >= 6 && noted < 12) {
				/* and a file's name, when it is asked to be opened */
				char name[80] = "";
				size_t at = 0;
				for (ssize_t i = 6; i < got && i < 200 && at < sizeof(name) - 1 &&
						(unsigned char)drain[4] == 0x11; i++) {
					if (drain[i] >= 0x20 && drain[i] < 0x7f)
						name[at++] = drain[i];
				}
				name[at] = 0;
				noted++;
				fprintf(stderr, "ztoscorenetworkingd: rfs <- %zd bytes, command 0x%02x, id %d %s\n", got,
					(unsigned char)drain[4], (unsigned char)drain[5], name);
			}
		}
		if (busy && ms() - sent_at > AT_TIMEOUT_MS) {
			fprintf(stderr, "ztoscorenetworkingd: %s: no answer\n", sent);
			busy = false;
		}
		if (ms() >= next_poll) {
			if (ipc_log)
				ipc_send(0x05, 0x01, 2, NULL, 0);	/* while finding out: does it still answer? */
			poll_state();
			if (!screen_off)
				wifi_read();	/* (it runs a program: not for a dark screen) */
			data_names();
			next_poll = ms() + (screen_off ? PERIOD_OFF_S : PERIOD_S) * 1000;
		}
		if (!busy && !q_len) {
			data_keep();
			data_route();
			publish();
		}
	}
	return 1;
}
