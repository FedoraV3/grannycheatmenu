#include "game/unlock.h"
#include "game/offsets.h"
#include "core/patch_local.h"

#include <windows.h>

bool unlock_enabled = false;

/*
 * Requirement flags on HandlePuzzles: "you have what this needs".
 *
 * ONE ENTRY FOR NOW, DELIBERATELY. usedPadlockForPort is the only one traced
 * end to end through PickRay::Update, and the rest of the class is a wall of
 * similarly-named bools that fall into two groups which look alike and
 * behave nothing alike:
 *
 *   requirement -- usedPadlockForPort, UsedMaster, UsedSafeKey, UsedWPKey,
 *                  UsedCarKey, usedElecKey, usedPlayhouseKey, UsedPadlockKey,
 *                  UsedRustyPadlock, UsedSpecialKey, UsedChainCutter,
 *                  UsedCode, UsedBattery, usedSparkPlug, PlacedEnginePart,
 *                  PlacedCarBattery, PlacedCogwheels, PlacedMelon,
 *                  PlacedStick, usedPlank, PlacedUSB, fuseInPlace,
 *                  usedRemote, WheelCrankPlaced, CutWire1, CutWire2
 *
 *   already done -- openedPort, OpenedTank, FillingTank, IsRecharging,
 *                   RechargedRobot, GotShotgun, IsUpFully, RotatingWinch,
 *                   RotateFan, IsDoorFree, OpenedExLock, OpenedIronDoors,
 *                   SpiderCellarDone
 *
 * Setting one from the second group is worse than doing nothing: the branch
 * checks it FIRST and bails as already-complete, so the puzzle becomes
 * permanently uninteractable rather than unlocked.
 */
static const struct {
	const char *name;
	uintptr_t offset;
} g_requirement_flags[] = {
	{ "usedPadlockForPort", FIELD_HandlePuzzles_usedPadlockForPort },
};

#define UNLOCK_FLAG_COUNT ((int)(sizeof(g_requirement_flags) / sizeof(g_requirement_flags[0])))

/*
 * The "I need a ..." prompts are a different mechanism from the port's
 * HandlePuzzles bool, and there is no flag to set for them. Possession is
 * represented by whether the item's hand object is active, and every one of
 * these checks compiles to the same three instructions:
 *
 *     mov  rcx, [rbx+<hand object>]
 *     call UnityEngine.GameObject::get_activeSelf
 *     test al, al
 *     jnz  <the interaction>          ; in hand -> proceed
 *     ...  "I need a master key"
 *
 * So the patch is on the test, not the jump: `or al, 1` is the same two
 * bytes as `test al, al` and always clears ZF, which makes the jnz always
 * taken. al is the return value of get_activeSelf and is dead after the
 * branch, so clobbering it costs nothing, and keeping the length identical
 * avoids relocating the jump.
 *
 * Every entry was confirmed to sit directly between a get_activeSelf call
 * and a jnz that skips the message. That predicate check matters: several
 * sites share the identical jump shape while testing something else
 * entirely -- "I need a crossbow" and "break this camera" test the result of
 * a PickRay method, and "I need to get closer" tests Input::GetKeyDown, so
 * patching that one would have fired the interaction every frame instead of
 * unlocking anything.
 *
 * Note the table is keyed on the CHECK, not the message, and the checks were
 * found by their SHAPE rather than by the text they print.
 *
 * Keying off the failure messages was the original approach and it was
 * wrong twice over. A puzzle that accepts either of two items, or needs
 * several parts, emits one message from several tests -- and worse, plenty
 * of checks print nothing at all when they fail. The screwdriver is the
 * clearest case: four separate screw puzzles share one "I need a
 * screwdriver" string, so patching per message fixed the cellar screws and
 * left the other three silently doing nothing.
 *
 * So the scan anchors on the interaction shape instead. Every one of these
 * sits inside a window that begins with the click being consumed:
 *
 *     cmp [rbx+4D0h], r15b          ; PickRay.buttonClicked
 *     jz  done
 *     mov [rbx+4D0h], r15b          ; consume it
 *     mov rcx, [rbx+<hand object>]
 *     call UnityEngine.GameObject::get_activeSelf
 *     test al, al                   ; <-- patched here
 *     jnz <the interaction>
 *
 * All 52 were confirmed to have that exact form with a hand object read from
 * a PickRay field, which is what separates them from the other
 * get_activeSelf tests in this function -- "I need a crossbow" and "break
 * this camera" test a PickRay method's return, and "I need to get closer"
 * tests Input::GetKeyDown, so forcing that one would fire the interaction
 * every frame instead of unlocking anything.
 *
 * Still not covered, because they aren't possession checks: the weight plate
 * wants weight on it, the baton wants charge, "find a switch" wants world
 * state, and "get closer" is proximity.
 *
 * The two "hand +0x..." entries are checks whose item was never named by a
 * message -- the shape and the field are verified, only the label is a
 * guess, so they are labelled by offset rather than invented.
 */
#define UNLOCK_PATCH_SIZE 2
static const uint8_t g_force_taken[UNLOCK_PATCH_SIZE] = { 0x0C, 0x01 }; /* or al, 1 */

/* What has to be there first. Checked at every site, so a game update that
 * shifts the RVAs refuses to patch instead of stamping over 52 arbitrary
 * mid-function addresses and saving the wreckage as the originals. */
static const uint8_t g_expect_test[UNLOCK_PATCH_SIZE] = { 0x84, 0xC0 }; /* test al, al */

static struct {
	const char *name;
	uintptr_t rva;
	uint8_t original[UNLOCK_PATCH_SIZE];
} g_item_checks[] = {
	{ "spider key",            0x23A245, { 0 } },
	{ "wheel crank",           0x23A442, { 0 } },
	{ "lever",                 0x23A61B, { 0 } },
	{ "padlock key (spider)",  0x23A7CC, { 0 } },
	{ "pliers, wire 1",        0x23AA1F, { 0 } },
	{ "chain cutter, wire 1",  0x23AA3E, { 0 } },
	{ "pliers, wire 2",        0x23ABBE, { 0 } },
	{ "chain cutter, wire 2",  0x23ABDD, { 0 } },
	{ "pliers, wire 3",        0x23AD43, { 0 } },
	{ "chain cutter, wire 3",  0x23AD62, { 0 } },
	{ "weapon key",            0x23AEFD, { 0 } },
	{ "hand +0x358",           0x23B0FC, { 0 } },
	{ "battery",               0x23B2D5, { 0 } },
	{ "padlock key (1/2)",     0x23B4AB, { 0 } },
	{ "padlock key (2/2)",     0x23B67E, { 0 } },
	{ "cabinet key",           0x23BA69, { 0 } },
	{ "electric (1/2)",        0x23BD01, { 0 } },
	{ "car key",               0x23C264, { 0 } },
	{ "hand +0x2E0",           0x23C427, { 0 } },
	{ "plank",                 0x23C5E6, { 0 } },
	{ "special key",           0x23C7A5, { 0 } },
	{ "car battery",           0x23C9AC, { 0 } },
	{ "engine part",           0x23CB6B, { 0 } },
	{ "spark plug",            0x23CD82, { 0 } },
	{ "fuse",                  0x23CF95, { 0 } },
	{ "data container",        0x23D192, { 0 } },
	{ "painting piece 1",      0x23D38B, { 0 } },
	{ "painting piece 2",      0x23D3AA, { 0 } },
	{ "painting piece 3",      0x23D3C9, { 0 } },
	{ "painting piece 4",      0x23D3E8, { 0 } },
	{ "shotgun part 1",        0x23DAEF, { 0 } },
	{ "shotgun part 2",        0x23DB0E, { 0 } },
	{ "shotgun part 3",        0x23DB2D, { 0 } },
	{ "playhouse key",         0x23DD96, { 0 } },
	{ "padlock code",          0x23DF54, { 0 } },
	{ "hammer",                0x23E2D7, { 0 } },
	{ "master key",            0x23E57E, { 0 } },
	{ "chain cutter",          0x23E909, { 0 } },
	{ "safe key",              0x23EB0D, { 0 } },
	{ "gasoline can",          0x23FE38, { 0 } },
	{ "hand +0x380",           0x2400B1, { 0 } },
	{ "shotgun",               0x2404C2, { 0 } },
	{ "screwdriver (1/4)",     0x240DE8, { 0 } },
	{ "screwdriver (2/4)",     0x240FB2, { 0 } },
	{ "screwdriver (3/4)",     0x24117C, { 0 } },
	{ "screwdriver (4/4)",     0x241346, { 0 } },
	{ "cogwheel 1",            0x241B84, { 0 } },
	{ "cogwheel 2",            0x241BA3, { 0 } },
	{ "wrench",                0x241DD8, { 0 } },
	{ "electric (2/2)",        0x241F91, { 0 } },
	{ "book",                  0x24220A, { 0 } },
	{ "winch handle",          0x2423AB, { 0 } },
};

#define UNLOCK_CHECK_COUNT ((int)(sizeof(g_item_checks) / sizeof(g_item_checks[0])))

static bool g_patched = false;

int unlock_flag_count(void) {
	return UNLOCK_FLAG_COUNT;
}

int unlock_check_count(void) {
	return UNLOCK_CHECK_COUNT;
}

static bool object_alive(void *object) {
	if (!object) return false;
	return *(void **)((uintptr_t)object + FIELD_UnityObject_m_CachedPtr) != NULL;
}

bool unlock_apply(void) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) {
		unlock_enabled = false;
		return false;
	}

	/* Idempotent, for the same reason the death and trap patches are:
	 * patching twice would save our own bytes as the "original" ones. */
	if (unlock_enabled == g_patched) return true;

	if (unlock_enabled) {
		for (int i = 0; i < UNLOCK_CHECK_COUNT; i++) {
			if (patch_bytes_checked(base + g_item_checks[i].rva, g_expect_test, g_force_taken,
			                        UNLOCK_PATCH_SIZE, g_item_checks[i].original)) {
				continue;
			}

			/* The name identifies the check, not the item's message -- see the
			 * table -- so it isn't quoted as one. Failure here is nearly
			 * always a stale offset after a game update. */
			char line[128];
			wsprintfA(line, "[cheat] requirement check '%s' (0x%X) is not `test al, al`",
			          g_item_checks[i].name, (unsigned int)g_item_checks[i].rva);
			OutputDebugStringA(line);

			/* All or nothing: a half-patched set is harder to reason about in
			 * game than none of it working. */
			for (int j = 0; j < i; j++) {
				restore_bytes_local(base + g_item_checks[j].rva,
				                    g_item_checks[j].original, UNLOCK_PATCH_SIZE);
			}
			unlock_enabled = false;
			return false;
		}
		g_patched = true;
		OutputDebugStringA("[cheat] item requirement checks patched");
		return true;
	}

	/* The saved bytes are the only copy of those instructions, so a failed
	 * restore leaves the flag set -- clearing it would let the next enable
	 * save our own `or al, 1` as the original and strand the checks patched
	 * for the rest of the session. */
	bool restored = true;
	for (int i = 0; i < UNLOCK_CHECK_COUNT; i++) {
		if (!restore_bytes_local(base + g_item_checks[i].rva,
		                         g_item_checks[i].original, UNLOCK_PATCH_SIZE)) {
			restored = false;
		}
	}
	if (!restored) {
		OutputDebugStringA("[cheat] failed to restore the requirement checks, leaving them patched");
		unlock_enabled = true;
		return false;
	}

	g_patched = false;
	OutputDebugStringA("[cheat] item requirement checks restored");
	return true;
}

/* What the drop state looked like at Update's entry, so the hide an
 * interaction causes can be told apart from the one a real drop causes. */
static bool g_drop_was_active = false;
static bool g_click_pending = false;

/* activeSelf, not activeInHierarchy: the drop gate reads the object's own
 * flag, and Drop1 hangs off a UI canvas that may be switched off wholesale
 * on PC -- testing the hierarchy would be false forever while the game saw
 * true. */
static bool drop_active(uintptr_t base, void *drop) {
	return ((GameObject_get_active_t)(base + OFFSET_GameObject_get_activeSelf))(drop, NULL);
}

void unlock_pre_update(void *pickray) {
	g_click_pending = false;
	g_drop_was_active = false;
	if (!unlock_enabled || !object_alive(pickray)) return;

	/* Nothing to undo unless an interaction is about to be processed. */
	if (!*(volatile bool *)((uintptr_t)pickray + FIELD_PickRay_buttonClicked)) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	void *drop = *(void **)((uintptr_t)pickray + FIELD_PickRay_Drop1);
	if (!object_alive(drop)) return;

	g_click_pending = true;
	g_drop_was_active = drop_active(base, drop);
}

void unlock_post_update(void *pickray) {
	if (!g_click_pending || !g_drop_was_active) return;
	g_click_pending = false;
	if (!unlock_enabled || !object_alive(pickray)) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	void *drop = *(void **)((uintptr_t)pickray + FIELD_PickRay_Drop1);
	if (!object_alive(drop)) return;

	/* Was holding something, pressed interact, and the interaction took the
	 * drop button away. Give it back -- the item is still in hand. */
	if (!drop_active(base, drop)) {
		((Unity_set_bool_t)(base + OFFSET_GameObject_SetActive))(drop, true, NULL);
	}
}

void unlock_tick(void *pickray) {
	if (!unlock_enabled) return;
	if (!object_alive(pickray)) return;

	/* HandlePuzzles is a scene object like any other, and it is rebuilt on
	 * every level load -- so it's re-read from the PickRay each tick rather
	 * than latched, and checked before being written through. */
	void *puzzles = *(void **)((uintptr_t)pickray + FIELD_PickRay_HP);
	if (!object_alive(puzzles)) return;

	/* Held true rather than set once. The game owns these fields and sets
	 * them itself when you legitimately use an item, and a level load builds
	 * a fresh HandlePuzzles with all of them false -- so a single write
	 * would last only until the next reload. One byte store per tick. */
	for (int i = 0; i < UNLOCK_FLAG_COUNT; i++) {
		*(volatile bool *)((uintptr_t)puzzles + g_requirement_flags[i].offset) = true;
	}
}
