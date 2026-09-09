#include "game/player.h"
#include "game/offsets.h"
#include "core/keybinds.h"
#include "core/patch_local.h"
#include "overlay/d3d11_hook.h"
#include "MinHook.h"

#include <windows.h>

bool player_speed_enabled = false;
float player_speed_multiplier = 1.0f;
bool player_noclip_enabled = false;
bool player_no_hard_landing = false;
bool player_air_control = false;

static MobileFPS_Update_t original_update = NULL;
static void *volatile g_player = NULL;
static uintptr_t g_base = 0;

/*
 * Speed state.
 *
 * Writing the speed fields from this hook alone does nothing: MobileFPS's
 * own Update rewrites SpeedMove and SpeedMoveCrouch from hardcoded constants
 * at the top of every frame, before anything reads them, and then derives
 * moveSpeed from those. So the two stores that do that are NOPed out while
 * the override is on -- see OFFSET_MobileFPS_SpeedStores -- and only then do
 * our own writes survive long enough to matter.
 *
 * The derivation itself (moveSpeed = crouched ? SpeedMoveCrouch : SpeedMove)
 * is deliberately left alone, so crouching, standing and the isFalling stop
 * all keep working; they just read our numbers instead of the game's.
 */
static void *g_applied_instance = NULL;
static float g_original_stand = 0.0f;
static float g_original_crouch = 0.0f;
static bool g_originals_captured = false;
static bool g_dirty = false;

static bool g_speed_patched = false;
static uint8_t g_speed_store_original[MOBILEFPS_SPEED_STORES_SIZE];
static const uint8_t g_nop_patch[MOBILEFPS_SPEED_STORES_SIZE] = {
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

/* Whether the CharacterController is currently switched off, and which player
 * it belongs to -- re-enabling the wrong (rebuilt) one would do nothing and
 * leave the new one intangible. */
static bool g_collider_disabled = false;

void player_mark_dirty(void) {
	g_dirty = true;
}

void *player_current(void) {
	return g_player;
}

/* Destroyed Unity objects keep their managed wrapper, so a pointer that
 * looks fine from C can be a freed native object. Same rule as everywhere
 * else: check m_CachedPtr before calling into one. */
static bool object_alive(void *object) {
	if (!object) return false;
	return *(void **)((uintptr_t)object + FIELD_UnityObject_m_CachedPtr) != NULL;
}

static bool read_bool(void *instance, uintptr_t field) {
	return *(volatile bool *)((uintptr_t)instance + field);
}

static float read_float(void *instance, uintptr_t field) {
	return *(volatile float *)((uintptr_t)instance + field);
}

static void write_float(void *instance, uintptr_t field, float value) {
	*(volatile float *)((uintptr_t)instance + field) = value;
}

/* Air control: the two `jnz` that route a falling frame to `moveSpeed = 0`.
 * Two NOPs each, so the fall falls through to the real speed instead. */
static bool g_air_patched = false;
static const uint8_t g_nop2[MOBILEFPS_FALL_GATE_SIZE] = { 0x90, 0x90 };
/* Both gates are the same two bytes -- `jnz short` over the seven that load
 * the real speed. Verified before writing so a shifted offset refuses rather
 * than NOPing out whatever now lives there. */
static const uint8_t g_expect_jnz[MOBILEFPS_FALL_GATE_SIZE] = { 0x75, 0x07 };
static uint8_t g_air_original[2][MOBILEFPS_FALL_GATE_SIZE];

bool player_apply_air_control(void) {
	if (g_base == 0) {
		player_air_control = false;
		return false;
	}
	if (player_air_control == g_air_patched) return true;

	const uintptr_t sites[2] = {
		g_base + OFFSET_MobileFPS_FallGate_Stand,
		g_base + OFFSET_MobileFPS_FallGate_Crouch,
	};

	if (player_air_control) {
		for (int i = 0; i < 2; i++) {
			if (patch_bytes_checked(sites[i], g_expect_jnz, g_nop2,
			                        MOBILEFPS_FALL_GATE_SIZE, g_air_original[i])) {
				continue;
			}
			OutputDebugStringA("[cheat] falling speed gate is not `jnz short`, refusing to patch");
			/* Standing patched without crouched would give air control only
			 * while upright, which is stranger than neither. */
			for (int j = 0; j < i; j++) {
				restore_bytes_local(sites[j], g_air_original[j], MOBILEFPS_FALL_GATE_SIZE);
			}
			player_air_control = false;
			return false;
		}
		g_air_patched = true;
		OutputDebugStringA("[cheat] air control enabled");
		return true;
	}

	/* A failed restore keeps the flag set, so the next enable can't save our
	 * own NOPs over the only copy of those two jumps. */
	bool restored = true;
	for (int i = 0; i < 2; i++) {
		if (!restore_bytes_local(sites[i], g_air_original[i], MOBILEFPS_FALL_GATE_SIZE)) {
			restored = false;
		}
	}
	if (!restored) {
		OutputDebugStringA("[cheat] failed to restore the falling speed gate");
		player_air_control = true;
		return false;
	}

	g_air_patched = false;
	OutputDebugStringA("[cheat] air control disabled");
	return true;
}

/* Stops or restores the game's per-frame rewrite of the two speed presets.
 * Idempotent, for the same reason the death patch is: patching twice would
 * save our own NOPs as the "original" bytes. */
static bool set_speed_patch(bool on) {
	if (g_base == 0) return false;
	if (on == g_speed_patched) return true;

	uintptr_t address = g_base + OFFSET_MobileFPS_SpeedStores;

	if (on) {
		if (!patch_bytes_local(address, g_nop_patch, MOBILEFPS_SPEED_STORES_SIZE,
		                       g_speed_store_original)) {
			OutputDebugStringA("[cheat] failed to patch the MobileFPS speed stores");
			return false;
		}
		g_speed_patched = true;
		return true;
	}

	/* The saved bytes are the only copy of those two instructions, so a
	 * failed restore must leave the flag set -- clearing it would let the
	 * next enable save the NOPs over them and strand the player at whatever
	 * speed was last written, permanently. */
	if (!restore_bytes_local(address, g_speed_store_original, MOBILEFPS_SPEED_STORES_SIZE)) {
		OutputDebugStringA("[cheat] failed to restore the MobileFPS speed stores");
		return false;
	}
	g_speed_patched = false;
	return true;
}

/* Applies or lifts the speed override. */
static void apply_speed(void *instance) {
	if (player_speed_enabled) {
		/* Read the game's own values BEFORE patching, while it is still
		 * writing them each frame.
		 *
		 * Two moments have to be avoided, and both are handled by refusing to
		 * capture rather than by correcting afterwards. InWeb swaps the
		 * constants for a slowed 1.0/0.3 pair, so capturing while stuck in a
		 * web would base every later multiplier on the wrong numbers for the
		 * rest of the session. And our hook sits at Update's entry, so on a
		 * freshly built player the game has not written either field yet and
		 * the serialized prefab values are all that's there.
		 *
		 * Waiting costs at most a frame: as soon as Update has run once
		 * outside a web, the real constants are sitting in the fields. */
		if (!g_originals_captured && !read_bool(instance, FIELD_MobileFPS_InWeb)) {
			const float stand = read_float(instance, FIELD_MobileFPS_SpeedMove);
			const float crouch = read_float(instance, FIELD_MobileFPS_SpeedMoveCrouch);
			if (stand > 0.0f && crouch > 0.0f) {
				g_original_stand = stand;
				g_original_crouch = crouch;
				g_originals_captured = true;
			}
		}
		/* Nothing sensible to scale yet -- try again next tick rather than
		 * patching the stores out and pinning the player at zero. */
		if (!g_originals_captured) return;
		if (!set_speed_patch(true)) {
			player_speed_enabled = false;
			return;
		}
		/* Only the two presets: moveSpeed derives itself from them further
		 * down the same Update, and that store is left intact. */
		float m = player_speed_multiplier;
		write_float(instance, FIELD_MobileFPS_SpeedMove, g_original_stand * m);
		write_float(instance, FIELD_MobileFPS_SpeedMoveCrouch, g_original_crouch * m);
		return;
	}

	/* Nothing to write back: with the stores restored, the game overwrites
	 * both fields with its own constants on the very next frame. */
	set_speed_patch(false);
}

/*
 * Holds the falling state at zero.
 *
 * Necessary because noclip disables the CharacterController, and
 * FallingHolder decides whether the player is falling purely from
 * CharacterController.isGrounded -- which a disabled controller reports as
 * false forever. Left alone, one noclip session convinces the game the
 * player has been falling the whole time: fallDuration runs away past
 * FallMega, which latches Damaged, fires HandleLanding and calls
 * PickRay::CheckItemDropping to drop whatever is in your hands. The latched
 * flags then outlive noclip, and CrouchHolder refuses to crouch or stand
 * while isFalling or isLanding is set -- which is what "I can't crouch or
 * pick anything up after turning noclip off" actually was.
 *
 * Cleared every tick rather than once, because FallingHolder::Update is a
 * separate MonoBehaviour and may run either side of this one in the frame.
 * Whichever order it lands in, the counter never gets more than a single
 * frame to accumulate, so it can't reach FallMega.
 */
static void suppress_falling(void *instance) {
	void *falling = *(void **)((uintptr_t)instance + FIELD_MobileFPS_FallingHolder);
	if (!object_alive(falling)) return;

	/* CanFallSound, isFalling, isLanding, Fell, Damaged and DeathFall are six
	 * contiguous bools -- Damaged and DeathFall included deliberately, since
	 * a fall that already registered would otherwise kill the player on the
	 * next grounded frame. */
	volatile unsigned char *flags =
	    (volatile unsigned char *)((uintptr_t)falling + FIELD_FallingHolder_FLAGS_FIRST);
	for (int i = 0; i < FALLINGHOLDER_FLAG_COUNT; i++) {
		flags[i] = 0;
	}

	*(volatile float *)((uintptr_t)falling + FIELD_FallingHolder_fallDuration) = 0.0f;
	*(volatile float *)((uintptr_t)falling + FIELD_FallingHolder_DurateCan) = 0.0f;
}

/*
 * Keeps a fall from ever counting as a hard one.
 *
 * FallingHolder::Update compares fallDuration against FallDurationHold on
 * the frame you land, and anything above it runs HandleLanding() -- the
 * stagger that sets isAllowedToMove to false, plays the get-up animation and
 * waits on a coroutine to give control back. Holding the counter at the
 * threshold means that comparison is never true, so the drop resolves as an
 * ordinary landing with its usual sound.
 *
 * Read from the instance rather than hardcoded: both thresholds are
 * serialized fields, so a different level or difficulty can carry different
 * values.
 */
static void cap_fall_duration(void *instance) {
	void *falling = *(void **)((uintptr_t)instance + FIELD_MobileFPS_FallingHolder);
	if (!object_alive(falling)) return;

	const float hold = read_float(falling, FIELD_FallingHolder_FallDurationHold);
	/* A zero or nonsense threshold would clamp every fall to nothing, which
	 * would also silence ordinary landings. Leave it alone instead. */
	if (!(hold > 0.0f)) return;

	if (read_float(falling, FIELD_FallingHolder_fallDuration) > hold) {
		/* Exactly at the threshold, not below it: the test is a strict
		 * greater-than, so this is the largest value that still lands soft. */
		write_float(falling, FIELD_FallingHolder_fallDuration, hold);
	}
}

/* Turns the player's collider on or off. Guarded so the IL2CPP call only
 * happens on an actual transition, not every tick. */
static void set_collider_enabled(void *instance, bool enabled) {
	void *controller = *(void **)((uintptr_t)instance + FIELD_MobileFPS_characterController);
	if (!object_alive(controller)) return;

	((Unity_set_bool_t)(g_base + OFFSET_Collider_set_enabled))(controller, enabled, NULL);
	g_collider_disabled = !enabled;
}

/*
 * Noclip.
 *
 * With the CharacterController disabled the game's own
 * CharacterController::Move in ApplyFinalMovements becomes a no-op, so the
 * player stops moving entirely -- which is why this has to drive the
 * transform itself. It does that with the game's own moveDirection rather
 * than re-deriving one: MobileFPS::HandlePlayerMovement has already turned
 * raw input into a world-space vector scaled by moveSpeed and oriented to
 * where the player is facing, and ApplyFinalMovements does nothing to it but
 * multiply by deltaTime. Reusing it means noclip movement matches walking
 * exactly, and needs no input handling of our own.
 *
 * The vertical component is the exception. moveDirection.y carries the
 * game's gravity accumulator, which with the controller disabled just grows
 * unbounded, so it's ignored and replaced with the two rebindable noclip
 * keys, read straight from Win32 -- no IL2CPP input calls needed.
 */
static void noclip_move(void *instance) {
	void *transform =
	    ((Component_get_transform_t)(g_base + OFFSET_Component_get_transform))(instance, NULL);
	if (!object_alive(transform)) return;

	unity_vector3 direction = *(unity_vector3 *)((uintptr_t)instance + FIELD_MobileFPS_moveDirection);

	/* Held keys rather than edges, so these are read directly instead of
	 * through the keybind snapshot -- which lives on the overlay thread and
	 * only reports the frame a key went down. Skipped entirely while the menu
	 * has a text field focused, or rebinding a key would fly you across the
	 * level as you typed it. */
	float vertical = 0.0f;
	if (!overlay_wants_keyboard()) {
		const int up = keybind_keys[KEYBIND_NOCLIP_UP];
		const int down = keybind_keys[KEYBIND_NOCLIP_DOWN];
		if (up > 0 && (GetAsyncKeyState(up) & 0x8000)) vertical += 1.0f;
		if (down > 0 && (GetAsyncKeyState(down) & 0x8000)) vertical -= 1.0f;
	}

	float delta = ((Time_get_deltaTime_t)(g_base + OFFSET_Time_get_deltaTime))(NULL);
	/* A paused or hitching frame can hand back 0 or something absurd; a step
	 * built from that either does nothing or teleports the player out of the
	 * level. */
	if (!(delta > 0.0f) || delta > 0.5f) return;

	float rise = read_float(instance, FIELD_MobileFPS_moveSpeed) * vertical;

	unity_vector3 position;
	((Transform_get_position_t)(g_base + OFFSET_Transform_get_position))(&position, transform, NULL);
	position.x += direction.x * delta;
	position.y += rise * delta;
	position.z += direction.z * delta;
	((Transform_set_position_t)(g_base + OFFSET_Transform_set_position))(transform, &position, NULL);
}

/* Main thread only -- runs from the MobileFPS::Update detour below. */
static void player_tick(void *instance) {
	if (instance == NULL || g_base == 0) return;

	/* A different instance means the game rebuilt the player on a level load
	 * or a new game, so its speeds are back to the defaults and anything we
	 * saved belonged to an object that no longer exists. The collider flag
	 * goes with it: the new player's controller is enabled, whatever we did
	 * to the old one. */
	bool new_instance = (instance != g_applied_instance);
	if (new_instance) {
		g_applied_instance = instance;
		g_collider_disabled = false;
	}

	/* Written every tick while the override is on, rather than only on
	 * change. With the game's own stores patched out nothing is fighting us,
	 * so this is two float writes that settle a rebuilt player and a moved
	 * slider alike -- cheaper than the bookkeeping needed to skip them. */
	if (player_speed_enabled || g_speed_patched || g_dirty || new_instance) {
		g_dirty = false;
		apply_speed(instance);
	}

	/* Before noclip's own handling, which zeroes the counter outright while
	 * flying -- doing it the other way round would just be redundant. */
	if (player_no_hard_landing) {
		cap_fall_duration(instance);
	}

	if (player_noclip_enabled) {
		if (!g_collider_disabled) {
			set_collider_enabled(instance, false);
		}
		suppress_falling(instance);
		noclip_move(instance);
	} else if (g_collider_disabled) {
		set_collider_enabled(instance, true);
		/* One last clear on the way out. The controller only reports itself
		 * grounded again once it has been re-enabled AND moved, so without
		 * this the flags stay set for the frames in between -- long enough to
		 * block a crouch, and long enough for a stale fallDuration to be read
		 * as a killing fall. */
		suppress_falling(instance);
	}
}

static void __fastcall hooked_update(void *instance) {
	g_player = instance;
	player_tick(instance);
	original_update(instance);
}

int player_hook_install(void) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) {
		OutputDebugStringA("[cheat] GameAssembly.dll not loaded, cannot hook MobileFPS::Update");
		return 0;
	}
	g_base = base;

	void *target = (void *)(base + OFFSET_MobileFPS_Update);
	if (MH_CreateHook(target, (void *)&hooked_update, (void **)&original_update) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(MobileFPS::Update) failed");
		return 0;
	}
	if (MH_EnableHook(target) != MH_OK) {
		OutputDebugStringA("[cheat] MH_EnableHook(MobileFPS::Update) failed");
		return 0;
	}

	OutputDebugStringA("[cheat] MobileFPS::Update hooked");
	return 1;
}
