#ifndef GRANNY_AI_H
#define GRANNY_AI_H

#include <stdbool.h>

#include "game/ai_granny_hook.h"
#include "game/offsets.h"

/* Needed because the menu (d3d11_hook.cpp) is C++ -- without this the
 * declarations get C++ linkage there and won't match the definitions
 * compiled as C. */
#ifdef __cplusplus
extern "C" {
#endif

// self explainatory functions
void change_granny_walk_speed(float speed);
void change_granny_run_speed(float speed);

/** Bound to the Granny tab's speed controls. */
extern bool granny_speed_enabled;
extern float granny_walk_speed;
extern float granny_run_speed;

/**
 * @brief Apply the speed override when it needs applying, and restore her
 * original speeds when it's switched off.
 *
 * Call every tick from the game's main thread (the AI_Granny::FixedUpdate
 * hook), but it only writes on the two occasions that matter:
 *
 *   - `instance` differs from last tick, meaning the game rebuilt her on a
 *     level load or respawn and her speeds are back to the difficulty's
 *   - a menu control changed, flagged by granny_speed_mark_dirty()
 *
 * Every other tick returns immediately. Writing once per change rather than
 * continuously also means we aren't fighting the game's own logic, which is
 * what makes blind cancel the player's pepper spray.
 *
 * The originals are captured before the first write to each instance, so
 * switching the feature off restores what the game actually chose rather
 * than a hardcoded guess -- Hollow doesn't use the same values as Normal.
 * That capture also seeds the sliders, so they open showing her real speeds.
 *
 * @param instance The live AI_Granny, straight from the FixedUpdate hook.
 */
void granny_ai_tick(void *instance);

/** Tell the next tick that a menu control changed and needs applying. */
void granny_speed_mark_dirty(void);

#ifdef __cplusplus
}
#endif

#endif // GRANNY_AI_H
