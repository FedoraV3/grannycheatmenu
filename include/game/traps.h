#ifndef TRAPS_H
#define TRAPS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Desired trap state, bound to the World tab's checkbox. Lives here rather
 *  than in the menu so the config can save it and re-apply it on load. */
extern bool traps_disabled;

/**
 * @brief Push `traps_disabled` into the game.
 *
 * Reverts the flag if the patch fails, so the menu never shows a state the
 * game isn't actually in.
 *
 * @return true if the level is now in the requested state.
 */
bool traps_apply(void);

/**
 * @brief Suppress or restore every trap in the level.
 *
 * Byte patches `ret` over the four trap `OnTriggerEnter` methods, the same
 * technique the death functions use and for the same reason: nothing needs
 * to run in their place, so a detour would only add a trampoline. The traps
 * still exist and still animate -- they just stop reacting to the player
 * walking into them.
 *
 * There is no shared base class or global toggle behind the game's traps, so
 * each entry point is patched separately: TrapTrigger (the generic one),
 * BearTrapLogic, ExploTrapTrigger and TrapPoison.
 *
 * Safe to call from the menu thread -- it only writes to code pages, never
 * into IL2CPP.
 *
 * Idempotent, and all-or-nothing: if any one patch fails the others are
 * rolled back, so the level can't end up with half its traps disabled.
 *
 * @param disabled true to suppress the traps, false to restore them.
 * @return true if the level is now in the requested state.
 */
bool traps_set_disabled(bool disabled);

#ifdef __cplusplus
}
#endif

#endif /* TRAPS_H */
