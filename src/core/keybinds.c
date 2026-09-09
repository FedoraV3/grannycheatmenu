#include "core/keybinds.h"

#include <windows.h>

/*
 * Defaults are all function keys plus Insert for the menu, deliberately:
 * Granny binds most of the letter keys, WASD, Shift, Ctrl, Space, Tab and
 * Escape, and a cheat key that also crouches you or drops what you're
 * holding is worse than no cheat key. F1-F12 and Insert are the block the
 * game doesn't touch.
 *
 * Granny speed is left unbound -- it's driven by sliders, so toggling it
 * blind isn't much use.
 */
int keybind_keys[KEYBIND_COUNT] = {
	VK_INSERT, /* menu */
	VK_F1,     /* granny esp */
	VK_F2,     /* item esp */
	0,         /* mom spider esp -- no free function key left */
	0,         /* off-screen arrows */
	VK_F3,     /* fullbright */
	VK_F4,     /* immortality */
	VK_F5,     /* noclip */
	'R',       /* noclip rise */
	'Q',       /* noclip descend */
	VK_F6,     /* move speed */
	VK_F7,     /* blind */
	VK_F8,     /* deaf */
	VK_F9,     /* freeze */
	0,         /* granny speed -- slider driven, no useful default */
	VK_F10,    /* stop granny */
	VK_F11,    /* disable traps */
	VK_F12,    /* spawn item */
	0,         /* spawn every item -- unbound, it's a big irreversible mess */
};

/*
 * Noclip's rise and descend are the exception to the no-letter-keys rule
 * above, on request. They're safe in a way the toggles aren't: they're only
 * read while noclip is actually on, and the previous defaults were worse --
 * Ctrl to descend is also the crouch key, and Space is bound to enough in
 * this game that flying with it was asking for trouble.
 */

static const char *const g_names[KEYBIND_COUNT] = {
	"Toggle menu",
	"Granny ESP",
	"Item ESP",
	"Mom Spider ESP",
	"Off-screen arrows",
	"Fullbright",
	"Immortality",
	"Noclip",
	"Noclip: rise",
	"Noclip: descend",
	"Move speed",
	"Blind",
	"Deaf",
	"Freeze in place",
	"Granny speed",
	"Stop granny",
	"Disable traps",
	"Spawn selected item",
	"Spawn every item",
};

/* Config-file keys. Kept short, lower case and stable -- renaming one of
 * these silently unbinds that action for everyone who already has a config. */
static const char *const g_slugs[KEYBIND_COUNT] = {
	"menu",
	"granny_esp",
	"item_esp",
	"momspider_esp",
	"arrows",
	"fullbright",
	"immortality",
	"noclip",
	"noclip_up",
	"noclip_down",
	"move_speed",
	"blind",
	"deaf",
	"freeze",
	"granny_speed",
	"stop_granny",
	"disable_traps",
	"spawn_item",
	"spawn_all",
};

/* Held state from the previous frame, so a press is an edge rather than a
 * level. Indexed by virtual-key code, not by action: two actions bound to
 * the same key then agree with each other instead of one eating the edge. */
static bool g_was_down[256];
static bool g_pressed[KEYBIND_COUNT];

/*
 * Three arrays indexed by one enum, and C will happily pad a short
 * initialiser list with zeros rather than complain -- which would hand
 * keybind_name() a NULL for the missing entry and let the Keybinds tab
 * dereference it. Entries have already been inserted into the middle of this
 * enum twice, so make the compiler check rather than the player.
 */
_Static_assert(sizeof(keybind_keys) / sizeof(keybind_keys[0]) == KEYBIND_COUNT,
               "keybind_keys is out of step with keybind_id");
_Static_assert(sizeof(g_names) / sizeof(g_names[0]) == KEYBIND_COUNT,
               "g_names is out of step with keybind_id");
_Static_assert(sizeof(g_slugs) / sizeof(g_slugs[0]) == KEYBIND_COUNT,
               "g_slugs is out of step with keybind_id");

static keybind_id g_capturing = KEYBIND_COUNT;

/* The NULL check is belt and braces alongside the assertions above: the
 * callers hand these straight to ImGui and to the config writer, neither of
 * which survives a null. */
const char *keybind_name(keybind_id id) {
	if (id < 0 || id >= KEYBIND_COUNT || !g_names[id]) return "?";
	return g_names[id];
}

const char *keybind_config_name(keybind_id id) {
	if (id < 0 || id >= KEYBIND_COUNT || !g_slugs[id]) return "?";
	return g_slugs[id];
}

static bool key_is_down(int vk) {
	if (vk <= 0 || vk > 255) return false;
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Returns the first key that went down this frame, or 0. Used only while
 * rebinding. */
static int first_new_key(const bool *now_down) {
	/* From VK_BACK: everything below it is mouse buttons, and binding a
	 * cheat to left click would make the menu unusable. */
	for (int vk = VK_BACK; vk < 256; vk++) {
		if (now_down[vk] && !g_was_down[vk]) return vk;
	}
	return 0;
}

void keybinds_update(bool accept_input) {
	bool now_down[256];
	now_down[0] = false;
	for (int vk = 1; vk < 256; vk++) {
		now_down[vk] = key_is_down(vk);
	}

	for (int i = 0; i < KEYBIND_COUNT; i++) {
		g_pressed[i] = false;
	}

	if (g_capturing != KEYBIND_COUNT) {
		int vk = first_new_key(now_down);
		if (vk == VK_ESCAPE) {
			g_capturing = KEYBIND_COUNT;
		} else if (vk == VK_BACK || vk == VK_DELETE) {
			keybind_keys[g_capturing] = 0;
			g_capturing = KEYBIND_COUNT;
		} else if (vk != 0) {
			keybind_keys[g_capturing] = vk;
			g_capturing = KEYBIND_COUNT;
		}
		/* Swallow everything else this frame: the key you're binding
		 * shouldn't also fire whatever it used to be bound to. */
		for (int vk2 = 0; vk2 < 256; vk2++) {
			g_was_down[vk2] = now_down[vk2];
		}
		return;
	}

	if (accept_input) {
		for (int i = 0; i < KEYBIND_COUNT; i++) {
			int vk = keybind_keys[i];
			if (vk > 0 && vk < 256 && now_down[vk] && !g_was_down[vk]) {
				g_pressed[i] = true;
			}
		}
	}

	/* Recorded even when input isn't accepted, so a key held down while a
	 * text field had focus doesn't fire the moment focus is lost. */
	for (int vk = 0; vk < 256; vk++) {
		g_was_down[vk] = now_down[vk];
	}
}

bool keybind_pressed(keybind_id id) {
	if (id < 0 || id >= KEYBIND_COUNT) return false;
	return g_pressed[id];
}

void keybind_begin_capture(keybind_id id) {
	if (id < 0 || id >= KEYBIND_COUNT) return;
	g_capturing = id;
}

keybind_id keybind_capturing(void) {
	return g_capturing;
}

void keybind_cancel_capture(void) {
	g_capturing = KEYBIND_COUNT;
}

void keybind_key_name(int vk, char *out, int size) {
	if (!out || size <= 0) return;
	out[0] = '\0';
	if (size < 2) return;

	if (vk <= 0 || vk > 255) {
		lstrcpynA(out, "None", size);
		return;
	}

	/* GetKeyNameTextA wants a scancode in bits 16-23, and bit 24 set for the
	 * extended keys -- without it Insert, Delete, Home and the arrows come
	 * back named after their numpad twins. */
	UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
	LONG param = (LONG)(scan << 16);
	switch (vk) {
	case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
	case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
	case VK_UP:     case VK_DOWN:   case VK_DIVIDE: case VK_NUMLOCK:
		param |= (1 << 24);
		break;
	default:
		break;
	}

	if (scan == 0 || GetKeyNameTextA(param, out, size) == 0) {
		/* Nothing sensible from Windows -- fall back to the raw code rather
		 * than showing an empty button. */
		wsprintfA(out, "Key %d", vk);
	}
}
