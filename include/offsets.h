#pragma once
#include <stdint.h>

/**
 * @file offsets.h
 * @brief IL2CPP method RVAs for Granny, as dumped (e.g. via Il2CppDumper).
 *
 * These are offsets from **GameAssembly.dll's** base, NOT the main exe's —
 * IL2CPP compiles all game/script code into GameAssembly.dll, and the exe
 * itself is just the Unity bootstrap. To get a real, callable/patchable
 * address at runtime:
 *
 * @code
 * uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
 * uintptr_t addr = base + OFFSET_PlayerStatus_GrannyCaughtYou;
 * @endcode
 *
 * These offsets are tied to one specific build of the game — if the game
 * updates, GameAssembly.dll is rebuilt and every offset here can shift.
 * They'd need to be re-dumped after any game update.
 *
 * Every method in this file has been confirmed via IDA/Hex-Rays against
 * this exact GameAssembly.dll to share one signature:
 * `void __fastcall f(void *__this)` -- the instance pointer is the sole
 * argument (IL2CPP elided the trailing MethodInfo* since none of these are
 * generic/virtual-dispatch calls). One typedef, granny_method_t, covers all
 * of them; per-function aliases below just make call sites self-documenting.
 * Cast an address to the relevant alias to call or hook it:
 *
 * @code
 * PlayerStatus_GrannyCaughtYou_t fn =
 *     (PlayerStatus_GrannyCaughtYou_t)(base + OFFSET_PlayerStatus_GrannyCaughtYou);
 * fn(player_status_instance);
 * @endcode
 */
typedef void (__fastcall *granny_method_t)(void *instance);

/** PlayerStatus::GrannyCaughtYou — runs when Granny catches you standing/moving. */
static const uintptr_t OFFSET_PlayerStatus_GrannyCaughtYou    = 0x24A9A0;
typedef granny_method_t PlayerStatus_GrannyCaughtYou_t;

/** PlayerStatus::GrannyCaughtYouBed — runs when Granny catches you hiding under a bed. */
static const uintptr_t OFFSET_PlayerStatus_GrannyCaughtYouBed = 0x24A8C0;
typedef granny_method_t PlayerStatus_GrannyCaughtYouBed_t;

/** PlayerStatus::KnockDeath — death by the knockout/trap variant. */
static const uintptr_t OFFSET_PlayerStatus_KnockDeath         = 0x24AA20;
typedef granny_method_t PlayerStatus_KnockDeath_t;

/** PlayerStatus::NormalDeath — the generic/default death path. */
static const uintptr_t OFFSET_PlayerStatus_NormalDeath        = 0x24B350;
typedef granny_method_t PlayerStatus_NormalDeath_t;

/**
 * AI_Granny::ResetAIDecision — 0x8f bytes. Tears down/nulls several
 * component references on the instance (calls into what look like
 * SetActive(false)/Stop()-style engine calls at offsets +168, +48/+40,
 * +264), NOT a "reset to patrol" as the name suggests. Needs more digging
 * before relying on its exact effect.
 */
static const uintptr_t OFFSET_AI_Granny_ResetAIDecision       = 0x1C1930;
typedef granny_method_t AI_Granny_ResetAIDecision_t;

/**
 * AI_Granny::StopAI — 0x12b bytes. Same shape as ResetAIDecision but touches
 * more fields (+216, +512/+520, +176/+184, +168, +48/+40, +264, +760, +344)
 * and sets byte +392 to 1 instead of 0 -- looks like the more thorough
 * teardown of the two.
 */
static const uintptr_t OFFSET_AI_Granny_StopAI                = 0x1C2100;
typedef granny_method_t AI_Granny_StopAI_t;

/**
 * AI_Granny::ChaseAction — only 0x21 bytes. Just four raw field writes
 * (dword +396 = 256, qword +412 = 0, dword +640 = 0, byte +394 = 0) and
 * returns. This is a state-enter/reset stub, NOT the per-tick chase/pursuit
 * logic -- that lives in FixedUpdate below.
 */
static const uintptr_t OFFSET_AI_Granny_ChaseAction           = 0x1BDA40;
typedef granny_method_t AI_Granny_ChaseAction_t;

/**
 * AI_Granny::SmackTimer — 0x62 bytes. Lazily fetches a static/singleton
 * object, constructs or fetches something via it, stashes `__this` at
 * +32 of the result, and returns that object -- reads like it's
 * creating/retrieving a timer/coroutine handle rather than ticking one.
 */
static const uintptr_t OFFSET_AI_Granny_SmackTimer            = 0x1C1DE0;
typedef granny_method_t AI_Granny_SmackTimer_t;

/**
 * AI_Granny::FixedUpdate — 0x366d bytes, 2917 instructions, 81 callees.
 * This is the real per-physics-tick driver (everything above it is a small
 * helper by comparison). Runs continuously for as long as an AI_Granny
 * instance is alive, which makes it the right function to hook (not
 * no-op-patch) purely to capture the current live instance pointer every
 * tick -- see ai_granny_hook.h.
 */
static const uintptr_t OFFSET_AI_Granny_FixedUpdate           = 0x1BDCC0;
typedef granny_method_t AI_Granny_FixedUpdate_t;
