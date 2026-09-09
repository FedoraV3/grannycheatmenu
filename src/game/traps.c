#include "game/traps.h"
#include "game/offsets.h"
#include "core/patch_local.h"

#include <windows.h>

/*
 * `ret` (0xC3), not NOP. These are `void f(void *this, Collider *other)`
 * under the Win64 ABI -- the caller cleans up and nothing has been pushed at
 * entry, so returning immediately is safe. NOPing the first instruction
 * would only skip it and fall through into the rest of the function.
 */
#define TRAP_PATCH_SIZE 1
static const uint8_t g_ret_patch[TRAP_PATCH_SIZE] = { 0xC3 };

typedef struct {
	const char *name;
	uintptr_t rva;
	uint8_t original[TRAP_PATCH_SIZE];
} trap_patch;

static trap_patch g_traps[] = {
	{ "TrapTrigger",      OFFSET_TrapTrigger_OnTriggerEnter,      { 0 } },
	{ "BearTrapLogic",    OFFSET_BearTrapLogic_OnTriggerEnter,    { 0 } },
	{ "ExploTrapTrigger", OFFSET_ExploTrapTrigger_OnTriggerEnter, { 0 } },
	{ "TrapPoison",       OFFSET_TrapPoison_OnTriggerEnter,       { 0 } },
};

#define TRAP_COUNT ((int)(sizeof(g_traps) / sizeof(g_traps[0])))

bool traps_disabled = false;

static bool g_patched = false;

bool traps_apply(void) {
	if (traps_set_disabled(traps_disabled)) return true;

	OutputDebugStringA("[cheat] trap patch failed, reverting toggle");
	traps_disabled = !traps_disabled;
	return false;
}

bool traps_set_disabled(bool disabled) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return false;

	/* Idempotent: patching twice would save the `ret` we wrote as the
	 * "original" byte and make the restore a no-op forever. */
	if (disabled == g_patched) return true;

	if (disabled) {
		for (int i = 0; i < TRAP_COUNT; i++) {
			if (patch_bytes_local(base + g_traps[i].rva, g_ret_patch, TRAP_PATCH_SIZE,
			                      g_traps[i].original)) {
				continue;
			}

			char line[96];
			wsprintfA(line, "[cheat] failed to patch %s::OnTriggerEnter", g_traps[i].name);
			OutputDebugStringA(line);

			/* Don't leave the level with some traps live and some not --
			 * that's harder to reason about in game than none of it working. */
			for (int j = 0; j < i; j++) {
				restore_bytes_local(base + g_traps[j].rva, g_traps[j].original, TRAP_PATCH_SIZE);
			}
			return false;
		}
		g_patched = true;
		OutputDebugStringA("[cheat] trap triggers patched to ret");
		return true;
	}

	/* Restore. If any one write fails the saved originals are still the only
	 * copy of those bytes, so the patched flag stays set: clearing it would
	 * let the next enable overwrite them with the 0xC3 that is still sitting
	 * there, losing the real opcodes for the rest of the session. */
	bool all_restored = true;
	for (int i = 0; i < TRAP_COUNT; i++) {
		if (!restore_bytes_local(base + g_traps[i].rva, g_traps[i].original, TRAP_PATCH_SIZE)) {
			char line[96];
			wsprintfA(line, "[cheat] failed to restore %s::OnTriggerEnter", g_traps[i].name);
			OutputDebugStringA(line);
			all_restored = false;
		}
	}
	if (!all_restored) return false;

	g_patched = false;
	OutputDebugStringA("[cheat] trap triggers restored");
	return true;
}
