#include "game/ai_granny_hook.h"
#include "game/granny_ai.h"
#include "game/offsets.h"
#include "core/patch_local.h"
#include "overlay/esp.h"
#include "MinHook.h"

#include <windows.h>

static AI_Granny_FixedUpdate_t original_fixed_update = NULL;
static PlayerStatus_GrannyCaughtYou_t original_granny_caught_you = NULL;
static PlayerStatus_GrannyCaughtYouBed_t original_granny_caught_you_bed = NULL;
static void *volatile g_granny_instance = NULL;
static uintptr_t g_gameassembly_base = 0;

/*
 * NormalDeath and KnockDeath are disabled by byte patch rather than by a
 * MinHook detour. Both were pure no-op detours -- they existed only to stop
 * the original running -- so writing `ret` over the entry point does the
 * same job without a trampoline, and saves two hooks.
 *
 * `ret` (0xC3), not NOP: these are `void f(void *this)` under the Win64 ABI,
 * where the caller cleans the stack and nothing has been pushed at entry, so
 * returning immediately is safe. NOPing the first instruction would only
 * skip it and fall through into the rest of the function.
 *
 * GrannyCaughtYou and GrannyCaughtYouBed stay as detours: they don't just
 * suppress the original, they call ResetAIDecision, which needs real code.
 */
#define DEATH_PATCH_SIZE 1
static const uint8_t g_ret_patch[DEATH_PATCH_SIZE] = { 0xC3 };

static uint8_t g_normal_death_original[DEATH_PATCH_SIZE];
static uint8_t g_knock_death_original[DEATH_PATCH_SIZE];
static bool g_death_patched = false;

/* Bound to the "Blind" checkbox in the Granny tab. Applied every tick in
 * hooked_fixed_update rather than once on toggle, because BlindTimer means
 * the game clears IsBlind on its own. */
bool granny_is_blind = false;

static void __fastcall hooked_fixed_update(void *instance) {
    g_granny_instance = instance;

    /* This runs on the game's main thread, which is the only safe place to
     * call into IL2CPP -- so ESP gathers its camera/position data here and
     * the render thread just draws the cached results. */
    esp_collect(instance);
	
	/* Blind toggle, polled here because this is the game's main thread.
	 *
	 * Re-applied every tick rather than written once, since BlindTimer
	 * (+0x168) means the game clears IsBlind on its own.
	 *
	 * Deliberately only writes when the toggle is ON. Forcing it to false
	 * otherwise would also cancel the game's own blinding -- pepper spray
	 * sets this flag (see PepperedEnemy at +0x178), so an else branch here
	 * would wipe the effect a tick after the player used the spray. With
	 * the toggle off, BlindTimer just runs down naturally. */
	if (granny_is_blind && instance != NULL) {
		*(volatile bool *)((uintptr_t)instance + FIELD_AI_Granny_IsBlind) = true;
	}

	/* Writes only when she's been rebuilt or a control changed -- see
	 * granny_ai_tick(). */
	granny_ai_tick(instance);

    original_fixed_update(instance);
}

static void __fastcall granny_caught_you(void *instance) {
	/* `instance` here is a PlayerStatus*, not an AI_Granny* -- ResetAIDecision
	 * needs to run on the actual AI_Granny instance, which is what
	 * ai_granny_current() (kept fresh by the FixedUpdate hook above) hands
	 * us. Calling it on the wrong object corrupts memory instead of
	 * freezing her. */
	(void)instance;

	void *granny = ai_granny_current();
	if (granny) {
		((AI_Granny_ResetAIDecision_t)(g_gameassembly_base + OFFSET_AI_Granny_ResetAIDecision))(granny);
	} else {
		OutputDebugStringA("[cheat] GrannyCaughtYou fired but no AI_Granny instance tracked yet");
	}
}

static void __fastcall granny_caught_you_bed(void *instance) {
	// yeah i just wont let that happen
	(void)instance;
	
	void *granny = ai_granny_current();
	if (granny) {
		((AI_Granny_ResetAIDecision_t)(g_gameassembly_base + OFFSET_AI_Granny_ResetAIDecision))(granny);
	} else {
		OutputDebugStringA("[cheat] GrannyCaughtYouBed fired but no AI_Granny instance tracked yet");
	}
	return;
}

int ai_granny_hook_install(void) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
    if (base == 0) {
        OutputDebugStringA("[cheat] GameAssembly.dll not loaded, cannot hook AI_Granny::FixedUpdate");
        return 0;
    }
    g_gameassembly_base = base;

    if (MH_CreateHook((LPVOID)(base + OFFSET_AI_Granny_FixedUpdate), (void *)&hooked_fixed_update,
                       (void **)&original_fixed_update) != MH_OK) {
        OutputDebugStringA("[cheat] MH_CreateHook(AI_Granny::FixedUpdate) failed");
        return 0;
    }
	
	if (MH_CreateHook((LPVOID)(base + OFFSET_PlayerStatus_GrannyCaughtYou), (void *)&granny_caught_you,
					  (void **)&original_granny_caught_you) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(PlayerStatus::GrannyCaughtYou failed");
		return 0;					
	}
	
	if (MH_CreateHook((LPVOID)(base + OFFSET_PlayerStatus_GrannyCaughtYouBed), (void *)&granny_caught_you_bed,
					  (void **)&original_granny_caught_you_bed) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(PlayerStatus::GrannyCaughtYouBed failed");
	}
	
	/* NormalDeath and KnockDeath deliberately get no hook -- they're byte
	 * patched instead, in granny_set_death_disabled(). */

    if (MH_EnableHook((LPVOID)(base + OFFSET_AI_Granny_FixedUpdate)) != MH_OK) {
        OutputDebugStringA("[cheat] MH_EnableHook(AI_Granny::FixedUpdate) failed");
        return 0;
    }

    OutputDebugStringA("[cheat] AI_Granny::FixedUpdate hooked");
    return 1;
}

void *ai_granny_current(void) {
    return g_granny_instance;
}

/* The instance StopAI was called on. Compared against the live one so the
 * "stopped" state clears itself when the game builds a new AI_Granny. */
static void *g_stopped_instance = NULL;

bool granny_stop_ai(void) {
	if (g_gameassembly_base == 0) return false;

	void *granny = ai_granny_current();
	if (!granny) {
		OutputDebugStringA("[cheat] no AI_Granny instance yet, can't call StopAI");
		return false;
	}

	((AI_Granny_StopAI_t)(g_gameassembly_base + OFFSET_AI_Granny_StopAI))(granny);
	g_stopped_instance = granny;
	OutputDebugStringA("[cheat] StopAI called");
	return true;
}

bool granny_is_stopped(void) {
	if (!g_stopped_instance) return false;

	/* A different (or absent) instance means the game replaced her, so
	 * whatever we stopped is gone and this one is untouched. */
	void *current = ai_granny_current();
	if (current != g_stopped_instance) {
		g_stopped_instance = NULL;
		return false;
	}
	return true;
}

bool granny_set_death_disabled(bool disabled) {
	if (g_gameassembly_base == 0) return false;
	/* Idempotent: patching twice would save the `ret` as the "original"
	 * byte and make the restore permanent. */
	if (disabled == g_death_patched) return true;

	uintptr_t normal_death = g_gameassembly_base + OFFSET_PlayerStatus_NormalDeath;
	uintptr_t knock_death = g_gameassembly_base + OFFSET_PlayerStatus_KnockDeath;

	if (disabled) {
		if (!patch_bytes_local(normal_death, g_ret_patch, DEATH_PATCH_SIZE, g_normal_death_original)) {
			OutputDebugStringA("[cheat] failed to patch PlayerStatus::NormalDeath");
			return false;
		}
		if (!patch_bytes_local(knock_death, g_ret_patch, DEATH_PATCH_SIZE, g_knock_death_original)) {
			OutputDebugStringA("[cheat] failed to patch PlayerStatus::KnockDeath");
			/* Don't leave one of the pair patched. */
			restore_bytes_local(normal_death, g_normal_death_original, DEATH_PATCH_SIZE);
			return false;
		}
		g_death_patched = true;
		OutputDebugStringA("[cheat] NormalDeath/KnockDeath patched to ret");
	} else {
		restore_bytes_local(normal_death, g_normal_death_original, DEATH_PATCH_SIZE);
		restore_bytes_local(knock_death, g_knock_death_original, DEATH_PATCH_SIZE);
		g_death_patched = false;
		OutputDebugStringA("[cheat] NormalDeath/KnockDeath restored");
	}
	return true;
}
