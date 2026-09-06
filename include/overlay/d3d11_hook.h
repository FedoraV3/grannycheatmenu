#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hook IDXGISwapChain::Present via MinHook.
 *
 * Present isn't exported anywhere we can just GetProcAddress -- its address
 * only exists inside a live swap chain's vtable. So this spins up a
 * throwaway D3D11 device + swap chain bound to an invisible 100x100 window
 * purely to read that vtable, grabs index 8 (Present), then tears the dummy
 * device/window back down immediately. The pointer stays valid afterward
 * because Present is implemented once in the driver, shared by every swap
 * chain in the process -- the game's real swap chain included.
 *
 * Handles MH_Initialize() itself. Safe to call once during startup, e.g.
 * from your DllMain-spawned worker thread (never call this from DllMain
 * itself).
 *
 * @return Nonzero on success. Zero if any step failed -- check
 *         OutputDebugStringA output (e.g. via DebugView) for which one.
 */
int d3d11_hook_install(void);

/**
 * @brief Disable the Present hook and uninitialize MinHook.
 *
 * Only call this once nothing else in the process is relying on MinHook
 * being initialized -- MH_Uninitialize() tears down every hook, not just
 * this one.
 */
void d3d11_hook_remove(void);

#ifdef __cplusplus
}
#endif
