#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	float x, y, z;
} esp_vec3;

/** Unity Matrix4x4, indexed [row][col] once copied out of the game. */
typedef struct {
	float m[4][4];
} esp_mat4;

/* Feature toggles -- the menu binds checkboxes straight to these. */
extern bool esp_granny_enabled;
extern bool esp_items_enabled;

/** Box around the cellar's Mom Spider, from its own AI_MomSpider::Update. */
extern bool esp_momspider_enabled;

/**
 * Off-screen direction arrows.
 *
 * A box only helps while the thing is in front of you, which is the case you
 * least need help with. When an enemy is off the edge of the screen -- or
 * behind you, where the projection would otherwise mirror it to the wrong
 * side entirely -- this puts an arrow on the screen edge pointing at them,
 * with the distance next to it.
 *
 * Applies to whichever enemies are switched on above; it doesn't add
 * markers of its own.
 */
extern bool esp_arrows_enabled;

/** Arrow size in pixels, and how far its ring sits from the screen edge. */
extern float esp_arrow_size;
extern float esp_arrow_margin;

/**
 * Debug aid: draw items that are alive but not active in the hierarchy.
 *
 * If nothing shows with this off but boxes appear at sensible world
 * positions with it on, the active check is the problem. If they instead
 * pile up at one spot or the origin, ItemSpawn's fields are prefab
 * templates rather than the live scene objects.
 */
extern bool esp_items_ignore_active;

/**
 * Per-category item filters, keyed on ItemSeedData::category -- the game's
 * own classification, per its tooltip: 1 = escape-only, 2 = escape+puzzle,
 * 3 = puzzle-only, anything else free/misc.
 *
 * These only bite when items come from the ItemSeedData registry. The
 * fallback sources have no category, so they all land in "other".
 */
extern bool esp_show_escape_items;
extern bool esp_show_escape_puzzle_items;
extern bool esp_show_puzzle_items;
extern bool esp_show_other_items;

/**
 * Debug aid: log each item slot to OutputDebugStringA before touching it.
 *
 * If the item walk crashes the game, DebugView's last line names the exact
 * slot that did it -- the log survives the crash, a breakpoint wouldn't.
 * Noisy (runs per collect), so leave it off unless chasing a crash.
 */
extern bool esp_items_verbose;

/**
 * Debug aid: stop the item walk after this many slots (1..55).
 *
 * Lets a crash be bisected by scan depth without a rebuild each time.
 */
extern int esp_item_scan_limit;

/**
 * Box dimensions for the Granny marker, in world units. Her transform sits
 * at her feet, so the box is drawn from there upward by esp_box_height.
 * Tunable at runtime because the right value depends on the model's actual
 * scale, which is easier to eyeball in game than to derive.
 */
extern float esp_box_height;
extern float esp_box_width_ratio;

/** The same, for the Mom Spider -- she's low and wide where Granny is tall
 *  and narrow, so one pair of numbers can't flatter both. */
extern float esp_spider_box_height;
extern float esp_spider_box_width_ratio;

/**
 * @brief Feed the camera's view-projection matrix for this frame.
 *
 * Unity's `Camera.projectionMatrix * Camera.worldToCameraMatrix`. Must be
 * called once per frame before esp_render(); until a real matrix arrives,
 * esp_world_to_screen() fails and the overlay draws nothing but a status
 * line.
 *
 * @param vp The combined view-projection matrix, or NULL to mark it stale.
 */
/**
 * @brief Initialise the ESP's internal lock. Call once before any hook that
 * can reach esp_collect() is enabled.
 *
 * The collected state crosses threads -- the game thread fills it, the
 * present thread draws it -- so it needs a lock, and that lock has to exist
 * before the first collection can run.
 */
void esp_init(void);

void esp_set_view_projection(const esp_mat4 *vp);

/**
 * @brief Project a world position to screen space.
 *
 * @param world  World-space position.
 * @param out_x  Receives the screen X in pixels.
 * @param out_y  Receives the screen Y in pixels.
 * @return true if the point is in front of the camera and on screen; false
 *         if it's behind the camera, off screen, or no matrix has been fed
 *         in yet (in which case out_x/out_y are untouched).
 */
bool esp_world_to_screen(esp_vec3 world, float *out_x, float *out_y);

/**
 * @brief Pull the camera matrix and entity positions out of the game.
 *
 * MUST be called from the game's main thread -- in practice from the
 * AI_Granny::FixedUpdate hook. IL2CPP methods expect a runtime-attached
 * thread, and calling them from the D3D11 present hook (a different thread
 * when Unity uses threaded rendering) can crash. So this half runs on the
 * game thread and only caches plain floats; esp_render() on the render
 * thread just reads what was cached.
 *
 * Because FixedUpdate ticks at the physics rate (50Hz) rather than the
 * frame rate, the cached camera matrix can be up to a frame or two stale,
 * which shows up as the overlay lagging slightly during fast camera
 * movement.
 *
 * @param granny_instance The live AI_Granny instance, or NULL if none.
 */
void esp_collect(void *granny_instance);

/**
 * @brief Cache the world positions of every item still lying in the level.
 *
 * Walks ItemSpawn's 55 contiguous GameObject* fields, skipping any whose
 * GameObject is inactive (the game deactivates an item once it's been
 * picked up). Same main-thread-only rule as esp_collect() -- call it from
 * the ItemSpawn::Update hook, never from the render thread.
 *
 * @param item_spawn The live ItemSpawn instance, or NULL if none.
 */
void esp_collect_items(void *item_spawn);

/**
 * @brief Hook ItemSpawn::Update so item positions can be sourced each frame.
 *
 * MinHook must already be initialized (d3d11_hook_install() does that) and
 * GameAssembly.dll must be loaded. The hook is a no-op while Item ESP is
 * switched off, so it costs nothing until you enable it.
 *
 * @return Nonzero on success.
 */
int esp_install_hooks(void);

/** Snapshot of the ESP's internal state, for the menu's Debug tab. */
typedef struct {
	bool have_view_projection;  /**< A camera matrix has been captured. */
	bool have_granny_position;  /**< Granny's transform position was read. */
	bool have_spider_position;  /**< The Mom Spider's position was read. */
	bool have_player_position;  /**< The player's own position, for distances. */
	int item_count;             /**< Items drawn (passed every filter). */
	int items_alive;            /**< Of 55 slots, how many are live objects. */
	int items_active;           /**< Of those, how many are active in a scene. */
	bool have_item_spawn;       /**< The ItemSpawn instance has been captured. */
	bool item_spawn_alive;      /**< ...and its native object still exists. */
	void *item_spawn;           /**< The captured instance pointer itself. */
	void *item_slot0;           /**< Raw pointer in the first item field (+0x28). */
	unsigned long granny_ticks; /**< AI_Granny::FixedUpdate hook call count. */
	unsigned long item_ticks;   /**< ItemRepositionSeed::Awake hook call count. */
	unsigned long pickray_ticks;/**< PickRay::Update hook call count (primary tick). */
	unsigned long spider_ticks; /**< AI_MomSpider::Update hook call count. */
} esp_debug_info;

/**
 * @brief Read the ESP's current state.
 *
 * Tells you which stage of the pipeline is failing: zero ticks means the
 * relevant hook never fired, ticks without a view projection means the
 * camera couldn't be resolved, and so on.
 *
 * @param out Receives the snapshot.
 */
void esp_get_debug_info(esp_debug_info *out);

/**
 * @brief Draw the enabled overlays for this frame.
 *
 * Call every frame between ImGui::NewFrame() and ImGui::Render(), whether
 * or not the menu is open -- ESP is meant to be visible while playing.
 * Renders into ImGui's background draw list so the menu window stays on
 * top of it.
 */
void esp_render(void);

#ifdef __cplusplus
}
#endif
