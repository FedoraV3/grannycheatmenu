#include "ai_granny_hook.h"
#include "offsets.h"
#include "esp.h"
#include "MinHook.h"

#include <windows.h>

static AI_Granny_FixedUpdate_t original_fixed_update = NULL;
static PlayerStatus_GrannyCaughtYou_t original_granny_caught_you = NULL;
static PlayerStatus_GrannyCaughtYouBed_t original_granny_caught_you_bed = NULL;
static PlayerStatus_NormalDeath_t original_granny_normal_death = NULL;
static PlayerStatus_KnockDeath_t original_granny_knock_death = NULL;
static void *volatile g_granny_instance = NULL;
static uintptr_t g_gameassembly_base = 0;

static void __fastcall hooked_fixed_update(void *instance) {
    g_granny_instance = instance;

    /* This runs on the game's main thread, which is the only safe place to
     * call into IL2CPP -- so ESP gathers its camera/position data here and
     * the render thread just draws the cached results. */
    esp_collect(instance);

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

static void __fastcall granny_normal_death(void *instance) {
	(void)instance;
	return;
}

static void __fastcall granny_knocked_death(void *instance) {
	(void)instance;
	return;
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
	
	if (MH_CreateHook((LPVOID)(base + OFFSET_PlayerStatus_NormalDeath), (void *)&granny_normal_death,
					  (void **)&original_granny_normal_death) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(PlayerStatus::GrannyNormalDeath failed");
	}
	
	if (MH_CreateHook((LPVOID)(base + OFFSET_PlayerStatus_KnockDeath), (void *)&granny_knocked_death,
					  (void **)&original_granny_knock_death) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(PlayerStatus::GrannyKnockDeath failed");
	}

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
