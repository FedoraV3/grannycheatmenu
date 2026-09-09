#ifndef SPAWN_H
#define SPAWN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many items the dropper can produce -- ItemSpawn's 55 slots. */
int spawn_item_count(void);

/**
 * @brief Display name for a spawnable item.
 *
 * Indexed 0..spawn_item_count()-1, which is CountItem minus one.
 *
 * The order comes from ItemSpawn::Update's dispatch chain, NOT from
 * dump.cs's field declaration order -- the two disagree. The decompile maps
 * 29 to shotgun and 30 to shotgun2 (dump.cs declares shotgun2 after sp3),
 * and 53 to ornamentfreeze before 54 ornamentbomb (dump.cs has bomb first).
 * Taking the declaration order mislabelled slots 29-32 and 52-53.
 *
 * @return A static string, never NULL for a valid index.
 */
const char *spawn_item_name(int index);

/** Which item the menu's Spawn button and its keybind will produce. Lives
 *  here rather than in the menu so the config can save it. */
extern int spawn_selected_index;

/**
 * @brief Ask for an item to be spawned at the player's drop point.
 *
 * Safe to call from the menu thread: it only records the request. The work
 * happens on the next PickRay::Update tick, because Instantiate is an IL2CPP
 * call and those are main-thread only.
 *
 * Only one request can be outstanding at a time -- a second call before the
 * first is serviced replaces it rather than queueing, which is what you want
 * from a button that can be clicked faster than the game ticks.
 *
 * @param index 0-based item index, as passed to spawn_item_name().
 */
void spawn_request_item(int index);

/**
 * @brief Service a pending spawn request. Main thread only.
 *
 * Call from the PickRay::Update hook, passing that hook's instance -- the
 * PickRay is where both the dropper prefab and the drop point live.
 *
 * @param pickray The live PickRay, or NULL.
 */
void spawn_tick(void *pickray);

/**
 * @brief Ask for one of every item in the game.
 *
 * Serviced one item per tick rather than all in one go: 55 droppers
 * instantiated in a single frame is a visible hitch, and 55 rigidbodies
 * appearing inside one another in a single physics step shove each other
 * through the floor. Spread out, the pile empties in about a second and the
 * items spill instead. Calling it again while it's running restarts it.
 */
void spawn_request_all(void);

/** Abandon a running bulk spawn. */
void spawn_cancel_all(void);

/** Whether a bulk spawn is still working through the list. */
bool spawn_bulk_active(void);

/** How many items a running bulk spawn has left, or 0 when idle. */
int spawn_bulk_remaining(void);

/** Whether the last serviced request succeeded, for the menu to report. */
bool spawn_last_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* SPAWN_H */
