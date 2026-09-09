#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>

/* The menu is C++, so these need C linkage to match the definitions. */
#ifdef __cplusplus
extern "C" {
#endif

/** Bound to the Player tab's "Move speed" control. */
extern bool player_speed_enabled;
/** Multiplier on the game's own speeds, not an absolute value -- the game
 *  swaps between a standing and a crouched speed, and a multiplier keeps
 *  that difference intact instead of flattening both to one number. */
extern float player_speed_multiplier;

/** Bound to the Player tab's "Noclip" checkbox. */
extern bool player_noclip_enabled;

/**
 * Bound to the Player tab's "No hard landing" checkbox.
 *
 * Removes the stagger after a big drop -- the animation that pins you in
 * place while your character picks themselves up, which
 * FallingHolder::HandleLanding drives by setting isAllowedToMove to false.
 *
 * Rather than suppressing that function, this keeps fallDuration from ever
 * crossing FallDurationHold, so the game classifies every survivable drop as
 * an ordinary landing and the recovery is never scheduled in the first
 * place. Patching HandleLanding out instead would leave fallDuration stuck
 * above the threshold for the rest of the session, since resetting it is one
 * of the things that function does.
 *
 * Side effect: with the counter capped it also never reaches FallMega, so
 * fatal falls stop being fatal. That is the same threshold, one step higher.
 */
extern bool player_no_hard_landing;

/**
 * @brief Hook MobileFPS::Update, which drives both features on this tab.
 *
 * MinHook must already be initialized and GameAssembly.dll loaded. Same
 * capture-the-instance pattern as AI_Granny::FixedUpdate: the player object
 * is rebuilt on every level load, so it has to be re-read rather than
 * latched once.
 *
 * @return Nonzero on success.
 */
int player_hook_install(void);

/** The live MobileFPS, or NULL if none has ticked yet. */
void *player_current(void);

/** Tell the next tick that a menu control changed and needs applying. */
void player_mark_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_H */
