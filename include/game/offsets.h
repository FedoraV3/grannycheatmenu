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

/**
 * ItemSpawn::CountItem -- selects which of the 55 fields this dropper will
 * spawn, as a 1-based float (1.0 = crossbow ... 20.0 = melon ... 55.0 =
 * fuse). PickRay::CheckItemDropping writes it right after instantiating the
 * ItemDrop prefab.
 *
 * Reading this inside a hook on ItemSpawn::Update -- while the spawner is
 * still alive -- is how a dropped item gets tracked. The GameObject it
 * points to outlives the spawner, so keeping THAT pointer is safe even
 * though keeping the spawner's was not.
 */
static const uintptr_t FIELD_ItemSpawn_CountItem              = 0x24;

/**
 * ItemRepositionSeed::Awake -- hooked to capture the object that actually
 * holds the level's items.
 *
 * Unlike ItemSpawn this is a persistent level fixture (it also has an
 * OnTriggerEnter), so the pointer stays usable rather than destroying
 * itself mid-frame. Awake fires once per level load, so the capture
 * self-heals across reloads. Game-assembly method: single argument, no
 * trailing MethodInfo*.
 */
static const uintptr_t OFFSET_ItemRepositionSeed_Awake        = 0x245DC0;
typedef granny_method_t ItemRepositionSeed_Awake_t;

/**
 * PickRay::Update -- the player's interaction script (pickup raycast, drop,
 * shoot), hooked as the ESP's primary per-frame main-thread tick.
 *
 * This exists whenever the player does, which matters because Granny can be
 * switched off in the game's own options -- AI_Granny::FixedUpdate then
 * never runs, and anything driven off it silently stops. PickRay also
 * carries PlayerStatus, giving a route to the camera that doesn't go
 * through Granny at all.
 *
 * Huge function (9444 instructions) but we only detour its entry.
 * Game-assembly method: single argument, no trailing MethodInfo*.
 */
static const uintptr_t OFFSET_PickRay_Update                  = 0x237CE0;
typedef granny_method_t PickRay_Update_t;

/** PickRay::PlayerStatus -- the Granny-independent path to PlayerCam. */
static const uintptr_t FIELD_PickRay_PlayerStatus             = 0x88;

/*
 * ItemRepositionSeed's item references: 35 Transform* fields laid out
 * contiguously from `Pliers` at 0x28 through `Fuse` at 0x138, 8 bytes
 * apart. These are Transforms rather than GameObjects, so reading a
 * position is a single Transform::get_position call with no GetComponent
 * step. Slot names live in esp.cpp in this same order.
 */
#define ITEMSEED_FIRST_ITEM_FIELD 0x28
#define ITEMSEED_ITEM_COUNT       35

/*
 * ItemSeedData -- a per-item MonoBehaviour carrying the item's own name and
 * the developers' category. This is the complete item source: every item in
 * the level has one, whereas ItemRepositionSeed only covers the 35 it
 * repositions and ItemSpawn's fields belong to the dropper.
 *
 * Hooking .ctor and OnDestroy gives a self-maintaining registry -- items add
 * themselves when built and remove themselves when picked up.
 *
 * Read the fields at collect time, NOT inside .ctor: Unity assigns
 * serialized fields after the constructor runs, so itemName is still null
 * there.
 */
static const uintptr_t OFFSET_ItemSeedData_ctor               = 0x247230;
static const uintptr_t OFFSET_ItemSeedData_OnDestroy          = 0x2471A0;
typedef granny_method_t ItemSeedData_ctor_t;
typedef granny_method_t ItemSeedData_OnDestroy_t;

/**
 * GameObject::GetComponent<T>, the fully-shared-generic form.
 *
 * Convention confirmed from ItemSpawn::Update's call site rather than
 * inferred: rcx = the GameObject, rdx = an out buffer that receives the
 * component pointer, r8 = the baked MethodInfo for the instantiation. IDA
 * labels the first two "retstr" and "this", which is misleading.
 *
 *   void *component = NULL;
 *   get_component(game_object, &component, *(void **)(base + METHODINFO_...));
 *
 * Needed because scene-placed items never enter the ItemSeedData registry --
 * Unity deserializes their MonoBehaviours without running the managed
 * .ctor we hook -- so their category has to be fetched directly.
 */
static const uintptr_t OFFSET_GameObject_GetComponent_shared  = 0x2B7FD0;

/**
 * The baked MethodInfo* for GetComponent<ItemSeedData>. This global holds a
 * POINTER to the MethodInfo, so dereference it before passing it on.
 */
static const uintptr_t METHODINFO_GetComponent_ItemSeedData   = 0xC338B8;
typedef void(__fastcall *GameObject_GetComponent_t)(void *object, void *out_component, void *method);

/*
 * UnityEngine.RenderSettings -- static scene lighting, used for fullbright.
 *
 * Only fog and ambientMode have getters, so those are the only originals
 * that can be saved for a restore. That's enough: with the mode put back to
 * whatever it was (Skybox, normally) the ambient colour and intensity stop
 * being used, so not restoring them changes nothing visible.
 *
 * These are static UnityEngine methods, so each takes its value plus a
 * trailing MethodInfo*, and NULL is accepted for it.
 */
static const uintptr_t OFFSET_RenderSettings_get_fog           = 0x712200;
static const uintptr_t OFFSET_RenderSettings_set_fog           = 0x7123F0;
static const uintptr_t OFFSET_RenderSettings_get_ambientMode   = 0x712020;
static const uintptr_t OFFSET_RenderSettings_set_ambientMode   = 0x7122F0;
static const uintptr_t OFFSET_RenderSettings_set_ambientIntensity = 0x712230;
static const uintptr_t OFFSET_RenderSettings_set_ambientLight  = 0x7122B0;

/**
 * AmbientMode. Note Flat is 3, NOT 2 -- the enum is
 * Skybox=0, Trilight=1, Flat=3, Custom=4, with 2 unused. Passing 0 would
 * select Skybox and appear to do nothing.
 */
#define UNITY_AMBIENT_MODE_FLAT 3

/** UnityEngine.Color -- four floats. */
typedef struct {
	float r, g, b, a;
} unity_color;

typedef bool(__fastcall *RenderSettings_get_fog_t)(void *method);
typedef void(__fastcall *RenderSettings_set_fog_t)(bool value, void *method);
typedef int(__fastcall *RenderSettings_get_ambientMode_t)(void *method);
typedef void(__fastcall *RenderSettings_set_ambientMode_t)(int value, void *method);
typedef void(__fastcall *RenderSettings_set_float_t)(float value, void *method);
/* Color is 16 bytes, so the Win64 ABI passes it by reference, not by value. */
typedef void(__fastcall *RenderSettings_set_color_t)(const unity_color *value, void *method);

/**
 * UnityEngine.Object::FindObjectsOfType(Type, bool includeInactive).
 *
 * The non-generic overload, so it can be called with a System.Type built at
 * runtime instead of needing a baked generic MethodInfo. This is the only
 * COMPLETE item source: ItemRepositionSeed knows just the 35 it repositions
 * and the ItemSeedData registry only catches Instantiate()d objects, since
 * Unity deserializes scene-placed MonoBehaviours without running the managed
 * .ctor we hook.
 *
 * includeInactive = false also drops the deactivated preset placeholders for
 * free, which previously needed a separate activeInHierarchy call.
 *
 * Expensive (it scans every object), so call it on a timer and re-read
 * positions from the cached components each frame.
 */
static const uintptr_t OFFSET_Object_FindObjectsOfType        = 0x725FF0;
typedef void *(__fastcall *Object_FindObjectsOfType_t)(void *system_type, bool include_inactive, void *method);

/*
 * Il2CppArray layout: klass at 0, monitor at 8, bounds at 0x10, max_length
 * at 0x18, and the elements from 0x20.
 */
#define IL2CPP_ARRAY_LENGTH_OFFSET 0x18
#define IL2CPP_ARRAY_ELEMENTS_OFFSET 0x20

static const uintptr_t FIELD_ItemSeedData_itemName            = 0x20; /**< System.String* */
/**
 * ItemSeedData::category, straight from the dev tooltip:
 * "1=escape-only, 2=escape+puzzle, 3=puzzle-only, 3=free" -- the last is
 * evidently a typo for 4, so treat anything outside 1..3 as free/misc.
 */
static const uintptr_t FIELD_ItemSeedData_category            = 0x28; /**< int */

/*
 * System.String layout under IL2CPP: klass at 0, monitor at 8, length as an
 * int32 at 0x10, then UTF-16 characters from 0x14.
 */
#define IL2CPP_STRING_LENGTH_OFFSET 0x10
#define IL2CPP_STRING_CHARS_OFFSET  0x14

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
static const uintptr_t FIELD_AI_Granny_EnemyVision        = 0x108; /**< Eyes_Granny* -- her sight component */
static const uintptr_t FIELD_AI_Granny_IsBlind            = 0x164; /**< bool -- the game's own blind flag */
static const uintptr_t FIELD_AI_Granny_BlindTimer         = 0x168; /**< float -- counts the blind state down */

/*
 * Hearing. Note there is NO IsDeaf flag to match IsBlind -- deafness has to
 * be synthesised from these (clear IsFollowingSound/NoiseObj each tick, pin
 * the timers, or intercept whatever sets them). MurderNoiseObjs (0x1C1510)
 * works on the same tag system and is worth reading first.
 */
static const uintptr_t FIELD_AI_Granny_TimerNearNoise     = 0x190; /**< float */
static const uintptr_t FIELD_AI_Granny_TimerMaxNoise      = 0x194; /**< float */
static const uintptr_t FIELD_AI_Granny_NoiseObj           = 0x1E8; /**< GameObject* -- what she's heading toward */
static const uintptr_t FIELD_AI_Granny_NoiseObjectTag     = 0x1F8; /**< System.String* */
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

/**
 * Component::get_gameObject -- `GameObject *f(Component *this, MethodInfo *)`.
 *
 * Needed to ask whether a component's object is actually active. The level
 * is full of item objects that exist but are deactivated for this run's
 * layout (see ObjectsManager's Preset1O..Preset5O), and their components
 * stay perfectly alive -- so a liveness check alone happily reports items
 * that aren't in the world.
 */
static const uintptr_t OFFSET_Component_get_gameObject        = 0x71D4A0;

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
