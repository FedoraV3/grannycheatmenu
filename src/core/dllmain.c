#include <windows.h>
#include "core/patch_local.h"
#include "game/offsets.h"
#include "overlay/d3d11_hook.h"
#include "game/ai_granny_hook.h"
#include "overlay/esp.h"

/**
 * @brief Poll for a module to appear, in case we're mapped in before it loads.
 *
 * GameAssembly.dll may not be loaded yet the instant we're mapped in,
 * depending on when the injector attaches, so this retries GetModuleHandleW
 * instead of failing immediately.
 *
 * @param name      Module name to look up, e.g. `L"GameAssembly.dll"`.
 * @param max_tries Maximum number of attempts before giving up.
 * @param sleep_ms  Delay between attempts, in milliseconds.
 * @return The module's base address, or 0 if it never showed up.
 */
static uintptr_t wait_for_module(const wchar_t *name, int max_tries, DWORD sleep_ms) {
    for (int i = 0; i < max_tries; i++) {
        HMODULE mod = GetModuleHandleW(name);
        if (mod) return (uintptr_t)mod;
        Sleep(sleep_ms);
    }
    return 0;
}

/**
 * @brief Entry point for our own worker thread, started from DllMain.
 *
 * Never do real work directly in DllMain — the loader lock is held there
 * and calling most APIs (including LoadLibrary, or blocking calls) can
 * deadlock the process. Instead DllMain just spawns this thread and
 * returns immediately; this is where actual patch logic belongs.
 *
 * @param param Unused (required by LPTHREAD_START_ROUTINE's signature).
 * @return Unused thread exit code.
 */
static DWORD WINAPI main_thread(LPVOID param) {
    (void)param;

    /* Before any hook that can reach esp_collect() is enabled, since the
     * ESP's cross-thread state needs its lock to exist first. */
    esp_init();

    if (!d3d11_hook_install()) {
        OutputDebugStringA("[cheat] d3d11 hook failed, menu will not render");
    }

    uintptr_t base = wait_for_module(L"GameAssembly.dll", 50, 100);
    if (base == 0) {
        OutputDebugStringA("[cheat] GameAssembly.dll never showed up");
        return 0;
    }

    uintptr_t granny_caught_you     = base + OFFSET_PlayerStatus_GrannyCaughtYou;
    uintptr_t granny_caught_you_bed = base + OFFSET_PlayerStatus_GrannyCaughtYouBed;
    uintptr_t knock_death           = base + OFFSET_PlayerStatus_KnockDeath;
    uintptr_t normal_death          = base + OFFSET_PlayerStatus_NormalDeath;

    uintptr_t reset_ai_decision     = base + OFFSET_AI_Granny_ResetAIDecision;
    uintptr_t stop_ai               = base + OFFSET_AI_Granny_StopAI;
    uintptr_t chase_action          = base + OFFSET_AI_Granny_ChaseAction;
    uintptr_t smack_timer           = base + OFFSET_AI_Granny_SmackTimer;

    /* Addresses resolved but not yet patched -- decide per function what to
     * write (e.g. a `ret` prologue patch to no-op it) before touching these.
     * patch_bytes_local()/aob_scan_local() from patch_local.h are ready to
     * use once that's decided. */
    (void)granny_caught_you; (void)granny_caught_you_bed;
    (void)knock_death; (void)normal_death;
    (void)reset_ai_decision; (void)stop_ai; (void)chase_action; (void)smack_timer;

    if (!ai_granny_hook_install()) {
        OutputDebugStringA("[cheat] AI_Granny hook failed, no live instance tracking");
    }

    if (!esp_install_hooks()) {
        OutputDebugStringA("[cheat] ESP hooks failed, item ESP will have no data");
    }

    OutputDebugStringA("[cheat] resolved GameAssembly.dll offsets");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
    }
    return TRUE;
}
