#include "core/config.h"
#include "core/keybinds.h"
#include "game/ai_granny_hook.h"
#include "game/fullbright.h"
#include "game/granny_ai.h"
#include "game/player.h"
#include "game/spawn.h"
#include "game/traps.h"
#include "game/unlock.h"
#include "overlay/esp.h"
#include "overlay/notify.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
	CFG_BOOL,
	CFG_FLOAT,
	CFG_INT
} cfg_type;

typedef struct {
	const char *name;
	cfg_type type;
	void *value;
	/* The range the menu's widget offers. A config file is hand-editable and
	 * survives across versions, so without this it can hold values the UI
	 * cannot produce and cannot show -- notify_duration=0 kills every toast
	 * while the checkbox still reads enabled. Both zero means unbounded. */
	double lo, hi;
} cfg_entry;

/*
 * Everything worth remembering between sessions.
 *
 * Keyed by name rather than by position, so adding a setting here doesn't
 * invalidate anyone's saved file and removing one just means the old line is
 * ignored. The ESP's debug switches are deliberately absent: they exist to
 * chase a specific bug, and having one silently persist into the next
 * session is exactly the sort of thing that wastes an evening.
 */
static const cfg_entry g_entries[] = {
	{ "immortality",          CFG_BOOL,  &immortality },
	{ "noclip",               CFG_BOOL,  &player_noclip_enabled },
	{ "move_speed_enabled",   CFG_BOOL,  &player_speed_enabled },
	{ "move_speed",           CFG_FLOAT, &player_speed_multiplier , 0.1, 10.0 },
	{ "no_hard_landing",      CFG_BOOL,  &player_no_hard_landing },
	{ "air_control",          CFG_BOOL,  &player_air_control },

	{ "granny_blind",         CFG_BOOL,  &granny_is_blind },
	{ "granny_deaf",          CFG_BOOL,  &granny_is_deaf },
	{ "granny_freeze",        CFG_BOOL,  &granny_freeze_enabled },
	{ "granny_speed_enabled", CFG_BOOL,  &granny_speed_enabled },
	{ "granny_walk_speed",    CFG_FLOAT, &granny_walk_speed , 0.0, 500.0 },
	{ "granny_run_speed",     CFG_FLOAT, &granny_run_speed , 0.0, 500.0 },

	{ "traps_disabled",       CFG_BOOL,  &traps_disabled },
	{ "unlock_without_keys",  CFG_BOOL,  &unlock_enabled },
	{ "spawn_selected",       CFG_INT,   &spawn_selected_index , 0, ITEMSPAWN_ITEM_COUNT - 1 },

	{ "fullbright",           CFG_BOOL,  &fullbright_enabled },
	{ "granny_esp",           CFG_BOOL,  &esp_granny_enabled },
	{ "item_esp",             CFG_BOOL,  &esp_items_enabled },
	{ "momspider_esp",        CFG_BOOL,  &esp_momspider_enabled },
	{ "esp_arrows",           CFG_BOOL,  &esp_arrows_enabled },
	{ "esp_arrow_size",       CFG_FLOAT, &esp_arrow_size , 10.0, 60.0 },
	{ "esp_arrow_margin",     CFG_FLOAT, &esp_arrow_margin , 20.0, 200.0 },
	{ "esp_escape",           CFG_BOOL,  &esp_show_escape_items },
	{ "esp_escape_puzzle",    CFG_BOOL,  &esp_show_escape_puzzle_items },
	{ "esp_puzzle",           CFG_BOOL,  &esp_show_puzzle_items },
	{ "esp_other",            CFG_BOOL,  &esp_show_other_items },
	{ "esp_box_height",       CFG_FLOAT, &esp_box_height , 0.5, 5.0 },
	{ "esp_box_width",        CFG_FLOAT, &esp_box_width_ratio , 0.1, 1.5 },
	{ "esp_spider_height",    CFG_FLOAT, &esp_spider_box_height , 0.3, 4.0 },
	{ "esp_spider_width",     CFG_FLOAT, &esp_spider_box_width_ratio , 0.2, 3.0 },

	{ "notifications",        CFG_BOOL,  &notify_enabled },
	{ "notify_position",      CFG_INT,   &notify_position , 0, NOTIFY_POSITION_COUNT - 1 },
	{ "notify_duration",      CFG_FLOAT, &notify_duration , 0.5, 8.0 },
};

#define CFG_ENTRY_COUNT ((int)(sizeof(g_entries) / sizeof(g_entries[0])))

/* Keybinds get a "key." prefix and their own slug, so the file survives an
 * action being inserted into the middle of the enum -- which is exactly what
 * happened when noclip's up and down keys were added. */
#define CFG_KEY_PREFIX "key."

static char g_status[256] = "not loaded";
static char g_path[MAX_PATH];

const char *config_path(void) {
	if (g_path[0] != '\0') return g_path;

	char appdata[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return g_path; /* still empty */

	wsprintfA(g_path, "%s\\grannycheat\\config.ini", appdata);
	return g_path;
}

static bool ensure_folder(void) {
	char appdata[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return false;

	char folder[MAX_PATH];
	wsprintfA(folder, "%s\\grannycheat", appdata);
	/* Already existing is success, not failure. */
	if (!CreateDirectoryA(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}
	return true;
}

/* --------------------------------------------------------------------- */

static void trim(char *s) {
	int len = (int)strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
	                   s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[--len] = '\0';
	}
}

static void assign(const cfg_entry *entry, const char *text) {
	switch (entry->type) {
	case CFG_BOOL:
		*(bool *)entry->value = (atoi(text) != 0);
		break;
	case CFG_INT: {
		int v = atoi(text);
		if (entry->lo != entry->hi) {
			if (v < (int)entry->lo) v = (int)entry->lo;
			if (v > (int)entry->hi) v = (int)entry->hi;
		}
		*(int *)entry->value = v;
		break;
	}
	case CFG_FLOAT:
		/* strtod, not atof: a config written under a locale that uses a
		 * comma decimal separator would otherwise silently truncate every
		 * float to its integer part. strtod is locale-dependent too, but at
		 * least it reports how far it got. */
		{
			double v = strtod(text, NULL);
			if (entry->lo != entry->hi) {
				if (v < entry->lo) v = entry->lo;
				if (v > entry->hi) v = entry->hi;
			}
			*(float *)entry->value = (float)v;
		}
		break;
	}
}

static bool apply_line(char *line) {
	char *equals = strchr(line, '=');
	if (!equals) return false;
	*equals = '\0';

	char *key = line;
	char *value = equals + 1;
	trim(key);
	trim(value);
	if (key[0] == '\0' || key[0] == '#') return false;

	for (int i = 0; i < CFG_ENTRY_COUNT; i++) {
		if (lstrcmpiA(key, g_entries[i].name) == 0) {
			assign(&g_entries[i], value);
			return true;
		}
	}

	if (strncmp(key, CFG_KEY_PREFIX, sizeof(CFG_KEY_PREFIX) - 1) == 0) {
		const char *slug = key + sizeof(CFG_KEY_PREFIX) - 1;
		for (int i = 0; i < KEYBIND_COUNT; i++) {
			if (lstrcmpiA(slug, keybind_config_name((keybind_id)i)) == 0) {
				int vk = atoi(value);
				/* Anything outside a virtual-key code would be unpressable
				 * and unclearable from the menu, so drop it to unbound. */
				keybind_keys[i] = (vk > 0 && vk < 256) ? vk : 0;
				return true;
			}
		}
	}

	return false;
}

bool config_load(void) {
	const char *path = config_path();
	if (path[0] == '\0') {
		lstrcpynA(g_status, "no %APPDATA%, config disabled", sizeof(g_status));
		return false;
	}

	FILE *file = fopen(path, "r");
	if (!file) {
		lstrcpynA(g_status, "no saved config yet", sizeof(g_status));
		return false;
	}

	int applied = 0;
	char line[512];
	while (fgets(line, sizeof(line), file)) {
		if (apply_line(line)) applied++;
	}
	fclose(file);

	wsprintfA(g_status, "loaded %d settings", applied);
	OutputDebugStringA("[cheat] config loaded");
	return true;
}

void config_apply(void) {
	/* Only the two that need more than their flag set. Both are idempotent,
	 * so calling them when nothing was loaded is harmless. */
	if (immortality) granny_apply_immortality();
	if (granny_is_deaf) granny_apply_deaf();
	if (traps_disabled) traps_apply();
	if (unlock_enabled) unlock_apply();
	if (player_air_control) player_apply_air_control();

	/* The speed overrides are picked up by their own ticks, but they only
	 * write on a change -- so tell them something changed. */
	granny_speed_mark_dirty();
	player_mark_dirty();
}

bool config_save(void) {
	const char *path = config_path();
	if (path[0] == '\0') {
		lstrcpynA(g_status, "no %APPDATA%, cannot save", sizeof(g_status));
		return false;
	}
	if (!ensure_folder()) {
		lstrcpynA(g_status, "could not create the config folder", sizeof(g_status));
		return false;
	}

	FILE *file = fopen(path, "w");
	if (!file) {
		lstrcpynA(g_status, "could not open the config for writing", sizeof(g_status));
		return false;
	}

	fprintf(file, "# grannycheat config -- rewritten whenever you press Save.\n");
	fprintf(file, "# Keys are virtual-key codes; 0 means unbound.\n\n");

	for (int i = 0; i < CFG_ENTRY_COUNT; i++) {
		const cfg_entry *entry = &g_entries[i];
		switch (entry->type) {
		case CFG_BOOL:
			fprintf(file, "%s=%d\n", entry->name, *(bool *)entry->value ? 1 : 0);
			break;
		case CFG_INT:
			fprintf(file, "%s=%d\n", entry->name, *(int *)entry->value);
			break;
		case CFG_FLOAT:
			fprintf(file, "%s=%.4f\n", entry->name, (double)*(float *)entry->value);
			break;
		}
	}

	fprintf(file, "\n");
	for (int i = 0; i < KEYBIND_COUNT; i++) {
		fprintf(file, CFG_KEY_PREFIX "%s=%d\n", keybind_config_name((keybind_id)i),
		        keybind_keys[i]);
	}

	fclose(file);
	lstrcpynA(g_status, "saved", sizeof(g_status));
	OutputDebugStringA("[cheat] config saved");
	return true;
}

const char *config_status(void) {
	return g_status;
}
