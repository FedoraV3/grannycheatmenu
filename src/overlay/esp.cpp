#include "overlay/esp.h"
#include "game/ai_granny_hook.h"
#include "game/fullbright.h"
#include "game/offsets.h"
#include "game/spawn.h"
#include "MinHook.h"

#include <windows.h>
#include <math.h>

#include "imgui.h"

bool esp_granny_enabled = false;
bool esp_items_enabled = false;
bool esp_momspider_enabled = false;
bool esp_arrows_enabled = true;
float esp_arrow_size = 26.0f;
float esp_arrow_margin = 60.0f;
bool esp_items_ignore_active = false;
bool esp_items_verbose = false;

/* Per-category filters, using the game's own classification from
 * ItemSeedData::category. */
bool esp_show_escape_items = true;
bool esp_show_escape_puzzle_items = true;
bool esp_show_puzzle_items = true;
bool esp_show_other_items = true;
int esp_item_scan_limit = ITEMSEED_ITEM_COUNT;

/* Tuned by eye in game -- Granny's model is a good deal taller in world
 * units than a stock Unity humanoid, hence the large height. */
float esp_box_height = 4.65f;
float esp_box_width_ratio = 0.50f;

/* The Mom Spider is a low, wide thing where Granny is tall and narrow, so
 * she gets her own pair rather than being drawn as a squashed Granny. */
float esp_spider_box_height = 1.60f;
float esp_spider_box_width_ratio = 1.80f;

static esp_mat4 g_view_projection;
static bool g_have_view_projection = false;

static esp_vec3 g_granny_position;
static bool g_have_granny_position = false;

static esp_vec3 g_spider_position;
static bool g_have_spider_position = false;

/* The player's own world position, taken from the PickRay's transform -- it
 * is a component on the player, so its transform is the player's. Only used
 * for the distance shown beside a direction arrow. */
static esp_vec3 g_player_position;
static bool g_have_player_position = false;

/* Names for ItemRepositionSeed's 35 Transform* fields, in declaration order
 * -- index N is the field at ITEMSEED_FIRST_ITEM_FIELD + N * 8. */
static const char *const g_item_names[ITEMSEED_ITEM_COUNT] = {
	"Pliers", "Master Key", "Hammer", "PD Key", "Code", "Safe Key",
	"WP Key", "Battery", "Winch", "Melon", "Playhouse Key", "Red Cog",
	"Orange Cog", "Barrel", "Buttstock", "Trigger", "Car Key",
	"Spark Plug", "Gas", "Engine", "Car Battery", "Wrench", "Book",
	"Meat", "SP Key", "Remote", "Bird Seed", "Wheel Crank",
	"Chain Cutter", "Wooden Stick", "Rusty Key", "Robo Data", "Baton",
	"EC Key", "Fuse",
};

/* Dropped-item names now live in spawn.c, which needs the same table to
 * fill the spawn list -- one copy, keyed to ItemSpawn's dispatch order,
 * rather than two that can drift apart. */

/* GameObject per dropped item, indexed by CountItem - 1 so re-dropping the
 * same item overwrites its slot instead of accumulating duplicates.
 *
 * These pointers are NOT rooted by anything in the game once the item is
 * picked back up and destroyed, so IL2CPP's GC is free to reclaim and reuse
 * the block. m_CachedPtr alone can't detect that -- whatever object lands
 * there next has its own non-zero m_CachedPtr, so the check passes and we
 * call GameObject methods on something that isn't one. See
 * gameobject_still_valid(). */
static void *volatile g_dropped[ITEMSPAWN_ITEM_COUNT];

/* Il2CppClass* of GameObject, captured from an object we know is a live
 * GameObject. Cheaper than resolving the TypeInfo global, and exact. */
static void *volatile g_gameobject_klass = NULL;

/* Same idea for ItemSeedData, captured in its .ctor hook where the object
 * is unambiguously one. Without this, a stale registry entry whose block
 * the GC has reused passes the liveness check and gets called into. */
static void *volatile g_item_seed_data_klass = NULL;

/* And for ItemRepositionSeed, captured in its Awake hook. g_item_spawn
 * outlives its object the same way a dropped-item slot does: nothing in the
 * game roots it, so once the level tears it down the GC is free to hand that
 * block to something else, whose own non-zero m_CachedPtr then makes the
 * stale pointer look perfectly alive. */
static void *volatile g_item_seed_klass = NULL;


/* Live ItemSeedData components, maintained by the .ctor/OnDestroy hooks.
 * This is the complete item set; the ItemRepositionSeed and dropped-item
 * walks below are only a fallback for when it comes up empty. */
#define ESP_MAX_SEED_ITEMS 192
static void *g_seed_items[ESP_MAX_SEED_ITEMS];
static int g_seed_item_count = 0;

/* Long enough for the game's longest item name plus the spaces the
 * prettifier inserts ("Shotgun_Buttstock" -> "Shotgun Buttstock"). */
#define ESP_ITEM_NAME_MAX 40

typedef struct {
	esp_vec3 position;
	const char *name;   /**< static table entry, when from the fallback path */
	char owned_name[ESP_ITEM_NAME_MAX]; /**< from ItemSeedData::itemName otherwise */
	int category;        /**< 1 escape, 2 escape+puzzle, 3 puzzle, else free */
} esp_item;

/* Sized for the ItemSeedData registry, which is larger than the fallback
 * lists combined. */
#define ESP_MAX_ITEMS ESP_MAX_SEED_ITEMS

static esp_item g_items[ESP_MAX_ITEMS];
static int g_item_count = 0;
static int g_items_alive = 0;
static int g_items_active = 0;

static unsigned long g_granny_ticks = 0;
static unsigned long g_item_ticks = 0;
static unsigned long g_pickray_ticks = 0;
static unsigned long g_spider_ticks = 0;

/* The live AI_MomSpider, captured from its Update. Cleared the moment it
 * stops passing the liveness check -- the cellar unloads and this becomes a
 * pointer to a destroyed object like any other. */
static void *volatile g_momspider = NULL;

/* Il2CppClass* of AI_MomSpider, captured in its Update where the object is
 * unambiguously one. Liveness alone can't tell a reused block apart -- the
 * new occupant has its own non-zero m_CachedPtr -- so without this the
 * marker would stay pinned at the last cellar position once the level
 * unloaded and the GC handed that memory to something else. */
static void *volatile g_momspider_klass = NULL;

/* The ItemRepositionSeed instance, latched at its Awake. It holds the
 * level's 35 item Transforms and persists, unlike ItemSpawn which destroys
 * itself. Walked on the FixedUpdate tick. */
static void *volatile g_item_spawn = NULL;

/* The player's PickRay, latched from its Update. Drives the ESP whenever
 * Granny is switched off and AI_Granny::FixedUpdate never fires. */
static void *volatile g_pickray = NULL;

/* Guards the state handed from the game thread (which collects) to the
 * render thread (which draws): g_items/g_item_count, the view-projection
 * matrix, and Granny's position. Without it a reader can see an entry
 * half-written -- a torn 12-byte position or 64-byte matrix draws markers
 * at wrong positions for a frame.
 *
 * Held only across the publish/read, never across the IL2CPP walk, so the
 * render thread is never blocked for long.
 *
 * Lock order where both are held: the ImGui lock is taken first (in
 * render_frame), then this one inside esp_render. The game thread only ever
 * takes one or the other, so the orders can't invert. */
static CRITICAL_SECTION g_esp_lock;
static bool g_esp_lock_ready = false;

static void esp_lock(void) {
	if (g_esp_lock_ready) EnterCriticalSection(&g_esp_lock);
}

static void esp_unlock(void) {
	if (g_esp_lock_ready) LeaveCriticalSection(&g_esp_lock);
}

void esp_init(void) {
	if (g_esp_lock_ready) return;
	InitializeCriticalSection(&g_esp_lock);
	g_esp_lock_ready = true;
}

/* A UnityEngine.Object whose native side has been destroyed keeps its
 * managed wrapper, so the pointer still looks valid from C -- only
 * m_CachedPtr going NULL reveals it. Calling into one of those dereferences
 * a freed native object and crashes the game, so every Unity object has to
 * pass through here before we touch it. */
static bool unity_object_alive(void *object) {
	if (!object) return false;
	return *(void **)((uintptr_t)object + FIELD_UnityObject_m_CachedPtr) != NULL;
}

/* Every IL2CPP object stores its Il2CppClass* at offset 0, so a block the
 * GC has reused almost always shows a different class here. Combined with
 * the liveness check, this makes a dangling dropped-item pointer safe to
 * skip rather than fatal to dereference.
 *
 * Not airtight: if the GC reuses the block for another GameObject this
 * passes and the item draws at a wrong position. A cosmetic glitch instead
 * of a crash is the trade being made. */
static bool gameobject_still_valid(void *object) {
	if (!unity_object_alive(object)) return false;
	if (!g_gameobject_klass) return true; /* nothing to compare against yet */
	return *(void **)object == g_gameobject_klass;
}

/* The registry's equivalent. Entries can outlive their object when OnDestroy
 * doesn't reach us (scene teardown ordering), and liveness alone can't tell
 * a reused block apart -- the new occupant has its own non-zero
 * m_CachedPtr. */
static bool item_seed_data_still_valid(void *component) {
	if (!unity_object_alive(component)) return false;
	if (!g_item_seed_data_klass) return true; /* nothing to compare against yet */
	return *(void **)component == g_item_seed_data_klass;
}

/* The latched ItemRepositionSeed's equivalent of gameobject_still_valid().
 * Without it, a reused block is walked as if it held 35 Transform pointers
 * and every plausible-looking qword in it gets called into -- which is
 * exactly how latching ItemSpawn crashed the game on v1.8. */
static bool item_seed_still_valid(void *instance) {
	if (!unity_object_alive(instance)) return false;
	if (!g_item_seed_klass) return true; /* nothing to compare against yet */
	return *(void **)instance == g_item_seed_klass;
}

void esp_get_debug_info(esp_debug_info *out) {
	if (!out) return;
	esp_lock();
	out->have_view_projection = g_have_view_projection;
	out->have_granny_position = g_have_granny_position;
	out->have_spider_position = g_have_spider_position;
	out->have_player_position = g_have_player_position;
	out->item_count = g_item_count;
	out->items_alive = g_items_alive;
	out->items_active = g_items_active;
	out->have_item_spawn = (g_item_spawn != NULL);
	out->item_spawn = g_item_spawn;
	out->item_spawn_alive = item_seed_still_valid(g_item_spawn);
	/* Only read the slot through a pointer that passed both checks -- on a
	 * reused block this field is somebody else's data. */
	out->item_slot0 = out->item_spawn_alive
	                      ? *(void **)((uintptr_t)g_item_spawn + ITEMSEED_FIRST_ITEM_FIELD)
	                      : NULL;
	out->granny_ticks = g_granny_ticks;
	out->item_ticks = g_item_ticks;
	out->pickray_ticks = g_pickray_ticks;
	out->spider_ticks = g_spider_ticks;
	esp_unlock();
}

void esp_set_view_projection(const esp_mat4 *vp) {
	esp_lock();
	if (vp) {
		g_view_projection = *vp;
		g_have_view_projection = true;
	} else {
		g_have_view_projection = false;
	}
	esp_unlock();
}

/* Shared by the box projection and the arrows. The arrows need the raw clip
 * coordinates, because "behind the camera" is a case they handle rather than
 * reject -- so the division and the range checks live in the callers. */
static bool world_to_clip(esp_vec3 world, float *out_x, float *out_y, float *out_w) {
	if (!g_have_view_projection) return false;

	const float(*m)[4] = g_view_projection.m;

	float clip_x = m[0][0] * world.x + m[0][1] * world.y + m[0][2] * world.z + m[0][3];
	float clip_y = m[1][0] * world.x + m[1][1] * world.y + m[1][2] * world.z + m[1][3];
	float clip_w = m[3][0] * world.x + m[3][1] * world.y + m[3][2] * world.z + m[3][3];

	/* NaN compares false against everything, so it would slip past every
	 * range check downstream and reach ImGui as a garbage vertex -- which
	 * blows out the draw list's 16-bit index buffer and trips an assert deep
	 * inside ImGui rather than here. Reject it explicitly. */
	if (!isfinite(clip_x) || !isfinite(clip_y) || !isfinite(clip_w)) return false;

	*out_x = clip_x;
	*out_y = clip_y;
	*out_w = clip_w;
	return true;
}

bool esp_world_to_screen(esp_vec3 world, float *out_x, float *out_y) {
	float clip_x, clip_y, clip_w;
	if (!world_to_clip(world, &clip_x, &clip_y, &clip_w)) return false;

	/* Behind the camera (or right on the plane) -- projecting would mirror
	 * the point to the wrong side of the screen. */
	if (clip_w < 0.1f) return false;

	float ndc_x = clip_x / clip_w;
	float ndc_y = clip_y / clip_w;

	ImVec2 screen = ImGui::GetIO().DisplaySize;
	float x = (screen.x * 0.5f) * (1.0f + ndc_x);
	float y = (screen.y * 0.5f) * (1.0f - ndc_y);

	if (!isfinite(x) || !isfinite(y)) return false;
	if (x < 0.0f || y < 0.0f || x > screen.x || y > screen.y) return false;

	*out_x = x;
	*out_y = y;
	return true;
}

/* ItemSeedData::category, per the dev tooltip. Anything outside 1..3 is
 * treated as free/misc -- the tooltip labels 3 twice, so the intended
 * fourth value is a guess and shouldn't be relied on. */
static bool esp_category_shown(int category) {
	switch (category) {
	case 1: return esp_show_escape_items;
	case 2: return esp_show_escape_puzzle_items;
	case 3: return esp_show_puzzle_items;
	default: return esp_show_other_items;
	}
}

static ImU32 esp_category_color(int category) {
	switch (category) {
	case 1: return IM_COL32(120, 255, 140, 255); /* escape-only  -- green */
	case 2: return IM_COL32(255, 220, 110, 255); /* escape+puzzle -- amber */
	case 3: return IM_COL32(150, 190, 255, 255); /* puzzle-only  -- blue  */
	default: return IM_COL32(190, 190, 190, 255); /* free / misc  -- grey  */
	}
}

/* Box + label at a projected world position. Height is in world units and
 * projected separately so the box shrinks with distance on its own. */
static void draw_entity(esp_vec3 feet, float height, float width_ratio, const char *label,
                        ImU32 color) {
	float foot_x, foot_y, head_x, head_y;
	if (!esp_world_to_screen(feet, &foot_x, &foot_y)) return;

	esp_vec3 head = feet;
	head.y += height;
	if (!esp_world_to_screen(head, &head_x, &head_y)) return;

	float box_height = foot_y - head_y;
	float box_width = box_height * width_ratio;

	ImDrawList *draw = ImGui::GetBackgroundDrawList();
	ImVec2 top_left(foot_x - box_width * 0.5f, head_y);
	ImVec2 bottom_right(foot_x + box_width * 0.5f, foot_y);

	/* Dark outline under the real box so it stays readable against both
	 * Granny's white dress and the dark walls. */
	draw->AddRect(ImVec2(top_left.x - 1, top_left.y - 1),
	              ImVec2(bottom_right.x + 1, bottom_right.y + 1),
	              IM_COL32(0, 0, 0, 180), 0.0f, 0, 3.0f);
	draw->AddRect(top_left, bottom_right, color, 0.0f, 0, 1.5f);

	if (label) {
		ImVec2 text_size = ImGui::CalcTextSize(label);
		ImVec2 text_pos(foot_x - text_size.x * 0.5f, head_y - text_size.y - 2.0f);
		draw->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 200), label);
		draw->AddText(text_pos, color, label);
	}
}


/* Straight-line distance from the player, or -1 when we haven't got a player
 * position this frame. */
static float distance_from_player(esp_vec3 world) {
	if (!g_have_player_position) return -1.0f;

	const float dx = world.x - g_player_position.x;
	const float dy = world.y - g_player_position.y;
	const float dz = world.z - g_player_position.z;
	const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
	return isfinite(distance) ? distance : -1.0f;
}

/*
 * An arrow on the screen edge pointing at something you can't see.
 *
 * A box is only useful once the thing is already in front of you. This
 * covers the rest: off the side of the screen, or behind you.
 *
 * Behind the camera is the case that needs care. There, clip w goes
 * negative and dividing by it mirrors the point through the origin -- an
 * enemy directly behind and to your left projects to the upper right, which
 * is why a naive implementation points confidently the wrong way. So the
 * division is skipped entirely when w is negative and the clip x/y are
 * negated instead; only the direction matters, not the magnitude, because
 * the vector is normalised straight afterwards.
 *
 * The arrow then sits on an ellipse inset from the screen edge, which keeps
 * it fully visible in every direction without the corner special-casing a
 * rectangle would need.
 */
static void draw_direction_arrow(esp_vec3 world, ImU32 color, const char *name) {
	float clip_x, clip_y, clip_w;
	if (!world_to_clip(world, &clip_x, &clip_y, &clip_w)) return;

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	if (!(screen.x > 0.0f) || !(screen.y > 0.0f)) return;

	float ndc_x, ndc_y;
	if (clip_w > 0.1f) {
		ndc_x = clip_x / clip_w;
		ndc_y = clip_y / clip_w;
		/* On screen already -- the box has it covered, and an arrow pointing
		 * at something you can see is just clutter. */
		if (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f) return;
	} else if (clip_w >= 0.0f) {
		/* In front of the camera but too close to divide by. The direction is
		 * still correct here, only the magnitude blows up -- and the vector is
		 * normalised below, so the raw clip values do fine. Lumping this in
		 * with the negation below pointed the arrow directly away from an
		 * enemy standing on top of you, which is the one moment it matters. */
		ndc_x = clip_x;
		ndc_y = clip_y;
	} else {
		/* Genuinely behind: a negative w mirrors the point through the origin,
		 * so the sign has to come back off before the direction means
		 * anything. */
		ndc_x = -clip_x;
		ndc_y = -clip_y;
	}

	/* Screen y grows downward, clip y grows upward. */
	float dir_x = ndc_x;
	float dir_y = -ndc_y;
	const float length = sqrtf(dir_x * dir_x + dir_y * dir_y);
	if (!isfinite(length) || length < 1e-4f) return;
	dir_x /= length;
	dir_y /= length;

	const float centre_x = screen.x * 0.5f;
	const float centre_y = screen.y * 0.5f;
	const float radius_x = centre_x - esp_arrow_margin;
	const float radius_y = centre_y - esp_arrow_margin;
	/* A margin wider than the window would put the ring behind the screen
	 * centre and flip every arrow. */
	if (radius_x < 16.0f || radius_y < 16.0f) return;

	const float x = centre_x + dir_x * radius_x;
	const float y = centre_y + dir_y * radius_y;
	if (!isfinite(x) || !isfinite(y)) return;

	const float size = esp_arrow_size;
	const float half = size * 0.5f;
	/* Perpendicular to the heading, for the two base corners. */
	const float perp_x = -dir_y;
	const float perp_y = dir_x;

	const ImVec2 tip(x + dir_x * half, y + dir_y * half);
	const float base_x = x - dir_x * half;
	const float base_y = y - dir_y * half;
	const ImVec2 left(base_x + perp_x * half * 0.8f, base_y + perp_y * half * 0.8f);
	const ImVec2 right(base_x - perp_x * half * 0.8f, base_y - perp_y * half * 0.8f);

	ImDrawList *draw = ImGui::GetBackgroundDrawList();
	draw->AddTriangleFilled(tip, left, right, color);
	draw->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 200), 2.0f);

	/* Label inside the arrow, toward the middle of the screen, so it never
	 * runs off the edge the arrow is pinned to. */
	char label[64];
	const float distance = distance_from_player(world);
	if (distance >= 0.0f) {
		wsprintfA(label, "%s %dm", name, (int)(distance + 0.5f));
	} else {
		lstrcpynA(label, name, (int)sizeof(label));
	}

	const ImVec2 text_size = ImGui::CalcTextSize(label);
	const ImVec2 text_pos(x - dir_x * size - text_size.x * 0.5f,
	                      y - dir_y * size - text_size.y * 0.5f);
	draw->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), IM_COL32(0, 0, 0, 200), label);
	draw->AddText(text_pos, color, label);
}

/* Unity stores Matrix4x4 column-major: element (row, col) is raw[col*4+row].
 * out = a * b, written back in [row][col] order for esp_world_to_screen(). */
static void multiply_unity_matrices(const float *a, const float *b, esp_mat4 *out) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += a[k * 4 + row] * b[col * 4 + k];
			}
			out->m[row][col] = sum;
		}
	}
}

/* PlayerStatus holds PlayerCam, so any object that references PlayerStatus
 * is a route to the camera. */
static void *camera_via_player_status(void *player_status) {
	if (!unity_object_alive(player_status)) return NULL;
	void *camera = *(void **)((uintptr_t)player_status + FIELD_PlayerStatus_PlayerCam);
	return unity_object_alive(camera) ? camera : NULL;
}

/* Three routes, cheapest and most reliable first:
 *
 *   1. Camera.main -- returns NULL in this game, which never tags its
 *      camera "MainCamera", but costs nothing to try.
 *   2. PickRay -> PlayerStatus -> PlayerCam. PickRay is the player's own
 *      script, so this works even with Granny switched off in the game
 *      options (which is exactly when route 3 dies).
 *   3. AI_Granny -> PlayerStatus -> PlayerCam, as a last resort.
 *
 * Routes 2 and 3 are pure pointer derefs -- no tags, no IL2CPP calls. */
static void *resolve_camera(uintptr_t base) {
	void *camera = ((Camera_get_main_t)(base + OFFSET_Camera_get_main))(NULL);
	if (unity_object_alive(camera)) return camera;

	void *pickray = g_pickray;
	if (unity_object_alive(pickray)) {
		camera = camera_via_player_status(
		    *(void **)((uintptr_t)pickray + FIELD_PickRay_PlayerStatus));
		if (camera) return camera;
	}

	void *granny = ai_granny_current();
	if (!unity_object_alive(granny)) return NULL;

	return camera_via_player_status(
	    *(void **)((uintptr_t)granny + FIELD_AI_Granny_PlayerStatus));
}

/* Main thread only. Refreshed from both hooks so the overlay still has a
 * camera when only one of them is ticking. */
static bool refresh_camera(uintptr_t base) {
	void *camera = resolve_camera(base);
	if (!camera) {
		/* No camera reachable right now (menu screen, loading, scene swap). */
		esp_set_view_projection(NULL);
		return false;
	}

	float world_to_camera[16];
	float projection[16];
	Camera_get_matrix_t get_matrix = (Camera_get_matrix_t)(base + OFFSET_Camera_get_worldToCameraMatrix);
	get_matrix(world_to_camera, camera, NULL);
	get_matrix = (Camera_get_matrix_t)(base + OFFSET_Camera_get_projectionMatrix);
	get_matrix(projection, camera, NULL);

	esp_mat4 view_projection;
	multiply_unity_matrices(projection, world_to_camera, &view_projection);

	/* Sanity-check what came back before trusting it. An all-zero or
	 * non-finite matrix means the camera wasn't really initialised (or the
	 * calling convention is wrong) -- better to report no camera than to
	 * feed garbage into the projection. */
	bool any_nonzero = false;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float value = view_projection.m[row][col];
			if (!isfinite(value)) {
				esp_set_view_projection(NULL);
				return false;
			}
			if (value != 0.0f) any_nonzero = true;
		}
	}
	if (!any_nonzero) {
		esp_set_view_projection(NULL);
		return false;
	}

	esp_set_view_projection(&view_projection);
	return true;
}

void esp_collect(void *granny_instance) {
	g_granny_ticks++;

	/* Don't pay for IL2CPP calls every physics tick when nothing is drawn. */
	if (!esp_granny_enabled && !esp_items_enabled) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	if (!refresh_camera(base)) {
		esp_lock();
		g_have_granny_position = false;
		esp_unlock();
		return;
	}

	esp_vec3 position;
	bool have_position = false;
	if (unity_object_alive(granny_instance)) {
		void *transform =
		    ((Component_get_transform_t)(base + OFFSET_Component_get_transform))(granny_instance, NULL);
		if (unity_object_alive(transform)) {
			((Transform_get_position_t)(base + OFFSET_Transform_get_position))(&position, transform, NULL);
			have_position = true;
		}
	}

	/* Published together so the render thread never sees the flag set
	 * against a half-written position. */
	esp_lock();
	if (have_position) g_granny_position = position;
	g_have_granny_position = have_position;
	esp_unlock();

	/* Items are collected on the PickRay tick instead -- that one keeps
	 * running when Granny is switched off, and this hook doesn't. */
}

/* Walks ItemSpawn's item pointers. Driven from esp_collect() on the
 * FixedUpdate tick, not from the ItemSpawn hook -- that one only fires on
 * pickup/drop, so positions would almost never refresh. */
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Copies an IL2CPP System.String into `out`, splitting the game's PascalCase
 * item names into readable words.
 *
 * The names are written for code, not display: "WoodenStick", "CarBattery",
 * "Shotgun_Buttstock", "GRVase", "Vase2". Rather than a lookup table -- which
 * would only cover the names we happen to have seen -- the split is derived:
 *
 *   underscore                      -> space      Shotgun_Trigger / Shotgun Trigger
 *   upper after lower               -> space      WoodenStick     / Wooden Stick
 *   upper starting a word after an
 *     acronym run                   -> space      GRVase          / GR Vase
 *   digit after a letter            -> space      Vase2           / Vase 2
 *
 * Item names are plain ASCII, so anything outside that range becomes '?'. */
static void il2cpp_string_to_ascii(void *string_object, char *out, int out_size) {
	out[0] = '\0';
	if (!string_object || out_size <= 0) return;

	int length = *(int *)((uintptr_t)string_object + IL2CPP_STRING_LENGTH_OFFSET);
	if (length <= 0) return;

	const wchar_t *chars = (const wchar_t *)((uintptr_t)string_object + IL2CPP_STRING_CHARS_OFFSET);
	int written = 0;

	for (int i = 0; i < length && written < out_size - 1; i++) {
		wchar_t wide = chars[i];
		char c = (wide >= 0x20 && wide < 0x7F) ? (char)wide : '?';

		if (c == '_') {
			if (written > 0 && out[written - 1] != ' ') out[written++] = ' ';
			continue;
		}

		if (written > 0 && out[written - 1] != ' ') {
			char previous = (char)chars[i - 1];
			char next = (i + 1 < length) ? (char)chars[i + 1] : '\0';
			bool split = false;

			if (is_upper(c)) {
				/* End of a word, or the start of one after an acronym. */
				split = is_lower(previous) || is_digit(previous) ||
				        (is_upper(previous) && is_lower(next));
			} else if (is_digit(c) && !is_digit(previous)) {
				split = true;
			}

			if (split && written < out_size - 1) out[written++] = ' ';
		}

		if (written < out_size - 1) out[written++] = c;
	}
	out[written] = '\0';
}

/* Remembered category per item name.
 *
 * category lives on the instance, and the copy produced by dropping an item
 * is a fresh Instantiate() of the ItemDrop prefab, whose ItemSeedData is
 * left at category 1 rather than the item's real one. So the same item goes
 * green the moment you drop it. Keying on the name instead, first sighting
 * wins: the level's properly-configured placed copy is seen first, so an
 * item keeps one colour for its whole life.
 *
 * Cleared on level change along with everything else, since categories can
 * legitimately differ between levels. */
#define ESP_CATEGORY_CACHE_SIZE 96
typedef struct {
	char name[ESP_ITEM_NAME_MAX];
	int category;
} esp_category_entry;

static esp_category_entry g_category_cache[ESP_CATEGORY_CACHE_SIZE];
static int g_category_cache_count = 0;

static bool esp_names_equal(const char *a, const char *b) {
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Returns the category this item is already known by, recording `observed`
 * the first time the name is seen. */
static int remembered_category(const char *name, int observed) {
	if (!name || !name[0]) return observed;

	for (int i = 0; i < g_category_cache_count; i++) {
		if (esp_names_equal(g_category_cache[i].name, name)) {
			return g_category_cache[i].category;
		}
	}

	if (g_category_cache_count < ESP_CATEGORY_CACHE_SIZE) {
		esp_category_entry *entry = &g_category_cache[g_category_cache_count++];
		int i = 0;
		for (; name[i] && i < (int)sizeof(entry->name) - 1; i++) {
			entry->name[i] = name[i];
		}
		entry->name[i] = '\0';
		entry->category = observed;
	}
	return observed;
}

/* The System.Type for ItemSeedData, built once from the class pointer we
 * capture off a live instance. Needed because FindObjectsOfType's
 * non-generic overload takes a Type rather than a baked generic MethodInfo,
 * and no baked one exists for this class. */
static void *g_item_seed_data_type = NULL;

/* Components found by the last scene scan, plus its cadence counter. */
static void *g_found_items[ESP_MAX_SEED_ITEMS];
static int g_found_count = 0;
static int g_scan_countdown = 0;

/* FindObjectsOfType walks every object in the scene, so it runs on a timer
 * rather than per frame. Positions are re-read from these cached components
 * every frame, which is what actually needs to be current. */
#define ESP_SCAN_INTERVAL_TICKS 30

static void *build_item_seed_data_type(void) {
	if (g_item_seed_data_type) return g_item_seed_data_type;
	if (!g_item_seed_data_klass) return NULL;

	HMODULE game_assembly = GetModuleHandleW(L"GameAssembly.dll");
	if (!game_assembly) return NULL;

	/* IL2CPP exports its C API by name, so these resolve without needing
	 * offsets that would drift between game builds. */
	typedef void *(*il2cpp_class_get_type_t)(void *klass);
	typedef void *(*il2cpp_type_get_object_t)(void *type);

	il2cpp_class_get_type_t class_get_type =
	    (il2cpp_class_get_type_t)GetProcAddress(game_assembly, "il2cpp_class_get_type");
	il2cpp_type_get_object_t type_get_object =
	    (il2cpp_type_get_object_t)GetProcAddress(game_assembly, "il2cpp_type_get_object");
	if (!class_get_type || !type_get_object) {
		OutputDebugStringA("[esp] il2cpp type exports missing, falling back to partial sources");
		return NULL;
	}

	void *type = class_get_type(g_item_seed_data_klass);
	if (!type) return NULL;

	g_item_seed_data_type = type_get_object(type);
	return g_item_seed_data_type;
}

/* Rescans the scene for every ItemSeedData. Returns true if the scan ran. */
static bool rescan_items(uintptr_t base) {
	void *system_type = build_item_seed_data_type();
	if (!system_type) return false;

	/* includeInactive = false, so deactivated preset placeholders never
	 * enter the list in the first place. */
	void *array = ((Object_FindObjectsOfType_t)(base + OFFSET_Object_FindObjectsOfType))(
	    system_type, false, NULL);
	if (!array) return false;

	long long length = *(long long *)((uintptr_t)array + IL2CPP_ARRAY_LENGTH_OFFSET);
	if (length < 0) return false;
	if (length > ESP_MAX_SEED_ITEMS) length = ESP_MAX_SEED_ITEMS;

	void **elements = (void **)((uintptr_t)array + IL2CPP_ARRAY_ELEMENTS_OFFSET);

	esp_lock();
	g_found_count = 0;
	for (long long i = 0; i < length; i++) {
		if (item_seed_data_still_valid(elements[i])) {
			g_found_items[g_found_count++] = elements[i];
		}
	}
	esp_unlock();
	return true;
}

/* Drops registry entries whose object is gone, without doing any of the
 * per-item work. Runs on the game thread like everything else that touches
 * the registry, so it takes the lock for the same reason the hooks do. */
static void esp_prune_registry(void) {
	esp_lock();
	int write = 0;
	for (int i = 0; i < g_seed_item_count; i++) {
		if (item_seed_data_still_valid(g_seed_items[i])) {
			g_seed_items[write++] = g_seed_items[i];
		}
	}
	g_seed_item_count = write;
	esp_unlock();
}

/* Walks the live ItemSeedData components. Returns how many were staged, and
 * fills `seen_objects` with each item's GameObject so the dropped-item walk
 * can skip anything already reported here. */
static int collect_seed_data_items(uintptr_t base, void **components, int component_count,
                                    esp_item *staged, int max_items,
                                    void **seen_objects, int *seen_count) {
	Component_get_transform_t get_transform =
	    (Component_get_transform_t)(base + OFFSET_Component_get_transform);
	Component_get_transform_t get_gameobject =
	    (Component_get_transform_t)(base + OFFSET_Component_get_gameObject);
	GameObject_get_active_t get_active =
	    (GameObject_get_active_t)(base + OFFSET_GameObject_get_activeInHierarchy);
	Transform_get_position_t get_position =
	    (Transform_get_position_t)(base + OFFSET_Transform_get_position);

	int count = 0;
	int write = 0;

	/* Compaction rewrites the registry, so hold the lock for the same
	 * reason the .ctor/OnDestroy hooks do. Everything here is game-thread
	 * only today, but the locking shouldn't be inconsistent between the
	 * writers. */
	esp_lock();
	for (int i = 0; i < component_count; i++) {
		void *component = components[i];

		/* OnDestroy should have removed it, but an item destroyed without
		 * that firing leaves a dangling entry. The class check matters as
		 * much as liveness here: once the GC reuses the block, the new
		 * occupant's own m_CachedPtr makes it look alive, and we'd then call
		 * Component methods on something that isn't a Component. */
		if (!item_seed_data_still_valid(component)) continue;
		/* Compacting in place is only meaningful when the caller passed the
		 * registry; the scan list is rebuilt wholesale each rescan. */
		if (components == g_seed_items) g_seed_items[write++] = component;

		if (count >= max_items) continue;

		/* The component being alive is NOT enough. Granny keeps several
		 * preset layouts in the level at once and deactivates the ones it
		 * isn't using, so an unused placeholder still has a live
		 * ItemSeedData -- that's what put a winch marker inside a cabinet
		 * with no winch in it. Only draw objects that are actually active. */
		void *game_object = get_gameobject(component, NULL);
		if (!unity_object_alive(game_object)) continue;
		if (!get_active(game_object, NULL)) continue;

		void *transform = get_transform(component, NULL);
		if (!unity_object_alive(transform)) continue;

		esp_vec3 position;
		get_position(&position, transform, NULL);

		if (*seen_count < max_items) seen_objects[(*seen_count)++] = game_object;

		staged[count].position = position;
		staged[count].name = NULL;
		/* Name first: the category is resolved through it, so that a dropped
		 * copy doesn't report a different category from the placed one. */
		il2cpp_string_to_ascii(*(void **)((uintptr_t)component + FIELD_ItemSeedData_itemName),
		                       staged[count].owned_name, (int)sizeof(staged[count].owned_name));
		staged[count].category = remembered_category(
		    staged[count].owned_name,
		    *(int *)((uintptr_t)component + FIELD_ItemSeedData_category));
		count++;
	}
	if (components == g_seed_items) g_seed_item_count = write;
	esp_unlock();

	return count;
}

/* Staging buffers for the collect below. File-scope rather than stack
 * locals: together they are roughly 12KB, and this runs every frame inside
 * a detour on PickRay::Update, on top of a call stack we don't control.
 * Safe to share because collection only ever happens on the game thread. */
static esp_item g_staged[ESP_MAX_ITEMS];
static void *g_seen_objects[ESP_MAX_ITEMS];

void esp_collect_items(void *item_seed) {
	/* Built here and published in one step at the end, so the render thread
	 * never observes the shared array mid-rewrite. */
	esp_item *staged = g_staged;
	int staged_count = 0;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	Transform_get_position_t get_position =
	    (Transform_get_position_t)(base + OFFSET_Transform_get_position);

	/* All three sources are complementary, not alternatives -- each one is
	 * partial, so they're merged and deduplicated by GameObject rather than
	 * chosen between.
	 *
	 * The registry only ever holds items whose managed constructor actually
	 * ran, which means Instantiate()d ones: Unity deserializes scene-placed
	 * MonoBehaviours without calling the C# .ctor we hook. Gating the
	 * ItemRepositionSeed walk on the registry being empty therefore broke
	 * the moment anything was dropped -- one Instantiate made staged_count
	 * non-zero and every placed item vanished with the skipped fallback. */
	void **seen_objects = g_seen_objects;
	int seen_count = 0;

	/* Rescan the scene periodically. FindObjectsOfType(includeInactive=false)
	 * returns every active ItemSeedData, which is genuinely complete -- so
	 * when it works, the partial sources below are redundant rather than
	 * complementary and are skipped entirely. */
	if (--g_scan_countdown <= 0) {
		g_scan_countdown = ESP_SCAN_INTERVAL_TICKS;
		rescan_items(base);
	}

	if (g_found_count > 0) {
		staged_count = collect_seed_data_items(base, g_found_items, g_found_count, staged,
		                                        ESP_MAX_ITEMS, seen_objects, &seen_count);
		esp_lock();
		for (int i = 0; i < staged_count; i++) {
			g_items[i] = staged[i];
		}
		g_item_count = staged_count;
		g_items_alive = staged_count;
		g_items_active = staged_count;
		esp_unlock();
		return;
	}

	/* No scene scan available (il2cpp exports missing, or the class pointer
	 * hasn't been learned yet). Fall back to the partial sources, merged and
	 * deduplicated. */
	staged_count = collect_seed_data_items(base, g_seed_items, g_seed_item_count, staged,
	                                        ESP_MAX_ITEMS, seen_objects, &seen_count);

	if (!item_seed_still_valid(item_seed)) {
		/* Stale, or a block the GC has since reused. Drop it rather than
		 * re-testing it every frame; the next ItemRepositionSeed::Awake
		 * latches a fresh one. */
		if (item_seed != NULL && item_seed == g_item_spawn) g_item_spawn = NULL;
	} else {
		int limit = esp_item_scan_limit;
		if (limit < 0) limit = 0;
		if (limit > ITEMSEED_ITEM_COUNT) limit = ITEMSEED_ITEM_COUNT;

		for (int i = 0; i < limit; i++) {
			/* These are Transforms, so no GetComponent hop is needed. */
			void *transform = *(void **)((uintptr_t)item_seed + ITEMSEED_FIRST_ITEM_FIELD + i * 8);

			if (esp_items_verbose) {
				char line[160];
				/* wsprintfA has no %p -- its conversions stop at %x/%X. */
				wsprintfA(line, "[esp] slot %d %s tf=%llx", i, g_item_names[i],
				          (unsigned long long)(uintptr_t)transform);
				OutputDebugStringA(line);
			}

			/* A collected item's Transform is destroyed but its managed
			 * wrapper survives, so a plain NULL check would let a dead
			 * object through. */
			if (!unity_object_alive(transform)) continue;
			if (staged_count >= ESP_MAX_ITEMS) break;

			/* Transform is a Component, so this reaches the same GameObject
			 * the registry would have recorded -- which is what lets the two
			 * sources be merged without drawing an item twice. */
			void *game_object =
			    ((Component_get_transform_t)(base + OFFSET_Component_get_gameObject))(transform, NULL);
			if (!unity_object_alive(game_object)) continue;
			/* Deactivated preset placeholders are present but not in play. */
			if (!((GameObject_get_active_t)(base + OFFSET_GameObject_get_activeInHierarchy))(game_object, NULL)) {
				continue;
			}

			bool already_seen = false;
			for (int s = 0; s < seen_count; s++) {
				if (seen_objects[s] == game_object) {
					already_seen = true;
					break;
				}
			}
			if (already_seen) continue;
			if (seen_count < ESP_MAX_ITEMS) seen_objects[seen_count++] = game_object;

			esp_vec3 position;
			get_position(&position, transform, NULL);

			staged[staged_count].position = position;
			staged[staged_count].name = g_item_names[i];
			staged[staged_count].owned_name[0] = 0;
			staged[staged_count].category = 0;

			/* Pull the real category off the object's own ItemSeedData.
			 * Without this these items all report category 0 and render as
			 * "other/free" grey, while dropped ones (which do reach the
			 * registry) show their true category -- the same item changing
			 * colour depending on how we found it. */
			void *method_info = *(void **)(base + METHODINFO_GetComponent_ItemSeedData);
			if (method_info) {
				void *seed_component = NULL;
				((GameObject_GetComponent_t)(base + OFFSET_GameObject_GetComponent_shared))(
				    game_object, &seed_component, method_info);
				if (unity_object_alive(seed_component)) {
					/* Also the earliest chance to learn the class, which the
					 * scene scan needs to build its System.Type -- the .ctor
					 * hook alone wouldn't fire until something is dropped. */
					if (!g_item_seed_data_klass) {
						g_item_seed_data_klass = *(void **)seed_component;
					}
					staged[staged_count].category =
					    *(int *)((uintptr_t)seed_component + FIELD_ItemSeedData_category);
					/* Prefer the game's own name over our static table. */
					il2cpp_string_to_ascii(
					    *(void **)((uintptr_t)seed_component + FIELD_ItemSeedData_itemName),
					    staged[staged_count].owned_name,
					    (int)sizeof(staged[staged_count].owned_name));
					if (staged[staged_count].owned_name[0]) {
						staged[staged_count].name = NULL;
					}
				}
			}

			staged_count++;
		}
	}

	/* Dropped items are separate objects that ItemRepositionSeed never
	 * learns about, so they're tracked from the drop and walked here. These
	 * are GameObjects rather than Transforms, hence the extra hop. */
	Component_get_transform_t get_transform =
	    (Component_get_transform_t)(base + OFFSET_GameObject_get_transform);
	GameObject_get_active_t get_active =
	    (GameObject_get_active_t)(base + OFFSET_GameObject_get_activeInHierarchy);

	for (int i = 0; i < ITEMSPAWN_ITEM_COUNT && staged_count < ESP_MAX_ITEMS; i++) {
		void *object = g_dropped[i];
		/* Picked up again -- destroyed, or at least deactivated. The class
		 * check also rejects a slot whose object the GC has since reclaimed
		 * and reused, which a liveness check alone would let through. */
		if (!gameobject_still_valid(object)) {
			g_dropped[i] = NULL; /* stop re-testing a dead slot every frame */
			continue;
		}
		if (!get_active(object, NULL)) continue;

		/* Skip anything the ItemSeedData registry already reported, so a
		 * dropped item that does re-register isn't drawn twice. */
		bool already_seen = false;
		for (int s = 0; s < seen_count; s++) {
			if (seen_objects[s] == object) {
				already_seen = true;
				break;
			}
		}
		if (already_seen) continue;

		void *transform = get_transform(object, NULL);
		if (!unity_object_alive(transform)) continue;

		esp_vec3 position;
		get_position(&position, transform, NULL);

		staged[staged_count].position = position;
		staged[staged_count].name = spawn_item_name(i);
		staged[staged_count].owned_name[0] = 0;
		staged[staged_count].category = 0;
		staged_count++;
	}

	esp_lock();
	for (int i = 0; i < staged_count; i++) {
		g_items[i] = staged[i];
	}
	g_item_count = staged_count;
	g_items_alive = staged_count;
	g_items_active = staged_count;
	esp_unlock();
}

static ItemRepositionSeed_Awake_t original_item_seed_awake = NULL;

/* ItemSpawn::Update is deliberately NOT hooked any more. It isn't a
 * registry: it activates one item chosen by CountItem, throws it, unparents
 * it, then calls Destroy(this.gameObject) on itself. Latching that pointer
 * was a use-after-free -- once the GC reused the block the m_CachedPtr check
 * passed on unrelated data and we read 55 garbage pointers out of it, which
 * is what crashed the game on version 1.8. ItemRepositionSeed holds the
 * level's actual items and persists, so it's hooked instead. */
static void __fastcall hooked_item_seed_awake(void *instance) {
	g_item_ticks++;
	/* Unambiguously an ItemRepositionSeed right here, so this is where to
	 * learn its class for the staleness check above. */
	if (!g_item_seed_klass && instance) g_item_seed_klass = *(void **)instance;
	g_item_spawn = instance;

	/* New level: every pointer cached from the old one is stale. The
	 * registry has to be cleared here too -- teardown doesn't reliably
	 * deliver OnDestroy for every item, so entries would otherwise survive
	 * into the next level and get called into. */
	esp_lock();
	for (int i = 0; i < ITEMSPAWN_ITEM_COUNT; i++) {
		g_dropped[i] = NULL;
	}
	g_seed_item_count = 0;
	g_found_count = 0;
	/* Categories can legitimately differ per level, so don't carry the
	 * remembered ones across. */
	g_category_cache_count = 0;
	esp_unlock();
	original_item_seed_awake(instance);
}

static ItemSpawn_Update_t original_item_spawn_update = NULL;

/* Records the item a dropper is about to spawn.
 *
 * ItemSpawn::Update activates one item chosen by CountItem, throws it,
 * unparents it, then destroys its own GameObject. The spawner therefore
 * cannot be kept (that was the v1.8 use-after-free) -- but the item it
 * spawns is a separate object that outlives it, so reading that pointer
 * here, while the spawner is still alive, is safe.
 *
 * Needed because a dropped item is a fresh Instantiate() that
 * ItemRepositionSeed has no reference to. Without this, picking an item up
 * and dropping it would hide it from the ESP for the rest of the session. */
static void __fastcall hooked_item_spawn_update(void *instance) {
	if (unity_object_alive(instance)) {
		float count = *(float *)((uintptr_t)instance + FIELD_ItemSpawn_CountItem);
		int index = (int)count - 1; /* CountItem is 1-based */
		if (index >= 0 && index < ITEMSPAWN_ITEM_COUNT) {
			void *item = *(void **)((uintptr_t)instance + ITEMSPAWN_FIRST_ITEM_FIELD + index * 8);
			if (unity_object_alive(item)) {
				/* Known-good GameObject, so this is the moment to learn what
				 * a GameObject's Il2CppClass* looks like. */
				if (!g_gameobject_klass) g_gameobject_klass = *(void **)item;
				g_dropped[index] = item;
			}
		}
	}
	original_item_spawn_update(instance);
}

static ItemSeedData_ctor_t original_item_seed_data_ctor = NULL;
static ItemSeedData_OnDestroy_t original_item_seed_data_on_destroy = NULL;

/* Items register themselves as they're built. The fields aren't readable
 * yet -- Unity assigns serialized values after the constructor -- so only
 * the pointer is kept; name and category are read at collect time. */
static void __fastcall hooked_item_seed_data_ctor(void *instance) {
	original_item_seed_data_ctor(instance);

	esp_lock();
	/* Unambiguously an ItemSeedData right here, so this is where to learn
	 * its class for the staleness check. */
	if (!g_item_seed_data_klass && instance) {
		g_item_seed_data_klass = *(void **)instance;
	}
	if (g_seed_item_count < ESP_MAX_SEED_ITEMS) {
		g_seed_items[g_seed_item_count++] = instance;
	}
	esp_unlock();
}

/* ...and deregister when picked up or the level tears down. */
static void __fastcall hooked_item_seed_data_on_destroy(void *instance) {
	esp_lock();
	for (int i = 0; i < g_seed_item_count; i++) {
		if (g_seed_items[i] == instance) {
			g_seed_items[i] = g_seed_items[g_seed_item_count - 1];
			g_seed_item_count--;
			break;
		}
	}
	esp_unlock();

	original_item_seed_data_on_destroy(instance);
}

static AI_MomSpider_Update_t original_momspider_update = NULL;

/* The cellar spider's own driver. Hooked for the same reason as Granny's
 * FixedUpdate -- it is the only place the live instance reliably appears --
 * and it simply never fires outside the cellar, so nothing needs to know
 * which level is loaded. */
/* Same shape as gameobject_still_valid(), for the same reason. */
static bool momspider_still_valid(void *instance) {
	if (!unity_object_alive(instance)) return false;
	if (!g_momspider_klass) return true; /* nothing to compare against yet */
	return *(void **)instance == g_momspider_klass;
}

static void __fastcall hooked_momspider_update(void *instance) {
	g_spider_ticks++;
	if (!g_momspider_klass && instance) g_momspider_klass = *(void **)instance;
	g_momspider = instance;

	if (!esp_momspider_enabled) {
		/* Only take the lock if there's something to clear. */
		if (g_have_spider_position) {
			esp_lock();
			g_have_spider_position = false;
			esp_unlock();
		}
		original_momspider_update(instance);
		return;
	}

	esp_vec3 position;
	bool have_position = false;
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base != 0 && unity_object_alive(instance)) {
		void *transform =
		    ((Component_get_transform_t)(base + OFFSET_Component_get_transform))(instance, NULL);
		if (unity_object_alive(transform)) {
			((Transform_get_position_t)(base + OFFSET_Transform_get_position))(&position, transform, NULL);
			have_position = true;
		}
	}

	/* Published together so the render thread never sees the flag set against
	 * a half-written position. */
	esp_lock();
	if (have_position) g_spider_position = position;
	g_have_spider_position = have_position;
	esp_unlock();

	original_momspider_update(instance);
}

static PickRay_Update_t original_pickray_update = NULL;

/* The ESP's primary tick. Runs every frame on the main thread for as long
 * as the player exists, which -- unlike AI_Granny::FixedUpdate -- keeps
 * working when Granny is disabled in the game's options. */
static void __fastcall hooked_pickray_update(void *instance) {
	g_pickray_ticks++;
	g_pickray = instance;

	/* Main thread, so this is where RenderSettings can safely be touched --
	 * the checkbox itself is clicked on the present thread. */
	fullbright_tick(instance);
	/* Same reason: Instantiate is an IL2CPP call, and the Spawn button that
	 * queues one is clicked on the present thread. */
	spawn_tick(instance);

	/* The cellar unloading doesn't tell us anything -- the spider's Update
	 * just stops firing, leaving the last position on screen forever. Its
	 * instance going stale is the signal. */
	if (g_have_spider_position && !momspider_still_valid(g_momspider)) {
		esp_lock();
		g_have_spider_position = false;
		esp_unlock();
		g_momspider = NULL;
	}

	if (esp_granny_enabled || esp_items_enabled || esp_momspider_enabled) {
		uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
		if (base != 0) {
			refresh_camera(base);

			/* PickRay is a component on the player, so its transform is the
			 * player's -- which is all the distance beside an arrow needs.
			 * Only paid for when an arrow could actually be drawn. */
			if (esp_arrows_enabled) {
				esp_vec3 position;
				bool have_position = false;
				void *transform =
				    ((Component_get_transform_t)(base + OFFSET_Component_get_transform))(instance, NULL);
				if (unity_object_alive(transform)) {
					((Transform_get_position_t)(base + OFFSET_Transform_get_position))(
					    &position, transform, NULL);
					have_position = true;
				}
				esp_lock();
				if (have_position) g_player_position = position;
				g_have_player_position = have_position;
				esp_unlock();
			}

			if (esp_items_enabled) {
				esp_collect_items(g_item_spawn);
			}
		}
	}

	/* Compaction can't be left to the collect above. It only compacts when
	 * it is handed the registry itself, which the scene-scan path never does
	 * -- and with Item ESP off it doesn't run at all. Either way dead entries
	 * accumulate until the array hits its cap, after which the .ctor hook
	 * silently discards every new item. Cheap enough to just always do: a
	 * walk of at most 192 pointers, two loads each. */
	esp_prune_registry();

	original_pickray_update(instance);
}

int esp_install_hooks(void) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) {
		OutputDebugStringA("[cheat] GameAssembly.dll not loaded, cannot hook ItemRepositionSeed::Awake");
		return 0;
	}

	void *target = (void *)(base + OFFSET_ItemRepositionSeed_Awake);

	if (MH_CreateHook(target, (void *)&hooked_item_seed_awake,
	                  (void **)&original_item_seed_awake) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(ItemRepositionSeed::Awake) failed");
		return 0;
	}
	if (MH_EnableHook(target) != MH_OK) {
		OutputDebugStringA("[cheat] MH_EnableHook(ItemRepositionSeed::Awake) failed");
		return 0;
	}

	void *spawn_target = (void *)(base + OFFSET_ItemSpawn_Update);
	if (MH_CreateHook(spawn_target, (void *)&hooked_item_spawn_update,
	                  (void **)&original_item_spawn_update) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(ItemSpawn::Update) failed");
		return 0;
	}
	if (MH_EnableHook(spawn_target) != MH_OK) {
		OutputDebugStringA("[cheat] MH_EnableHook(ItemSpawn::Update) failed");
		return 0;
	}

	void *seed_ctor_target = (void *)(base + OFFSET_ItemSeedData_ctor);
	if (MH_CreateHook(seed_ctor_target, (void *)&hooked_item_seed_data_ctor,
	                  (void **)&original_item_seed_data_ctor) == MH_OK) {
		MH_EnableHook(seed_ctor_target);
	} else {
		OutputDebugStringA("[cheat] MH_CreateHook(ItemSeedData::.ctor) failed");
	}

	void *seed_destroy_target = (void *)(base + OFFSET_ItemSeedData_OnDestroy);
	if (MH_CreateHook(seed_destroy_target, (void *)&hooked_item_seed_data_on_destroy,
	                  (void **)&original_item_seed_data_on_destroy) == MH_OK) {
		MH_EnableHook(seed_destroy_target);
	} else {
		OutputDebugStringA("[cheat] MH_CreateHook(ItemSeedData::OnDestroy) failed");
	}

	/* Not fatal if it fails: the cellar spider is one level's enemy, and
	 * losing its box shouldn't cost you the rest of the ESP. */
	void *spider_target = (void *)(base + OFFSET_AI_MomSpider_Update);
	if (MH_CreateHook(spider_target, (void *)&hooked_momspider_update,
	                  (void **)&original_momspider_update) == MH_OK) {
		MH_EnableHook(spider_target);
	} else {
		OutputDebugStringA("[cheat] MH_CreateHook(AI_MomSpider::Update) failed");
	}

	void *pickray_target = (void *)(base + OFFSET_PickRay_Update);
	if (MH_CreateHook(pickray_target, (void *)&hooked_pickray_update,
	                  (void **)&original_pickray_update) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(PickRay::Update) failed");
		return 0;
	}
	if (MH_EnableHook(pickray_target) != MH_OK) {
		OutputDebugStringA("[cheat] MH_EnableHook(PickRay::Update) failed");
		return 0;
	}

	OutputDebugStringA("[cheat] ItemRepositionSeed::Awake + PickRay::Update hooked");
	return 1;
}

void esp_render(void) {
	if (!esp_granny_enabled && !esp_items_enabled && !esp_momspider_enabled) return;

	/* Held across the whole draw so the collected data can't be rewritten
	 * from the game thread midway through reading it. The collector only
	 * takes this to publish, never across its IL2CPP walk, so the wait here
	 * is short. */
	esp_lock();

	if (!g_have_view_projection) {
		/* Say so on screen rather than silently drawing nothing, so it's
		 * obvious the overlay is alive and just missing data. */
		ImDrawList *draw = ImGui::GetBackgroundDrawList();
		draw->AddText(ImVec2(12.0f, 12.0f), IM_COL32(255, 160, 60, 255),
		              "[esp] waiting for camera (is a level loaded?)");
		esp_unlock();
		return;
	}

	if (esp_granny_enabled && g_have_granny_position) {
		/* Her transform sits at her feet, so the box grows upward. */
		const ImU32 granny_color = IM_COL32(255, 64, 64, 255);
		draw_entity(g_granny_position, esp_box_height, esp_box_width_ratio, "Granny", granny_color);
		/* No-op while she's on screen -- see draw_direction_arrow(). */
		if (esp_arrows_enabled) draw_direction_arrow(g_granny_position, granny_color, "Granny");
	}

	if (esp_momspider_enabled && g_have_spider_position) {
		const ImU32 spider_color = IM_COL32(200, 120, 255, 255);
		draw_entity(g_spider_position, esp_spider_box_height, esp_spider_box_width_ratio,
		            "Mom Spider", spider_color);
		if (esp_arrows_enabled) draw_direction_arrow(g_spider_position, spider_color, "Mom Spider");
	}

	if (esp_items_enabled) {
		ImDrawList *draw = ImGui::GetBackgroundDrawList();
		for (int i = 0; i < g_item_count; i++) {
			if (!esp_category_shown(g_items[i].category)) continue;

			float x, y;
			if (!esp_world_to_screen(g_items[i].position, &x, &y)) continue;

			/* Fallback entries carry a static name; ItemSeedData ones carry
			 * the game's own string. */
			const char *label = g_items[i].name ? g_items[i].name : g_items[i].owned_name;
			if (!label || !label[0]) label = "item";

			const ImU32 color = esp_category_color(g_items[i].category);
			draw->AddCircleFilled(ImVec2(x, y), 3.0f, color);
			draw->AddCircle(ImVec2(x, y), 4.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);

			ImVec2 text_size = ImGui::CalcTextSize(label);
			ImVec2 text_pos(x - text_size.x * 0.5f, y + 6.0f);
			draw->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 200), label);
			draw->AddText(text_pos, color, label);
		}
	}

	esp_unlock();
}
