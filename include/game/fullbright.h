#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** Bound to the "Fullbright" checkbox in the Visuals tab. */
extern bool fullbright_enabled;

/**
 * @brief Apply or undo the fullbright lighting override.
 *
 * MUST be called from the game's main thread -- it calls into
 * UnityEngine.RenderSettings, and IL2CPP calls from the D3D11 present thread
 * (where the menu's checkbox is actually clicked) can crash. So the checkbox
 * only sets the flag and this does the work on the next game tick.
 *
 * Loading a level resets scene lighting, so the override has to be put back.
 * Watching ambientMode for drift wasn't enough in practice -- fullbright was
 * lost across restarts while Granny's speed override survived, the
 * difference being that speed keys off a new instance appearing. So this
 * takes the same signal: a different `player` pointer means the game rebuilt
 * the scene, and the lighting is re-applied unconditionally.
 *
 * Cheap on every other tick -- a pointer compare and one getter.
 *
 * @param player The live PickRay, straight from its Update hook. May be NULL.
 */
void fullbright_tick(void *player);

#ifdef __cplusplus
}
#endif
