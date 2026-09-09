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
 * PlayerStatus::PlayerGettingStopped -- takes control away for a kill
 * sequence, and the reason Immortality used to leave you frozen.
 *
 * It sets MobileFPS.isAllowedToMove and .AbleToMove to false and
 * CrouchHolder.Disabled to true; control comes back from the death and
 * respawn that normally follow. Suppressing only the deaths therefore froze
 * the player permanently -- no movement, no camera -- which is exactly what
 * the spider does:
 *
 *     PlayerStatus::PlayerGettingStopped();   // control taken
 *     yield WaitForSeconds(0.4);
 *     PlayerStatus::NormalDeath();            // patched to ret
 *
 * All three of its callers are kill paths -- AtticSpider's StingKill
 * coroutine, GrannyCaughtYou and GrandpaCaughtYou -- so it belongs in the
 * same patch group as the deaths rather than being handled separately.
 */
static const uintptr_t OFFSET_PlayerStatus_PlayerGettingStopped = 0x24B820;

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
 * AI_MomSpider::Update -- the cellar spider's per-frame driver, hooked purely
 * to capture the live instance, same trick as AI_Granny::FixedUpdate.
 *
 * A separate class from AtticSpider (the one that stings you in the attic):
 * this is the big one that patrols the cellar on its own NavMeshAgent, with
 * its own waypoints, webs and chase state. Only exists while that level is
 * loaded, so the hook simply never fires elsewhere.
 *
 * Game-assembly method, so a single argument and no trailing MethodInfo*.
 */
static const uintptr_t OFFSET_AI_MomSpider_Update             = 0x1D6E20;
typedef granny_method_t AI_MomSpider_Update_t;

/* AI_MomSpider instance fields, for anything that wants them later. */
static const uintptr_t FIELD_AI_MomSpider_Walk_Speed      = 0x20; /**< float */
static const uintptr_t FIELD_AI_MomSpider_Run_Speed       = 0x24; /**< float */
static const uintptr_t FIELD_AI_MomSpider_Agent           = 0x30; /**< NavMeshAgent* */
static const uintptr_t FIELD_AI_MomSpider_IsChasing       = 0x62; /**< bool */

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

/**
 * PickRay::HP -- HandlePuzzles, the manager that holds one bool per
 * progression step in the game.
 *
 * This is where every lock's "do you have the key" answer actually lives.
 * There is no comparison against the item in your hand: PickRay::Update
 * reads a flag off this object and either runs the unlock or prints "It's
 * locked". Confirmed by tracing the padlocked port -- see unlock.c.
 */
static const uintptr_t FIELD_PickRay_HP                   = 0x4D8; /**< HandlePuzzles* */

/**
 * HandlePuzzles::usedPadlockForPort -- "the padlock has been dealt with".
 *
 * The one requirement flag traced end to end so far. Its branch:
 *
 *     if (HP.openedPort)         goto done;
 *     if (!PickRay.buttonClicked) goto done;
 *     PickRay.buttonClicked = 0;
 *     if (HP.usedPadlockForPort) HandlePuzzles::OpenPort();
 *     else                       text("It's locked");
 *
 * openedPort (+0x70) is deliberately NOT listed: it means "already open" and
 * is tested first, so setting it makes the port uninteractable rather than
 * unlocked. The two kinds of flag look alike and behave oppositely.
 */
static const uintptr_t FIELD_HandlePuzzles_usedPadlockForPort = 0x71; /**< bool */

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

/*
 * ==========================================================================
 * MobileFPS -- the player controller. Move speed and noclip both live here.
 * ==========================================================================
 *
 * Confirmed from MobileFPS::HandlePlayerMovement's decompile rather than
 * guessed from the field names: the input vector is scaled by `moveSpeed`
 * alone. SpeedMove/SpeedMoveCrouch are the stored presets the game copies
 * into moveSpeed when you crouch and stand, so all three have to move
 * together or a crouch undoes the override.
 */

/** MobileFPS::Update -- hooked to capture the live player, same trick as
 *  AI_Granny::FixedUpdate. Game-assembly method: single argument. */
static const uintptr_t OFFSET_MobileFPS_Update            = 0x22D9C0;
typedef granny_method_t MobileFPS_Update_t;

static const uintptr_t FIELD_MobileFPS_characterController = 0x28; /**< CharacterController* */
static const uintptr_t FIELD_MobileFPS_moveSpeed           = 0x40; /**< float -- the live one */
static const uintptr_t FIELD_MobileFPS_SpeedMove           = 0x44; /**< float -- standing preset */
static const uintptr_t FIELD_MobileFPS_SpeedMoveCrouch     = 0x48; /**< float -- crouched preset */
/**
 * MobileFPS::InWeb -- set while the player is stuck in a spider web, which is
 * the only thing the two speed constants above encode: Update writes 1.0/0.3
 * while it is set and 6.0/2.8 otherwise. Checked before capturing the
 * originals, so a web can't become the baseline the multiplier scales from.
 */
static const uintptr_t FIELD_MobileFPS_InWeb               = 0x89; /**< bool */
/**
 * MobileFPS::moveDirection -- the world-space step the game is about to hand
 * to CharacterController::Move, already built from input and the camera's
 * facing (ApplyFinalMovements multiplies it by deltaTime and nothing else).
 *
 * This is what makes noclip cheap: with the controller disabled the game's
 * Move() call does nothing, and we can drive the transform with the game's
 * own direction vector instead of re-deriving one from raw input.
 */
static const uintptr_t FIELD_MobileFPS_moveDirection       = 0x54; /**< Vector3 */

/** UnityEngine.Vector3 -- three floats. Larger than 8 bytes, so it crosses
 *  the Win64 ABI by pointer in both directions. */
typedef struct {
	float x, y, z;
} unity_vector3;

/*
 * Traps. Each of these is a `private void OnTriggerEnter(Collider)` that
 * fires the trap when the player walks into it -- there is no shared base
 * class and no global enable, so disabling them means suppressing each entry
 * point. Byte patched to `ret` rather than hooked, exactly like the death
 * functions: nothing needs to run in their place.
 *
 * ExtraTrapsLogic is deliberately absent -- it only has Start/StartFunction
 * and sets the level's traps up, so it has no trigger to suppress. Its .ctor
 * shares RVA 0x1A26E0 with several other classes (identical COMDAT folding),
 * which is a good reason never to patch a constructor here.
 */
static const uintptr_t OFFSET_TrapTrigger_OnTriggerEnter       = 0x211A10;
static const uintptr_t OFFSET_BearTrapLogic_OnTriggerEnter     = 0x1A9A10;
static const uintptr_t OFFSET_ExploTrapTrigger_OnTriggerEnter  = 0x1FC890;
static const uintptr_t OFFSET_TrapPoison_OnTriggerEnter        = 0x261A10;

/*
 * More UnityEngine methods, all taking a trailing MethodInfo* that accepts
 * NULL like the ones above.
 */

/** NavMeshAgent::set_speed -- `void f(NavMeshAgent *this, float, MethodInfo *)`.
 *  Freezing her by zeroing Walk_Speed/Run_Speed alone leaves the agent
 *  coasting at whatever speed it was last given, so the agent is told too. */
static const uintptr_t OFFSET_NavMeshAgent_set_speed      = 0x6E67D0;

/** Collider::set_enabled -- CharacterController derives from Collider, not
 *  Behaviour, so its `enabled` property is this one. */
static const uintptr_t OFFSET_Collider_set_enabled        = 0x769040;
typedef void(__fastcall *Unity_set_bool_t)(void *instance, bool value, void *method);
typedef void(__fastcall *Unity_set_float_t)(void *instance, float value, void *method);

/** Transform::set_position -- `void f(Transform *this, Vector3 *value,
 *  MethodInfo *)`. Confirmed by decompile: Vector3 is 12 bytes so it arrives
 *  by pointer, and the function forwards straight to set_position_Injected. */
static const uintptr_t OFFSET_Transform_set_position      = 0x747B20;
typedef void(__fastcall *Transform_set_position_t)(void *instance, const unity_vector3 *value, void *method);

/** Time::get_deltaTime -- static, `float f(MethodInfo *)`. */
static const uintptr_t OFFSET_Time_get_deltaTime          = 0x72B210;
typedef float(__fastcall *Time_get_deltaTime_t)(void *method);

/*
 * ==========================================================================
 * Spawning an item
 * ==========================================================================
 *
 * Recipe lifted from PickRay::CheckItemDropping, which is what runs when you
 * press the drop key: Instantiate the ItemDrop prefab at the player's drop
 * point, then write CountItem on the copy's ItemSpawn component. ItemSpawn's
 * own Update does the rest -- it activates the chosen item, throws it,
 * unparents it and destroys the dropper.
 *
 * So spawning is the same operation as dropping, minus the part where the
 * game first takes the item out of your hands. Nothing has to be faked.
 *
 * (Hex-Rays renders the CountItem store as a write into `v43.klass`, which
 * is misleading: v43 is the out-buffer of the shared-generic GetComponent,
 * so it holds the ItemSpawn pointer, and the store is a plain float write at
 * +0x24. The constants it writes -- 1.0f, 30.0f, 37.0f -- are CountItem
 * values, which is what confirms the field's meaning.)
 */

/** PickRay::ItemDrop -- the dropper prefab that every dropped item comes
 *  from. A prefab, so it is never active in the scene and never shows up in
 *  a FindObjectsOfType scan. */
static const uintptr_t FIELD_PickRay_ItemDrop             = 0x230; /**< GameObject* */
/** PickRay::DropP -- the transform just in front of the player that the game
 *  drops items at. Reusing it means a spawned item lands exactly where a
 *  dropped one would, with no positioning maths of our own. */
static const uintptr_t FIELD_PickRay_DropP                = 0x50;  /**< Transform* */

/** UnityEngine.Object::Instantiate(GameObject, Vector3, Quaternion). Both
 *  struct arguments are over 8 bytes, so both arrive by pointer. Needs its
 *  baked generic MethodInfo -- NULL is not accepted here, unlike the plain
 *  UnityEngine methods above. */
static const uintptr_t OFFSET_Object_Instantiate          = 0x2CE440;
/** Globals holding POINTERS to the baked MethodInfo, so dereference before
 *  passing them on -- same shape as METHODINFO_GetComponent_ItemSeedData. */
static const uintptr_t METHODINFO_Instantiate_GameObject  = 0xC3B3E0;
static const uintptr_t METHODINFO_GetComponent_ItemSpawn  = 0xC338F0;

/** UnityEngine.Quaternion -- four floats, so it crosses the ABI by pointer. */
typedef struct {
	float x, y, z, w;
} unity_quaternion;

typedef void *(__fastcall *Object_Instantiate_t)(void *original, const unity_vector3 *position,
                                                 const unity_quaternion *rotation, void *method);

/** Transform::get_rotation -- `Quaternion *f(Quaternion *ret, Transform *this,
 *  MethodInfo *)`, same hidden-return shape as get_position. */
static const uintptr_t OFFSET_Transform_get_rotation      = 0x747480;
typedef void *(__fastcall *Transform_get_rotation_t)(void *ret_quaternion, void *instance, void *method);

/*
 * MobileFPS::Update rewrites SpeedMove and SpeedMoveCrouch from hardcoded
 * constants at the top of EVERY frame, before anything reads them:
 *
 *     SpeedMove       = InWeb ? 1.0f : 6.0f;
 *     SpeedMoveCrouch = InWeb ? 0.3f : 2.8f;
 *     ...
 *     moveSpeed = IsCrouched ? SpeedMoveCrouch : SpeedMove;
 *
 * which is why writing those fields from a hook did nothing -- the store
 * landed and was overwritten microseconds later in the same call. There is
 * no setter to intercept and HandlePlayerMovement isn't even called (Update
 * inlines it), so the two stores are NOPed out instead and the fields then
 * keep whatever we put in them.
 *
 *   0x22DABA  f3 0f 11 4f 44    movss [rdi+44h], xmm1   ; SpeedMove
 *   0x22DABF  f3 0f 11 47 48    movss [rdi+48h], xmm0   ; SpeedMoveCrouch
 *
 * Side effect worth knowing: InWeb no longer slows the player either, since
 * that is the only thing those two constants encode.
 */
/*
 * The two branches that kill air control.
 *
 * MobileFPS::Update picks the speed it is about to use, and zeroes it
 * outright while FallingHolder says you are falling -- once for standing and
 * once for crouched:
 *
 *     cmp byte ptr [rax+81h], 0        ; FallingHolder.isFalling
 *     jnz short zero                   ; 75 07
 *     movss xmm0, [rdi+44h]            ; SpeedMove (or +48h crouched)
 *     jmp short store
 *   zero:
 *     xorps xmm0, xmm0                 ; moveSpeed = 0
 *   store:
 *     movss [rdi+40h], xmm0
 *
 * which is why stepping off anything leaves you with no steering at all
 * until you land. NOPing the two jnz makes the fall load the real speed like
 * any other frame; nothing else about falling changes, so landing, damage
 * and the fall sounds all still work.
 *
 * Deliberately not done by clearing isFalling -- that flag also drives
 * landing detection, crouch gating and fall damage, and forcing it false
 * would quietly disable all three.
 */
static const uintptr_t OFFSET_MobileFPS_FallGate_Stand    = 0x22DCE6;
static const uintptr_t OFFSET_MobileFPS_FallGate_Crouch   = 0x22DD94;
#define MOBILEFPS_FALL_GATE_SIZE 2

static const uintptr_t OFFSET_MobileFPS_SpeedStores       = 0x22DABA;
#define MOBILEFPS_SPEED_STORES_SIZE 10

/*
 * FallingHolder -- the script that decides whether the player is falling,
 * and it is the reason noclip needs cleaning up after.
 *
 * FallingHolder::Update keys everything off CharacterController.isGrounded,
 * and a DISABLED controller reports false forever. So for as long as noclip
 * has the controller switched off the game believes the player is in an
 * endless fall:
 *
 *     isFalling = true;
 *     fallDuration += Time.deltaTime * Speed;
 *     if (fallDuration > FallMega && !DeathFall && !Damaged) {
 *         Damaged = true;
 *         HandleLanding();
 *         PickRay::CheckItemDropping();     // drops whatever you hold
 *     }
 *
 * and the consequences outlive noclip: CrouchHolder::Update refuses to
 * crouch or stand while isFalling or isLanding is set, MobileFPS::Update
 * forces moveSpeed to 0 while isFalling, and a big enough fallDuration on
 * the next grounded frame calls PlayerStatus::NormalDeath.
 *
 * Hence the whole block is held at zero while noclip is on. That isn't a
 * workaround so much as the truth: a player who is flying is not falling.
 */
static const uintptr_t FIELD_MobileFPS_FallingHolder      = 0xA0; /**< FallingHolder* */

/* Six contiguous bools at 0x80..0x85, then two floats. Cleared as a group. */
static const uintptr_t FIELD_FallingHolder_FLAGS_FIRST    = 0x80; /**< CanFallSound */
#define FALLINGHOLDER_FLAG_COUNT 6                                /**< ..DeathFall at 0x85 */
/*
 * The two thresholds a fall is measured against, both in the same units as
 * fallDuration. From FallingHolder::Update:
 *
 *     if (fallDuration > FallMega)         -> death, if DeathFall is set
 *     if (fallDuration > FallDurationHold) -> HandleLanding(), the get-up
 *     if (fallDuration <= FallChecker)     -> silent landing
 *     otherwise                            -> the small landing sound
 *
 * so keeping fallDuration at or below FallDurationHold is what turns a
 * bone-shaking landing into an ordinary one.
 */
static const uintptr_t FIELD_FallingHolder_FallDurationHold = 0x50; /**< float */
static const uintptr_t FIELD_FallingHolder_FallMega         = 0x58; /**< float */

static const uintptr_t FIELD_FallingHolder_isFalling      = 0x81; /**< bool */
static const uintptr_t FIELD_FallingHolder_isLanding      = 0x82; /**< bool */
static const uintptr_t FIELD_FallingHolder_fallDuration   = 0x88; /**< float */
static const uintptr_t FIELD_FallingHolder_DurateCan      = 0x8C; /**< float */
