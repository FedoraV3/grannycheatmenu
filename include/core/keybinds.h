#ifndef KEYBINDS_H
#define KEYBINDS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every action that can be bound to a key.
 *
 * Deliberately not everything in the menu: a slider has nothing to toggle,
 * and the ESP's debug switches aren't worth a key. What's here is what you'd
 * actually want to hit mid-run without opening the menu -- the things whose
 * value depends on flipping them at a particular moment.
 *
 * The order is the order they appear in the Keybinds tab.
 */
typedef enum {
	KEYBIND_MENU,
	KEYBIND_GRANNY_ESP,
	KEYBIND_ITEM_ESP,
	KEYBIND_MOMSPIDER_ESP,
	KEYBIND_ARROWS,
	KEYBIND_FULLBRIGHT,
	KEYBIND_IMMORTALITY,
	KEYBIND_NOCLIP,
	/* Held, not tapped: these two are read straight off the keyboard by the
	 * noclip mover on the game thread, so they never appear in the toggle
	 * dispatch. They sit in this table only so they can be rebound like
	 * everything else. */
	KEYBIND_NOCLIP_UP,
	KEYBIND_NOCLIP_DOWN,
	KEYBIND_MOVE_SPEED,
	KEYBIND_BLIND,
	KEYBIND_DEAF,
	KEYBIND_FREEZE,
	KEYBIND_GRANNY_SPEED,
	KEYBIND_STOP_GRANNY,
	KEYBIND_DISABLE_TRAPS,
	KEYBIND_SPAWN_ITEM,
	KEYBIND_SPAWN_ALL,
	KEYBIND_COUNT
} keybind_id;

/** Virtual-key code bound to each action, or 0 for unbound. The Keybinds tab
 *  edits these directly. */
extern int keybind_keys[KEYBIND_COUNT];

/** Display name for an action, e.g. "Toggle menu". */
const char *keybind_name(keybind_id id);

/**
 * @brief Stable slug for an action, e.g. "stop_granny".
 *
 * What the config file keys on. Deliberately not the display name and
 * deliberately not the enum's position: bindings have to survive both a
 * label being reworded and an action being inserted into the middle of the
 * list, which is exactly what happened when noclip's rise and descend keys
 * were added between Noclip and Move speed.
 */
const char *keybind_config_name(keybind_id id);

/**
 * @brief Sample the keyboard once for this frame.
 *
 * Call exactly once per frame, before any keybind_pressed() query, from the
 * thread that owns the overlay. Everything else here reads the snapshot it
 * takes, so all the queries in a frame agree with each other.
 *
 * @param accept_input false while something else owns the keyboard (an ImGui
 *        text field, say), which reports every action as unpressed for this
 *        frame without losing track of what's held down -- so releasing the
 *        key afterwards doesn't register as a fresh press.
 */
void keybinds_update(bool accept_input);

/**
 * @brief Whether this action's key went down this frame.
 *
 * Rising edge only, so holding a key toggles once rather than every frame.
 */
bool keybind_pressed(keybind_id id);

/**
 * @brief Start listening for the next key, to bind it to `id`.
 *
 * The next keybinds_update() that sees a key go down assigns it and stops
 * capturing. Escape cancels, Backspace and Delete clear the binding.
 * Starting a capture while another is running replaces it.
 */
void keybind_begin_capture(keybind_id id);

/** Which action is currently being rebound, or KEYBIND_COUNT if none. */
keybind_id keybind_capturing(void);

/** Abandon a capture without changing anything. */
void keybind_cancel_capture(void);

/**
 * @brief Human-readable name for a virtual-key code, e.g. "F4", "Insert".
 *
 * @param vk   The key, or 0 for unbound.
 * @param out  Receives the name; always NUL-terminated, never empty.
 * @param size Size of `out` in bytes. 32 is plenty.
 */
void keybind_key_name(int vk, char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* KEYBINDS_H */
