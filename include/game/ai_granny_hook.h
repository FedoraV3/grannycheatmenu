#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * Bound to the Player tab's "Immortality" checkbox.
 *
 * Named for what it does rather than for Granny: the byte patch lands on
 * PlayerStatus::NormalDeath and ::KnockDeath, which are the game's generic
 * death paths. Confirmed in game by surviving the third-floor fake-floor
 * trap, which kills by fall damage with Granny nowhere near.
 *
 * Lives here rather than in the menu so the config can save it and
 * granny_apply_immortality() can be re-run on load.
 */
extern bool immortality;

/**
 * @brief Push `immortality` into the game.
 *
 * Enables or disables the two catch detours and applies or lifts the death
 * byte patch. Reverts the flag if the patch fails, so the menu never shows a
 * state the game isn't in.
 *
 * Must not run before ai_granny_hook_install() has created the hooks.
 *
 * @return true if the game is now in the requested state.
 */
bool granny_apply_immortality(void);

extern bool granny_is_blind;

/**
 * Bound to the Granny tab's "Deaf" checkbox.
 *
 * There is no IsDeaf flag to pair with IsBlind, so deafness is synthesised:
 * every tick the hook clears the two fields that carry a heard noise
 * (IsFollowingSound and NoiseObj) plus the proximity timer, before her
 * FixedUpdate gets a chance to act on them. She still gets given noises by
 * the game's trigger volumes -- she just never has one to walk towards.
 */
extern bool granny_is_deaf;

/**
 * @brief Disable or restore PlayerStatus::NormalDeath and ::KnockDeath by
 * byte patch.
 *
 * Both are pure suppression -- nothing needs to run in their place -- so
 * writing `ret` (0xC3) over the entry point replaces what used to be two
 * no-op MinHook detours, with no trampoline and no per-call cost. The
 * original bytes are saved on patch and written back on restore.
 *
 * `ret` rather than NOP because these are `void f(void *this)` under the
 * Win64 ABI: the caller cleans the stack and nothing is pushed at entry, so
 * returning immediately is safe, whereas a NOP would just fall through into
 * the rest of the function.
 *
 * Idempotent -- calling it twice with the same value is a no-op, which
 * matters because a second patch would record the `ret` itself as the
 * "original" byte and make the damage permanent.
 *
 * @param disabled true to patch the death paths out, false to restore them.
 * @return true on success; false if the module base isn't resolved yet or a
 *         write failed (in which case neither function is left patched).
 */
bool granny_set_death_disabled(bool disabled);

/**
 * @brief Call AI_Granny::StopAI on the live instance, halting her for good.
 *
 * One-shot and irreversible: StopAI tears her components down and no
 * function has been found that undoes it. She only comes back when the game
 * builds a fresh AI_Granny -- on respawn into a new day, or a restart.
 *
 * @return true if she was stopped; false if no instance is currently
 *         ticking (she isn't spawned, or is disabled in the game options).
 */
bool granny_stop_ai(void);

/**
 * @brief Whether the instance we stopped is still the one in play.
 *
 * Goes false by itself once the game swaps in a new AI_Granny, since that
 * instance was never stopped -- which is how a respawn or restart clears
 * the state without us needing to detect either event directly.
 */
bool granny_is_stopped(void);

/**
 * @brief Hook AI_Granny::FixedUpdate to keep a live pointer to the current
 * AI_Granny instance.
 *
 * FixedUpdate runs every physics tick for as long as an AI_Granny instance
 * is alive in the scene, and (confirmed via IDA against this build's
 * GameAssembly.dll) IL2CPP compiled it as `void __fastcall f(void *__this)`
 * -- the instance is the sole argument, no trailing MethodInfo*. Hooking it
 * (rather than patching it out) means the game keeps ticking her normally,
 * while ai_granny_current() gets refreshed dozens of times a second as a
 * side effect. That makes it self-healing across level reloads: IL2CPP
 * allocates a new AI_Granny object on the GC heap each time the level
 * (re)loads, at a different address, but the moment that new instance's
 * FixedUpdate starts ticking, ai_granny_current() reflects it automatically
 * -- no manual re-scan or address comparison needed anywhere else.
 *
 * Must be called after MinHook has been initialized (e.g. after
 * d3d11_hook_install(), which calls MH_Initialize()) and after
 * GameAssembly.dll is confirmed loaded.
 *
 * @return Nonzero on success. Zero if GameAssembly.dll isn't loaded yet or
 *         the hook couldn't be installed -- check OutputDebugStringA output.
 */
int ai_granny_hook_install(void);

/**
 * @brief The most recently observed AI_Granny instance.
 *
 * Backed by whatever FixedUpdate call landed last -- always the currently
 * ticking instance, or NULL if none has ticked yet (e.g. before the level
 * with Granny in it has loaded). Re-read this each time you need it rather
 * than caching the result, since the underlying instance can be replaced
 * out from under you at any level reload.
 *
 * @return The current AI_Granny instance pointer, or NULL.
 */
void *ai_granny_current(void);

#ifdef __cplusplus
}
#endif
