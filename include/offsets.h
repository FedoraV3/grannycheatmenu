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

/**
 * ItemSpawn::Update -- per-frame tick on the object that owns every item
 * GameObject in the level. Hooked purely to capture the ItemSpawn instance,
 * same trick as AI_Granny::FixedUpdate. Game-assembly method, so single
 * argument, no trailing MethodInfo*.
 */
static const uintptr_t OFFSET_ItemSpawn_Update                = 0x21F2C0;
typedef granny_method_t ItemSpawn_Update_t;

/*
 * ItemSpawn's item pointers: 55 GameObject* fields laid out contiguously
 * from `crossbow` at 0x28 through `fuse` at 0x1D8, 8 bytes apart, so they
 * can be walked as an array rather than named one by one. Names for each
 * slot live in esp.cpp.
 */
#define ITEMSPAWN_FIRST_ITEM_FIELD 0x28
#define ITEMSPAWN_ITEM_COUNT       55

/*
 * AI_Granny *instance field* offsets (from Il2CppDumper's dump.cs). These
 * are offsets into the object -- add them to an instance pointer from
 * ai_granny_current(), NOT to the module base like the RVAs above.
 */
static const uintptr_t FIELD_AI_Granny_Walk_Speed         = 0x94;  /**< float */
static const uintptr_t FIELD_AI_Granny_Run_Speed          = 0x98;  /**< float */
static const uintptr_t FIELD_AI_Granny_Agent              = 0xA8;  /**< NavMeshAgent* */
static const uintptr_t FIELD_AI_Granny_Player             = 0xD0;  /**< Transform* */
static const uintptr_t FIELD_AI_Granny_PlayerStatus       = 0xE8;  /**< PlayerStatus* */
static const uintptr_t FIELD_AI_Granny_IsDying            = 0x138; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsBlind            = 0x164; /**< bool -- the game's own blind flag */
static const uintptr_t FIELD_AI_Granny_BlindTimer         = 0x168; /**< float */
static const uintptr_t FIELD_AI_Granny_CaughtPlayer       = 0x188; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsSearching        = 0x18A; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsAngry            = 0x18B; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsFollowingSound   = 0x18C; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsChasing          = 0x18D; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsWalking          = 0x18E; /**< bool */
static const uintptr_t FIELD_AI_Granny_IsIdle             = 0x18F; /**< bool */
static const uintptr_t FIELD_AI_Granny_DistanceFromPlayer = 0x1BC; /**< float */
static const uintptr_t FIELD_AI_Granny_PlayerPos          = 0x288; /**< Vector3 */

/*
 * PlayerStatus instance fields.
 */
/**
 * PlayerStatus::PlayerCam -- the player's Camera. Reaching it as
 * AI_Granny(+0xE8) -> PlayerStatus(+0x110) is more reliable than
 * Camera.main, which returns NULL unless the game tags its camera
 * "MainCamera", and needs no IL2CPP call at all.
 */
static const uintptr_t FIELD_PlayerStatus_PlayerCam       = 0x110; /**< Camera* */

/*
 * UnityEngine methods -- IL2CPP compiles the engine's own assemblies into
 * GameAssembly.dll too, so these live at fixed RVAs just like the game's.
 *
 * Unlike the game methods above, these DO take a trailing MethodInfo*.
 * Passing NULL for it is safe: AI_Granny::StopAI itself calls both
 * Component::get_transform and Transform::get_position that way (visible
 * in its decompile as `sub_18071D550(a1, 0)` / `sub_180747310(buf, v5, 0)`).
 *
 * Win64 ABI detail: a struct larger than 8 bytes is returned through a
 * hidden first pointer argument, so for Vector3 (12 bytes) and Matrix4x4
 * (64 bytes) the return buffer comes first and `this` shifts to the second
 * parameter. Unity's Matrix4x4 is stored column-major: element (row, col)
 * is raw[col * 4 + row].
 */

/** Component::get_transform -- `Transform *f(Component *this, MethodInfo *)`. */
static const uintptr_t OFFSET_Component_get_transform         = 0x71D550;
typedef void *(__fastcall *Component_get_transform_t)(void *instance, void *method);

/** Transform::get_position -- `Vector3 *f(Vector3 *ret, Transform *this, MethodInfo *)`. */
static const uintptr_t OFFSET_Transform_get_position          = 0x747310;
typedef void *(__fastcall *Transform_get_position_t)(void *ret_vector3, void *instance, void *method);

/** Camera::get_main -- static, `Camera *f(MethodInfo *)`. */
static const uintptr_t OFFSET_Camera_get_main                 = 0x6FE450;
typedef void *(__fastcall *Camera_get_main_t)(void *method);

/** Camera::get_worldToCameraMatrix -- `Matrix4x4 *f(Matrix4x4 *ret, Camera *this, MethodInfo *)`. */
static const uintptr_t OFFSET_Camera_get_worldToCameraMatrix  = 0x6FEA00;
/** Camera::get_projectionMatrix -- same shape as the above. */
static const uintptr_t OFFSET_Camera_get_projectionMatrix     = 0x6FE6B0;
typedef void *(__fastcall *Camera_get_matrix_t)(void *ret_matrix4x4, void *instance, void *method);

/**
 * UnityEngine.Object::m_CachedPtr -- the native object behind the managed
 * wrapper.
 *
 * Unity zeroes this on Destroy() but keeps the managed object alive, so a
 * destroyed GameObject still reads as a perfectly valid non-NULL pointer
 * from C. Unity's own == operator hides that ("fake null"); we can't, so
 * check this before calling ANY method on a UnityEngine.Object or the call
 * dereferences a freed native object and takes the game down.
 */
static const uintptr_t FIELD_UnityObject_m_CachedPtr          = 0x10;

/** GameObject::get_transform -- GameObject isn't a Component, so it has its own. */
static const uintptr_t OFFSET_GameObject_get_transform        = 0x720600;

/**
 * GameObject::get_activeInHierarchy -- `bool f(GameObject *this, MethodInfo *)`.
 * Used to skip items that have already been collected (the game deactivates
 * their GameObject) so the ESP only shows what's still out there.
 */
static const uintptr_t OFFSET_GameObject_get_activeInHierarchy = 0x720460;
typedef bool(__fastcall *GameObject_get_active_t)(void *instance, void *method);
