#ifndef UNLOCK_H
#define UNLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bound to the World tab's "Unlock without keys" checkbox.
 *
 * Every "do you have the key" test in this game turns out to be a plain bool
 * on a single manager, HandlePuzzles, reached as PickRay.HP (+0x4D8) -- not
 * a comparison against whatever you happen to be holding. Traced from the
 * padlocked port, whose branch in PickRay::Update reads:
 *
 *     if (HP.openedPort)          goto done;      // already open
 *     ...
 *     if (!PickRay.buttonClicked) goto done;      // not an interact frame
 *     PickRay.buttonClicked = 0;
 *     if (HP.usedPadlockForPort)  HandlePuzzles::OpenPort();
 *     else                        text("It's locked");
 *
 * So setting the requirement flag doesn't open anything by itself -- it
 * makes your interact succeed instead of printing "It's locked", which is
 * exactly the behaviour asked for. The animation, audio and state changes
 * all still run through the game's own OpenPort().
 *
 * The "I need a ..." prompts work differently -- possession there is just
 * whether the item's hand object is active, with no flag to set -- so those
 * are handled by a byte patch instead. See unlock.c.
 */
extern bool unlock_enabled;

/**
 * @brief Push `unlock_enabled` into the game.
 *
 * Applies or lifts the byte patches over the item requirement checks.
 * Reverts the flag if the patch fails, so the menu never shows a state the
 * game isn't in. Safe from the menu thread: it writes to code pages only.
 *
 * @return true if the game is now in the requested state.
 */
bool unlock_apply(void);

/**
 * @brief Hold the requirement flags true. Main thread only.
 *
 * Call from the PickRay::Update hook, passing that hook's instance -- the
 * PickRay is the only route to HandlePuzzles.
 *
 * Also puts the drop button back when an interaction hid it while your
 * hands were still full. Every interaction calls Drop1.SetActive(false) as
 * part of "you used your item", and the drop key is gated on
 * Drop1.activeSelf -- so with the requirement checks forced, using something
 * while holding an unrelated item kills dropping until the next pickup.
 *
 * Restoring it unconditionally worked but left the drop prompt showing with
 * empty hands. It is keyed on the honest signal instead: the button going
 * from shown to hidden while a hand object is still active. A real drop
 * empties your hands first and is left alone.
 *
 * @param pickray The live PickRay, or NULL.
 */
void unlock_tick(void *pickray);

/** Locks covered by a HandlePuzzles flag -- currently just the port. */
int unlock_flag_count(void);

/** Item requirement checks covered by a byte patch (the "I need ..." set). */
int unlock_check_count(void);

#ifdef __cplusplus
}
#endif

#endif /* UNLOCK_H */
