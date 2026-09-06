#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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
