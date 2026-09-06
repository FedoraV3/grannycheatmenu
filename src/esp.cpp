#include "esp.h"
#include "ai_granny_hook.h"
#include "offsets.h"
#include "MinHook.h"

#include <windows.h>
#include <math.h>

#include "imgui.h"

bool esp_granny_enabled = false;
bool esp_items_enabled = false;
bool esp_fullbright_enabled = false;
bool esp_items_ignore_active = false;
bool esp_items_verbose = false;
int esp_item_scan_limit = ITEMSEED_ITEM_COUNT;

/* Tuned by eye in game -- Granny's model is a good deal taller in world
 * units than a stock Unity humanoid, hence the large height. */
float esp_box_height = 4.65f;
float esp_box_width_ratio = 0.50f;

static esp_mat4 g_view_projection;
static bool g_have_view_projection = false;

static esp_vec3 g_granny_position;
static bool g_have_granny_position = false;

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

typedef struct {
	esp_vec3 position;
	const char *name;
} esp_item;

static esp_item g_items[ITEMSEED_ITEM_COUNT];
static int g_item_count = 0;
static int g_items_alive = 0;
static int g_items_active = 0;

static unsigned long g_granny_ticks = 0;
static unsigned long g_item_ticks = 0;
static unsigned long g_pickray_ticks = 0;

/* The ItemRepositionSeed instance, latched at its Awake. It holds the
 * level's 35 item Transforms and persists, unlike ItemSpawn which destroys
 * itself. Walked on the FixedUpdate tick. */
static void *volatile g_item_spawn = NULL;

/* The player's PickRay, latched from its Update. Drives the ESP whenever
 * Granny is switched off and AI_Granny::FixedUpdate never fires. */
static void *volatile g_pickray = NULL;

/* A UnityEngine.Object whose native side has been destroyed keeps its
 * managed wrapper, so the pointer still looks valid from C -- only
 * m_CachedPtr going NULL reveals it. Calling into one of those dereferences
 * a freed native object and crashes the game, so every Unity object has to
 * pass through here before we touch it. */
static bool unity_object_alive(void *object) {
	if (!object) return false;
	return *(void **)((uintptr_t)object + FIELD_UnityObject_m_CachedPtr) != NULL;
}

void esp_get_debug_info(esp_debug_info *out) {
	if (!out) return;
	out->have_view_projection = g_have_view_projection;
	out->have_granny_position = g_have_granny_position;
	out->item_count = g_item_count;
	out->items_alive = g_items_alive;
	out->items_active = g_items_active;
	out->have_item_spawn = (g_item_spawn != NULL);
	out->item_spawn = g_item_spawn;
	out->item_spawn_alive = unity_object_alive(g_item_spawn);
	out->item_slot0 = g_item_spawn
	                      ? *(void **)((uintptr_t)g_item_spawn + ITEMSEED_FIRST_ITEM_FIELD)
	                      : NULL;
	out->granny_ticks = g_granny_ticks;
	out->item_ticks = g_item_ticks;
	out->pickray_ticks = g_pickray_ticks;
}

void esp_set_view_projection(const esp_mat4 *vp) {
	if (vp) {
		g_view_projection = *vp;
		g_have_view_projection = true;
	} else {
		g_have_view_projection = false;
	}
}

bool esp_world_to_screen(esp_vec3 world, float *out_x, float *out_y) {
	if (!g_have_view_projection) return false;

	const float(*m)[4] = g_view_projection.m;

	float clip_x = m[0][0] * world.x + m[0][1] * world.y + m[0][2] * world.z + m[0][3];
	float clip_y = m[1][0] * world.x + m[1][1] * world.y + m[1][2] * world.z + m[1][3];
	float clip_w = m[3][0] * world.x + m[3][1] * world.y + m[3][2] * world.z + m[3][3];

	/* NaN compares false against everything, so it would slip past every
	 * range check below and reach ImGui as a garbage vertex -- which blows
	 * out the draw list's 16-bit index buffer and trips an assert deep
	 * inside ImGui rather than here. Reject it explicitly. */
	if (!isfinite(clip_x) || !isfinite(clip_y) || !isfinite(clip_w)) return false;

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

/* Box + label at a projected world position. Height is in world units and
 * projected separately so the box shrinks with distance on its own. */
static void draw_entity(esp_vec3 feet, float height, const char *label, ImU32 color) {
	float foot_x, foot_y, head_x, head_y;
	if (!esp_world_to_screen(feet, &foot_x, &foot_y)) return;

	esp_vec3 head = feet;
	head.y += height;
	if (!esp_world_to_screen(head, &head_x, &head_y)) return;

	float box_height = foot_y - head_y;
	float box_width = box_height * esp_box_width_ratio;

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
		g_have_granny_position = false;
		return;
	}

	g_have_granny_position = false;
	if (unity_object_alive(granny_instance)) {
		void *transform =
		    ((Component_get_transform_t)(base + OFFSET_Component_get_transform))(granny_instance, NULL);
		if (unity_object_alive(transform)) {
			esp_vec3 position;
			((Transform_get_position_t)(base + OFFSET_Transform_get_position))(&position, transform, NULL);
			g_granny_position = position;
			g_have_granny_position = true;
		}
	}

	/* Items are collected on the PickRay tick instead -- that one keeps
	 * running when Granny is switched off, and this hook doesn't. */
}

/* Walks ItemSpawn's item pointers. Driven from esp_collect() on the
 * FixedUpdate tick, not from the ItemSpawn hook -- that one only fires on
 * pickup/drop, so positions would almost never refresh. */
void esp_collect_items(void *item_seed) {
	g_item_count = 0;
	g_items_alive = 0;
	g_items_active = 0;
	/* Latched at Awake, possibly a level ago -- always re-verify. */
	if (!unity_object_alive(item_seed)) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	Transform_get_position_t get_position =
	    (Transform_get_position_t)(base + OFFSET_Transform_get_position);

	int limit = esp_item_scan_limit;
	if (limit < 0) limit = 0;
	if (limit > ITEMSEED_ITEM_COUNT) limit = ITEMSEED_ITEM_COUNT;

	for (int i = 0; i < limit; i++) {
		/* These are Transforms, so no GetComponent hop is needed. */
		void *transform = *(void **)((uintptr_t)item_seed + ITEMSEED_FIRST_ITEM_FIELD + i * 8);

		if (esp_items_verbose) {
			char line[160];
			wsprintfA(line, "[esp] slot %d %s tf=%p", i, g_item_names[i], transform);
			OutputDebugStringA(line);
		}

		/* A collected item's Transform is destroyed but its managed wrapper
		 * survives, so a plain NULL check would let a dead object through. */
		if (!unity_object_alive(transform)) continue;
		g_items_alive++;
		g_items_active++;

		esp_vec3 position;
		get_position(&position, transform, NULL);

		g_items[g_item_count].position = position;
		g_items[g_item_count].name = g_item_names[i];
		g_item_count++;
	}
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
	g_item_spawn = instance;
	original_item_seed_awake(instance);
}

static PickRay_Update_t original_pickray_update = NULL;

/* The ESP's primary tick. Runs every frame on the main thread for as long
 * as the player exists, which -- unlike AI_Granny::FixedUpdate -- keeps
 * working when Granny is disabled in the game's options. */
static void __fastcall hooked_pickray_update(void *instance) {
	g_pickray_ticks++;
	g_pickray = instance;

	if (esp_granny_enabled || esp_items_enabled) {
		uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
		if (base != 0) {
			refresh_camera(base);
			if (esp_items_enabled) {
				esp_collect_items(g_item_spawn);
			}
		}
	}

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
	if (!esp_granny_enabled && !esp_items_enabled) return;

	if (!g_have_view_projection) {
		/* Say so on screen rather than silently drawing nothing, so it's
		 * obvious the overlay is alive and just missing data. */
		ImDrawList *draw = ImGui::GetBackgroundDrawList();
		draw->AddText(ImVec2(12.0f, 12.0f), IM_COL32(255, 160, 60, 255),
		              "[esp] waiting for camera (is a level loaded?)");
		return;
	}

	if (esp_granny_enabled && g_have_granny_position) {
		/* Her transform sits at her feet, so the box grows upward. */
		draw_entity(g_granny_position, esp_box_height, "Granny", IM_COL32(255, 64, 64, 255));
	}

	if (esp_items_enabled) {
		ImDrawList *draw = ImGui::GetBackgroundDrawList();
		for (int i = 0; i < g_item_count; i++) {
			float x, y;
			if (!esp_world_to_screen(g_items[i].position, &x, &y)) continue;

			const ImU32 color = IM_COL32(120, 220, 255, 255);
			draw->AddCircleFilled(ImVec2(x, y), 3.0f, color);
			draw->AddCircle(ImVec2(x, y), 4.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);

			ImVec2 text_size = ImGui::CalcTextSize(g_items[i].name);
			ImVec2 text_pos(x - text_size.x * 0.5f, y + 6.0f);
			draw->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 200), g_items[i].name);
			draw->AddText(text_pos, color, g_items[i].name);
		}
	}
}
