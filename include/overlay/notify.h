#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Where the toasts stack. Stored as an int rather than the enum so the
 * config table can point a CFG_INT at it.
 */
enum {
	NOTIFY_TOP_RIGHT = 0,
	NOTIFY_TOP_LEFT,
	NOTIFY_TOP_CENTER,
	NOTIFY_BOTTOM_RIGHT,
	NOTIFY_BOTTOM_LEFT,
	NOTIFY_POSITION_COUNT
};

/** Bound to the Visuals tab's notification controls. */
extern bool notify_enabled;
extern int notify_position;
extern float notify_duration;   /**< Seconds a toast stays up, fade included. */

/** Display name for a position, for the menu's combo. */
const char *notify_position_name(int position);

/**
 * @brief Initialise the notification lock.
 *
 * Call once before any hook that could push a toast. Same reasoning as
 * esp_init(): the list is written from whichever thread noticed the change
 * and read by the present thread, so the lock has to exist first.
 */
void notify_init(void);

/**
 * @brief Post a toast.
 *
 * Safe from any thread. The text is copied, so the caller's buffer can go
 * away immediately.
 */
void notify_push(const char *text);

/**
 * @brief Post "<name>: ON" or "<name>: OFF", coloured to match.
 *
 * The common case, and the reason this exists -- a keybind changes something
 * you often can't see from where you're standing.
 */
void notify_toggle(const char *name, bool on);

/** @brief Post a toast in the warning colour, for a change that didn't take. */
void notify_warn(const char *text);

/**
 * @brief Whether any toast is still on screen.
 *
 * The overlay skips its whole ImGui frame when there is nothing to draw, and
 * "nothing to draw" has to include a pending toast -- otherwise the one case
 * you most need feedback in, every feature off, is the one case it never
 * appears.
 */
bool notify_active(void);

/**
 * @brief Draw the toasts. Present thread, between NewFrame and Render.
 */
void notify_render(void);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_H */
