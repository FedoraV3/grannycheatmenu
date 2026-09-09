#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Where the config lives, as a printable path.
 *
 * %APPDATA%\grannycheat\config.ini. Not next to the DLL, which is
 * manual-mapped and so has no path of its own, and not next to the game,
 * which may sit somewhere unwritable.
 *
 * @return A static string. Never NULL, but empty if %APPDATA% is unset.
 */
const char *config_path(void);

/**
 * @brief Load saved settings over the current ones.
 *
 * Every setting is optional: anything missing from the file keeps the value
 * it already had, so a config written by an older build still loads and a
 * newly added feature just starts at its default.
 *
 * Only writes the variables. Settings whose effect needs more than a flag --
 * Immortality's byte patch, the trap patches -- are applied by
 * config_apply(), which must run after the hooks are installed.
 *
 * @return true if a file was found and read.
 */
bool config_load(void);

/**
 * @brief Make the game agree with the loaded settings.
 *
 * Separate from config_load() because it calls into MinHook and patches
 * code, neither of which is safe until the hooks exist. Call once from the
 * init thread after everything is installed.
 */
void config_apply(void);

/**
 * @brief Write the current settings out, creating the folder if needed.
 *
 * @return true if the file was written.
 */
bool config_save(void);

/** Result of the last load or save, for the menu to show. Never NULL. */
const char *config_status(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
