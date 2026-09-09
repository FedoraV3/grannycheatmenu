#include "game/spawn.h"
#include "game/offsets.h"

#include <windows.h>

/* Names for ItemSpawn's 55 slots, indexed by CountItem - 1. See
 * spawn_item_name()'s comment for why this order isn't dump.cs's.
 *
 * This is the canonical copy -- the item ESP labels dropped items from it
 * too, rather than keeping a second table that could drift out of step. */
static const char *const g_item_names[ITEMSPAWN_ITEM_COUNT] = {
	"Crossbow", "Pliers", "Battery", "Gas", "Bird Seed", "Book", "Winch",
	"Car Battery", "Car Key", "Chain Cutter", "Code", "Baton", "EC Key",
	"Hammer", "Padlock Key", "Master Key", "Cog 1", "Cog 2", "Meat",
	"Melon", "Spray", "Plank", "Playhouse Key", "Remote", "Robo Data",
	"Rusty Key", "Safe Key", "Screwdriver", "Shotgun", "Shotgun 2",
	"SP 1", "SP 2", "SP 3", "Spark Plug", "Special", "Spider", "Syringe",
	"T1", "T2", "T3", "T4", "Teddy", "Text", "Engine Part", "WP Key", "Vase",
	"Vase 2", "Vase 3", "Wheel Crank", "Wooden Stick", "Wrench", "Rat",
	"Freeze Ornament", "Bomb Ornament", "Fuse",
};

int spawn_selected_index = 0;

/* -1 when nothing is pending. Written by the menu thread, read and cleared
 * by the game thread; a single aligned int, so no lock is needed for the
 * handoff and the worst case is servicing it a frame later. */
static volatile int g_pending = -1;

/* Cursor for "spawn everything": the next slot to produce, or -1 when idle.
 * Spread over consecutive ticks rather than done in one go -- 55 droppers
 * instantiated in a single frame is a visible hitch, and 55 rigidbodies
 * appearing inside each other in one physics step makes them shove each
 * other through the floor. One per tick empties in about a second and the
 * items spill out instead. */
static volatile int g_bulk_cursor = -1;

static bool g_last_failed = false;

int spawn_item_count(void) {
	return ITEMSPAWN_ITEM_COUNT;
}

const char *spawn_item_name(int index) {
	if (index < 0 || index >= ITEMSPAWN_ITEM_COUNT) return "?";
	return g_item_names[index];
}

void spawn_request_item(int index) {
	if (index < 0 || index >= ITEMSPAWN_ITEM_COUNT) return;
	g_pending = index;
}

void spawn_request_all(void) {
	g_bulk_cursor = 0;
}

void spawn_cancel_all(void) {
	g_bulk_cursor = -1;
}

bool spawn_bulk_active(void) {
	return g_bulk_cursor >= 0;
}

int spawn_bulk_remaining(void) {
	int cursor = g_bulk_cursor;
	if (cursor < 0) return 0;
	return ITEMSPAWN_ITEM_COUNT - cursor;
}

bool spawn_last_failed(void) {
	return g_last_failed;
}

static bool object_alive(void *object) {
	if (!object) return false;
	return *(void **)((uintptr_t)object + FIELD_UnityObject_m_CachedPtr) != NULL;
}

/*
 * Produces one item, exactly the way PickRay::CheckItemDropping does:
 * instantiate the ItemDrop prefab at the player's drop point, then write
 * CountItem on the copy's ItemSpawn. Its own Update activates that item,
 * throws it, unparents it and destroys the dropper.
 *
 * Main thread only -- every call here is IL2CPP.
 */
static bool spawn_one(uintptr_t base, void *pickray, int index) {
	/* The prefab is an asset rather than a scene object, so it is never
	 * "active" -- but it is still a UnityEngine.Object, and an unloaded one
	 * would have a null m_CachedPtr like anything else. */
	void *prefab = *(void **)((uintptr_t)pickray + FIELD_PickRay_ItemDrop);
	void *drop_point = *(void **)((uintptr_t)pickray + FIELD_PickRay_DropP);
	if (!object_alive(prefab) || !object_alive(drop_point)) return false;

	/* Both baked MethodInfo globals hold a pointer to the real thing, so
	 * they need one dereference. Instantiate and the generic GetComponent
	 * won't accept NULL for these the way the plain engine methods do. */
	void *instantiate_method = *(void **)(base + METHODINFO_Instantiate_GameObject);
	void *get_component_method = *(void **)(base + METHODINFO_GetComponent_ItemSpawn);
	if (!instantiate_method || !get_component_method) return false;

	/* Spawn where the game drops things: right in front of the player, at
	 * the drop point's own rotation. Nothing to compute, and the item ends
	 * up somewhere reachable rather than inside the floor. */
	unity_vector3 position;
	unity_quaternion rotation;
	((Transform_get_position_t)(base + OFFSET_Transform_get_position))(&position, drop_point, NULL);
	((Transform_get_rotation_t)(base + OFFSET_Transform_get_rotation))(&rotation, drop_point, NULL);

	void *dropper = ((Object_Instantiate_t)(base + OFFSET_Object_Instantiate))(
	    prefab, &position, &rotation, instantiate_method);
	if (!object_alive(dropper)) return false;

	void *item_spawn = NULL;
	((GameObject_GetComponent_t)(base + OFFSET_GameObject_GetComponent_shared))(
	    dropper, &item_spawn, get_component_method);
	if (!object_alive(item_spawn)) return false;

	/* CountItem is a 1-based float. Writing it is the whole trick. */
	*(volatile float *)((uintptr_t)item_spawn + FIELD_ItemSpawn_CountItem) = (float)(index + 1);
	return true;
}

void spawn_tick(void *pickray) {
	const int single = g_pending;
	const int bulk = g_bulk_cursor;
	if (single < 0 && bulk < 0) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0 || !object_alive(pickray)) {
		/* Nothing can be spawned without a live player, and retrying every
		 * tick against a dead one would just log forever. Drop both
		 * requests and report it. */
		g_pending = -1;
		g_bulk_cursor = -1;
		g_last_failed = true;
		return;
	}

	/* Cleared per serviced request rather than only on a single spawn, so a
	 * bad slot during a bulk run doesn't leave the menu's failure line up for
	 * the rest of the session. */
	g_last_failed = false;

	if (single >= 0) {
		g_pending = -1;
		g_last_failed = !spawn_one(base, pickray, single);
		if (!g_last_failed) {
			char line[96];
			wsprintfA(line, "[cheat] spawned item %d (%s)", single + 1, g_item_names[single]);
			OutputDebugStringA(line);
		}
	}

	if (bulk >= 0) {
		if (bulk >= ITEMSPAWN_ITEM_COUNT) {
			g_bulk_cursor = -1;
			OutputDebugStringA("[cheat] spawned every item");
			return;
		}
		/* One per tick. A failure here doesn't abort the rest: a single
		 * slot refusing to spawn shouldn't cost you the other 54. */
		if (!spawn_one(base, pickray, bulk)) g_last_failed = true;
		g_bulk_cursor = bulk + 1;
	}
}
