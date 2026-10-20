// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztcontacts: People, for ztMETRO.
 *
 * Three pages, in the manner of the phone's own:
 *
 *   people	everyone, under the letter their name starts with; a round
 *		button to add one. Tap a name for its card; hold it for a
 *		menu (edit, delete).
 *   profile	one person: call, text, the other number; edit and delete.
 *   edit	the same person as boxes to type into; save or cancel.
 *
 * The people are kept in ~/.local/share/ztmetro/contacts (comm.h), which
 * Phone and Messaging read to put names to numbers.
 *
 *	ztcontacts			the list
 *	ztcontacts --add NUMBER		a new person with that number filled in
 *	ztcontacts --render PAGE FILE	draw a page into a PNG (all, card, edit)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>

#include "zrnmetrocore.h"
#include "comm.h"

#define MARGIN ZMC_MARGIN

enum page { P_ALL, P_CARD, P_EDIT };

enum action {
	ACT_OPEN = 1,	/* arg: who */
	ACT_NEW,
	ACT_EDIT,
	ACT_DELETE,
	ACT_CALL,	/* arg: 0 mobile, 1 other */
	ACT_TEXT,
	ACT_FIELD,	/* arg: which box */
	ACT_SAVE,
	ACT_CANCEL,
	ACT_MENU,	/* arg: which item of the held person's menu */
	ACT_MENU_CLOSE,
};

static int shown = -1;		/* who the card is of */
static int menu_for = -1;	/* whose menu is open on the list */
static bool confirm_delete;	/* the card's "delete" has been tapped once */

/* the boxes of the edit page */
enum { F_NAME, F_MOBILE, F_OTHER, F_EMAIL, F_NOTE, N_FIELDS };
static const char *const FIELD_NAMES[N_FIELDS] = { "Name", "Mobile", "Other number", "E-mail", "Note" };
static char field[N_FIELDS][96];
static int focus = -1;
static int editing = -1;	/* who is being edited; -1: someone new */
static char edit_note[64];

static void edit_begin(int who, const char *number) {
	editing = who;
	memset(field, 0, sizeof(field));
	edit_note[0] = 0;
	if (who >= 0 && who < n_contacts) {
		struct contact *c = &contacts[who];
		snprintf(field[F_NAME], sizeof(field[0]), "%s", c->name);
		snprintf(field[F_MOBILE], sizeof(field[0]), "%s", c->mobile);
		snprintf(field[F_OTHER], sizeof(field[0]), "%s", c->other);
		snprintf(field[F_EMAIL], sizeof(field[0]), "%s", c->email);
		snprintf(field[F_NOTE], sizeof(field[0]), "%s", c->note);
	} else if (number) {
		snprintf(field[F_MOBILE], sizeof(field[0]), "%s", number);
	}
	focus = F_NAME;
}

/* ---- people --------------------------------------------------------------- */

static double draw_all(cairo_t *cr) {
	double y = zmc_header(cr, "people");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "add", "new", ACT_NEW, 0);
	y += 76;

	if (n_contacts == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Nobody here yet.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	char letter = 0;
	double menu_at = -1;
	for (int i = 0; i < n_contacts; i++) {
		struct contact *c = &contacts[i];
		char first = (char)toupper((unsigned char)c->name[0]);
		if (first < 'A' || first > 'Z') {
			first = '#';
		}
		if (first != letter) {
			/* the letter, in a square of the accent colour, as in the app list */
			char text[2] = { (char)tolower((unsigned char)first), 0 };
			letter = first;
			y += 6;
			zmc_accent(cr);
			cairo_rectangle(cr, MARGIN, y, 42, 42);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, text, FONT_SEMILIGHT, 26, MARGIN + 6, y + 8, 0);
			y += 52;
		}
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, c->name, FONT_SEMILIGHT, 24, MARGIN, y + 6, zmc.width - 2 * MARGIN);
		zmc_add_hit(0, y, zmc.width, 46, ACT_OPEN, i);
		if (i == menu_for) {
			menu_at = y + 44;
		}
		y += 48;
	}
	y += 24;
	if (menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "edit", "delete" };
		zmc_menu(cr, menu_at, items, 2, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y;
}

/* ---- one person ----------------------------------------------------------- */

static double draw_card(cairo_t *cr) {
	double y = zmc_header(cr, "profile");
	if (shown < 0 || shown >= n_contacts) {
		return y;
	}
	struct contact *c = &contacts[shown];
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, c->name, FONT_LIGHT, 34, MARGIN, y - 8, zmc.width - 2 * MARGIN) + 8;

	if (c->mobile[0]) {
		y = zmc_entry(cr, y, "call mobile", c->mobile, ACT_CALL, 0);
		y = zmc_entry(cr, y, "text", c->mobile, ACT_TEXT, 0);
	}
	if (c->other[0]) {
		y = zmc_entry(cr, y, "call other", c->other, ACT_CALL, 1);
	}
	if (!c->mobile[0] && !c->other[0]) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "No number.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 16;
	}
	if (c->email[0]) {
		y = zmc_field(cr, y + 4, "E-mail", c->email);
	}
	if (c->note[0]) {
		y = zmc_field(cr, y + 4, "Note", c->note);
	}
	y += 14;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "edit", "edit", ACT_EDIT, 0);
	zmc_round_button(cr, MARGIN + 22 + 76, y, "delete", confirm_delete ? "tap again" : "delete", ACT_DELETE, 0);
	return y + 90;
}

/* ---- typing one in -------------------------------------------------------- */

static double draw_edit(cairo_t *cr) {
	double y = zmc_header(cr, editing >= 0 ? "edit" : "new");
	for (int i = 0; i < N_FIELDS; i++) {
		y = zmc_textbox(cr, y, FIELD_NAMES[i], field[i], focus == i, ACT_FIELD, i);
	}
	if (edit_note[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, edit_note, FONT_REGULAR, 13.5, MARGIN, y, zmc.width - 2 * MARGIN) + 10;
	}
	y += 6;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "save", "save", ACT_SAVE, 0);
	zmc_round_button(cr, MARGIN + 22 + 76, y, "cancel", "cancel", ACT_CANCEL, 0);
	return y + 90;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_ALL: return draw_all(cr);
	case P_CARD: return draw_card(cr);
	case P_EDIT: return draw_edit(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------- */

static void save_edit(void) {
	if (!field[F_NAME][0]) {
		snprintf(edit_note, sizeof(edit_note), "A name is needed.");
		focus = F_NAME;
		zmc_keyboard(true);
		return;
	}
	struct contact *c;
	if (editing >= 0 && editing < n_contacts) {
		c = &contacts[editing];
	} else if (n_contacts < MAX_CONTACTS) {
		c = &contacts[n_contacts++];
	} else {
		snprintf(edit_note, sizeof(edit_note), "The list is full.");
		return;
	}
	char name[64];
	snprintf(name, sizeof(name), "%s", field[F_NAME]);
	snprintf(c->name, sizeof(c->name), "%s", field[F_NAME]);
	snprintf(c->mobile, sizeof(c->mobile), "%s", field[F_MOBILE]);
	snprintf(c->other, sizeof(c->other), "%s", field[F_OTHER]);
	snprintf(c->email, sizeof(c->email), "%s", field[F_EMAIL]);
	snprintf(c->note, sizeof(c->note), "%s", field[F_NOTE]);
	contacts_save();	/* (sorts: the person may have moved) */
	shown = -1;
	for (int i = 0; i < n_contacts; i++) {
		if (!strcmp(contacts[i].name, name)) {
			shown = i;
		}
	}
	focus = -1;
	zmc_keyboard(false);
	zmc_haptic("click");
	zmc_go(shown >= 0 ? P_CARD : P_ALL, -1);
}

static void activate(int action, int arg) {
	char command[160];
	if (action != ACT_DELETE) {
		confirm_delete = false;
	}
	switch (action) {
	case ACT_OPEN:
		if (arg >= 0 && arg < n_contacts) {
			shown = arg;
			zmc_go(P_CARD, 1);
		}
		break;
	case ACT_NEW:
		edit_begin(-1, NULL);
		zmc_go(P_EDIT, 1);
		zmc_keyboard(true);
		break;
	case ACT_EDIT:
		edit_begin(shown, NULL);
		zmc_go(P_EDIT, 1);
		zmc_keyboard(true);
		break;
	case ACT_DELETE:
		/* twice: once to ask, once to do it */
		if (!confirm_delete) {
			confirm_delete = true;
			break;
		}
		confirm_delete = false;
		contact_delete(shown);
		shown = -1;
		zmc_haptic("click");
		zmc_go(P_ALL, -1);
		break;
	case ACT_CALL:
		if (shown >= 0 && shown < n_contacts) {
			const char *number = arg ? contacts[shown].other : contacts[shown].mobile;
			snprintf(command, sizeof(command), "ztphone --call '%s'", number);
			run_app(command);
		}
		break;
	case ACT_TEXT:
		if (shown >= 0 && shown < n_contacts) {
			snprintf(command, sizeof(command), "ztmessages '%s'", contacts[shown].mobile);
			run_app(command);
		}
		break;
	case ACT_FIELD:
		focus = arg;
		edit_note[0] = 0;
		zmc_keyboard(true);
		break;
	case ACT_SAVE:
		save_edit();
		break;
	case ACT_CANCEL:
		focus = -1;
		zmc_keyboard(false);
		zmc_go(editing >= 0 && shown >= 0 ? P_CARD : P_ALL, -1);
		break;
	case ACT_MENU: {
		int who = menu_for;
		menu_for = -1;
		if (who < 0 || who >= n_contacts) {
			break;
		}
		if (arg == 0) {
			shown = who;
			edit_begin(who, NULL);
			zmc_go(P_EDIT, 1);
			zmc_keyboard(true);
		} else {
			contact_delete(who);
			zmc_haptic("click");
		}
		break;
	}
	case ACT_MENU_CLOSE:
		menu_for = -1;
		break;
	}
}

static bool hold(int action, int arg) {
	if (zmc.page == P_ALL && action == ACT_OPEN && menu_for < 0) {
		menu_for = arg;
		return true;
	}
	return false;
}

static void typed(const char *text) {
	if (zmc.page != P_EDIT || focus < 0) {
		return;
	}
	char *f = field[focus];
	size_t len = strlen(f);
	size_t max = focus == F_MOBILE || focus == F_OTHER ? 20 : focus == F_NAME ? 60 : 90;
	edit_note[0] = 0;
	if (text[0] == '\b') {
		/* a whole character, of however many bytes */
		while (len > 0 && ((unsigned char)f[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		f[len > 0 ? len - 1 : 0] = 0;
	} else if (text[0] == '\n') {
		if (focus < N_FIELDS - 1) {
			focus++;	/* on to the next box */
		} else {
			save_edit();
		}
	} else if (len + strlen(text) < max) {
		if ((focus == F_MOBILE || focus == F_OTHER) && !number_char(text[0]) && text[0] != ' ') {
			return;	/* numbers take digits and their signs only */
		}
		if (len == 0 && focus == F_NAME && text[0] >= 'a' && text[0] <= 'z') {
			f[0] = (char)(text[0] - 32);	/* a name starts with a capital */
			f[1] = 0;
		} else {
			strcat(f, text);
		}
	}
}

static bool back(void) {
	if (menu_for >= 0) {
		menu_for = -1;
		zmc_redraw();
		return true;
	}
	confirm_delete = false;
	if (zmc.page == P_EDIT) {
		focus = -1;
		zmc_keyboard(false);
		zmc_go(editing >= 0 && shown >= 0 ? P_CARD : P_ALL, -1);
		return true;
	}
	if (zmc.page == P_CARD) {
		zmc_go(P_ALL, -1);
		return true;
	}
	return false;
}

static const struct zmc_app people = {
	.app_id = "ztcontacts", .title = "People", .caps = "PEOPLE",
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold,
};

int main(int argc, char **argv) {
	contacts_load();
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		/* a few people to draw, when there are none: only for looking at the pages */
		if (n_contacts == 0) {
			static const char *const sample[][2] = { { "Amara Perera", "0771234567" },
				{ "Anil", "0712223334" }, { "Dilani Fernando", "+94779876543" },
				{ "Kasun", "0765556667" }, { "Nimal Silva", "0114445556" } };
			for (int i = 0; i < 5; i++) {
				snprintf(contacts[i].name, sizeof(contacts[i].name), "%s", sample[i][0]);
				snprintf(contacts[i].mobile, sizeof(contacts[i].mobile), "%s", sample[i][1]);
			}
			snprintf(contacts[2].other, sizeof(contacts[2].other), "0112345678");
			snprintf(contacts[2].email, sizeof(contacts[2].email), "dilani@example.com");
			n_contacts = 5;
		}
		int page = !strcmp(argv[2], "card") ? P_CARD : !strcmp(argv[2], "edit") ? P_EDIT : P_ALL;
		shown = 2;
		if (page == P_EDIT) {
			edit_begin(2, NULL);
		}
		return zmc_render_png(&people, page, 664, argv[3]);
	}
	int first = P_ALL;
	if (argc == 3 && !strcmp(argv[1], "--add")) {
		edit_begin(-1, argv[2]);
		first = P_EDIT;
	}
	int ret = zmc_run(&people, first);
	return ret;
}
