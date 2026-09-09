#include "game/ai_granny_hook.h"
#include "game/granny_ai.h"
#include "game/offsets.h"
#include "core/patch_local.h"
#include "core/crashlog.h"
#include "overlay/esp.h"
#include "MinHook.h"

#include <windows.h>

static AI_Granny_FixedUpdate_t original_fixed_update = NULL;
static PlayerStatus_GrannyCaughtYou_t original_granny_caught_you = NULL;
static PlayerStatus_GrannyCaughtYouBed_t original_granny_caught_you_bed = NULL;
static void *volatile g_granny_instance = NULL;
static uintptr_t g_gameassembly_base = 0;

/*
 * NormalDeath, KnockDeath and PlayerGettingStopped are disabled by byte
 * patch rather than by a MinHook detour. Both were pure no-op detours -- they existed only to stop
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

/*
 * PlayerGettingStopped joins the two deaths rather than being handled
 * separately, because on its own a suppressed death is worse than no cheat:
 * the kill sequences take control away first and only get it back from the
 * death that follows. Patch the death alone and the spider leaves you
 * standing still with a locked camera forever. All three of its callers are
 * kill paths, so there is nothing else it would break.
 */
static struct {
	const char *name;
	uintptr_t rva;
	uint8_t original[DEATH_PATCH_SIZE];
} g_death_patches[] = {
	{ "NormalDeath",          OFFSET_PlayerStatus_NormalDeath,         { 0 } },
	{ "KnockDeath",           OFFSET_PlayerStatus_KnockDeath,          { 0 } },
	{ "PlayerGettingStopped", OFFSET_PlayerStatus_PlayerGettingStopped, { 0 } },
};

#define DEATH_PATCH_COUNT ((int)(sizeof(g_death_patches) / sizeof(g_death_patches[0])))

static bool g_death_patched = false;

/* Bound to the "Blind" checkbox in the Granny tab. Applied every tick in
 * hooked_fixed_update rather than once on toggle, because BlindTimer means
 * the game clears IsBlind on its own. */
bool immortality = false;
bool granny_is_blind = false;

/* Bound to the "Deaf" checkbox. Applied here for the same reason blind is:
 * the game keeps handing her fresh noises, so it has to be re-cleared. */
bool granny_is_deaf = false;

static void __fastcall hooked_fixed_update(void *instance) {
    g_granny_instance = instance;

    /* This runs on the game's main thread, which is the only safe place to
     * call into IL2CPP -- so ESP gathers its camera/position data here and
     * the render thread just draws the cached results. */
    crashlog_mark("granny: esp collect");
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

	/* Deafness is done by the byte patch in granny_apply_deaf(), which stops
	 * her acquiring a noise at all. This only drops one she was ALREADY
	 * following when the toggle went on -- without it she finishes walking to
	 * the last thing she heard before going deaf, which reads as the feature
	 * not working.
	 *
	 * NoiseObj is a managed reference, and writing NULL over one needs no GC
	 * write barrier, so this is a plain store like the rest. */
	if (granny_is_deaf && instance != NULL) {
		*(volatile bool *)((uintptr_t)instance + FIELD_AI_Granny_IsFollowingSound) = false;
		*(void *volatile *)((uintptr_t)instance + FIELD_AI_Granny_NoiseObj) = NULL;
		/* Without this she still creeps toward the last noise's position for
		 * as long as the timer has left to run. */
		*(volatile float *)((uintptr_t)instance + FIELD_AI_Granny_TimerNearNoise) = 0.0f;
	}

	/* Writes only when she's been rebuilt or a control changed -- see
	 * granny_ai_tick(). */
	crashlog_mark("granny: ai tick");
	granny_ai_tick(instance);

    crashlog_mark("granny: original");
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
		/* Reported like its two siblings rather than swallowed. Falling
		 * through here used to make install claim success while the bed
		 * catch stayed unhooked, so Immortality would show as on and Granny
		 * would still kill the player under a bed. */
		OutputDebugStringA("[cheat] MH_CreateHook(PlayerStatus::GrannyCaughtYouBed) failed");
		return 0;
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

/*
 * Deaf is a byte patch, not a field write.
 *
 * `and al, 0` over the `test al, al` that asks whether FindGameObjectWithTag
 * turned up a noise object -- ZF ends up set either way, so the jz that skips
 * the whole acquisition is always taken.
 */
#define DEAF_PATCH_SIZE AI_GRANNY_NOISE_ACQUIRE_SIZE
static const uint8_t g_deaf_patch[DEAF_PATCH_SIZE] = { 0x24, 0x00 };  /* and al, 0  */
static const uint8_t g_deaf_expect[DEAF_PATCH_SIZE] = { 0x84, 0xC0 }; /* test al, al */
static uint8_t g_deaf_original[DEAF_PATCH_SIZE];
static bool g_deaf_patched = false;

bool granny_apply_deaf(void) {
	if (g_gameassembly_base == 0) {
		granny_is_deaf = false;
		return false;
	}
	if (granny_is_deaf == g_deaf_patched) return true;

	const uintptr_t site = g_gameassembly_base + OFFSET_AI_Granny_NoiseAcquire;

	if (granny_is_deaf) {
		if (!patch_bytes_checked(site, g_deaf_expect, g_deaf_patch,
		                         DEAF_PATCH_SIZE, g_deaf_original)) {
			OutputDebugStringA("[cheat] noise acquisition is not `test al, al`, refusing to patch");
			granny_is_deaf = false;
			return false;
		}
		g_deaf_patched = true;
		OutputDebugStringA("[cheat] granny deafened");
		return true;
	}

	/* A failed restore keeps the flag set, so the next enable can't save our
	 * own bytes over the only copy of that instruction. */
	if (!restore_bytes_local(site, g_deaf_original, DEAF_PATCH_SIZE)) {
		OutputDebugStringA("[cheat] failed to restore the noise acquisition");
		granny_is_deaf = true;
		return false;
	}
	g_deaf_patched = false;
	OutputDebugStringA("[cheat] granny hearing restored");
	return true;
}

bool granny_apply_immortality(void) {
	if (g_gameassembly_base == 0) return false;

	/* The two catch paths stay detours -- they substitute real behaviour
	 * (ResetAIDecision) rather than just suppressing the original, so they
	 * can't be byte patched like the deaths below. */
	uintptr_t caught = g_gameassembly_base + OFFSET_PlayerStatus_GrannyCaughtYou;
	uintptr_t caught_bed = g_gameassembly_base + OFFSET_PlayerStatus_GrannyCaughtYouBed;
	if (immortality) {
		MH_EnableHook((LPVOID)caught);
		MH_EnableHook((LPVOID)caught_bed);
	} else {
		MH_DisableHook((LPVOID)caught);
		MH_DisableHook((LPVOID)caught_bed);
	}

	if (granny_set_death_disabled(immortality)) return true;

	OutputDebugStringA("[cheat] death patch failed, reverting toggle");
	immortality = !immortality;
	return false;
}

bool granny_set_death_disabled(bool disabled) {
	if (g_gameassembly_base == 0) return false;
	/* Idempotent: patching twice would save the `ret` as the "original"
	 * byte and make the restore permanent. */
	if (disabled == g_death_patched) return true;

	if (disabled) {
		for (int i = 0; i < DEATH_PATCH_COUNT; i++) {
			if (patch_bytes_local(g_gameassembly_base + g_death_patches[i].rva, g_ret_patch,
			                      DEATH_PATCH_SIZE, g_death_patches[i].original)) {
				continue;
			}

			char line[96];
			wsprintfA(line, "[cheat] failed to patch PlayerStatus::%s", g_death_patches[i].name);
			OutputDebugStringA(line);

			/* All or nothing. Half of this applied is the worst state to be
			 * in: PlayerGettingStopped patched without the deaths means you
			 * still die, and the deaths without it means you freeze. */
			for (int j = 0; j < i; j++) {
				restore_bytes_local(g_gameassembly_base + g_death_patches[j].rva,
				                    g_death_patches[j].original, DEATH_PATCH_SIZE);
			}
			return false;
		}
		g_death_patched = true;
		OutputDebugStringA("[cheat] death and jumpscare-stop paths patched to ret");
		return true;
	}

	/* The saved bytes are the only surviving copy of those opcodes, so a
	 * failed restore must NOT clear the flag. Clearing it would let the next
	 * enable re-run patch_bytes_local, which would dutifully save the 0xC3
	 * still sitting there as the "original" -- after that every restore
	 * writes 0xC3 back and death stays suppressed for the rest of the
	 * session, with the menu reporting it as off. */
	bool restored = true;
	for (int i = 0; i < DEATH_PATCH_COUNT; i++) {
		if (!restore_bytes_local(g_gameassembly_base + g_death_patches[i].rva,
		                         g_death_patches[i].original, DEATH_PATCH_SIZE)) {
			restored = false;
		}
	}
	if (!restored) {
		OutputDebugStringA("[cheat] failed to restore the death patch, leaving it applied");
		return false;
	}

	g_death_patched = false;
	OutputDebugStringA("[cheat] death and jumpscare-stop paths restored");
	return true;
}
