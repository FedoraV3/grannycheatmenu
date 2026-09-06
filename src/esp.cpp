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
int esp_item_scan_limit = ITEMSPAWN_ITEM_COUNT;

/* Tuned by eye in game -- Granny's model is a good deal taller in world
 * units than a stock Unity humanoid, hence the large height. */
float esp_box_height = 4.65f;
float esp_box_width_ratio = 0.50f;

static esp_mat4 g_view_projection;
static bool g_have_view_projection = false;

static esp_vec3 g_granny_position;
static bool g_have_granny_position = false;

/* Names for ItemSpawn's 55 GameObject* fields, in declaration order --
 * index N here is the field at ITEMSPAWN_FIRST_ITEM_FIELD + N * 8. */
static const char *const g_item_names[ITEMSPAWN_ITEM_COUNT] = {
	"crossbow", "plier", "battery", "gas", "seed", "book", "winch",
	"car battery", "car key", "cutter", "code", "baton", "ec key", "hammer",
	"padlock", "mas", "cogwheel 1", "cogwheel 2", "meat", "melon", "spray",
	"plank", "playhouse", "remote", "data", "rusty", "safe", "screwdriver",
	"shotgun", "sp1", "sp2", "sp3", "shotgun 2", "spark", "special",
	"spider", "syringe", "t1", "t2", "t3", "t4", "teddy", "text", "topp",
	"wp", "vas", "vas2", "vas3", "wheel", "stick", "wrench", "rat",
	"ornament bomb", "ornament freeze", "fuse",
};

typedef struct {
	esp_vec3 position;
	const char *name;
} esp_item;

static esp_item g_items[ITEMSPAWN_ITEM_COUNT];
static int g_item_count = 0;
static int g_items_alive = 0;
static int g_items_active = 0;

static unsigned long g_granny_ticks = 0;
static unsigned long g_item_ticks = 0;

/* ItemSpawn::Update turned out NOT to be a per-frame tick -- in practice it
 * only runs when an item is picked up or dropped. So the hook is used only
 * to capture the instance, and the actual position walk happens on the
 * AI_Granny::FixedUpdate tick, which is reliably 50Hz. */
static void *volatile g_item_spawn = NULL;

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
	                      ? *(void **)((uintptr_t)g_item_spawn + ITEMSPAWN_FIRST_ITEM_FIELD)
	                      : NULL;
	out->granny_ticks = g_granny_ticks;
	out->item_ticks = g_item_ticks;
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

/* Camera.main returns NULL unless the game tags its camera "MainCamera",
 * which plenty of Unity games never bother to do -- so fall back to walking
 * AI_Granny -> PlayerStatus -> PlayerCam, which is two pointer derefs and
 * depends on no tags or IL2CPP calls at all. */
static void *resolve_camera(uintptr_t base) {
	void *camera = ((Camera_get_main_t)(base + OFFSET_Camera_get_main))(NULL);
	if (unity_object_alive(camera)) return camera;

	void *granny = ai_granny_current();
	if (!unity_object_alive(granny)) return NULL;

	void *player_status = *(void **)((uintptr_t)granny + FIELD_AI_Granny_PlayerStatus);
	if (!unity_object_alive(player_status)) return NULL;

	camera = *(void **)((uintptr_t)player_status + FIELD_PlayerStatus_PlayerCam);
	return unity_object_alive(camera) ? camera : NULL;
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

	/* Item collection is disabled: see hooked_item_spawn_update() for why
	 * ItemSpawn can't provide the world's items. */
	g_item_count = 0;
}

/* Walks ItemSpawn's item pointers. Driven from esp_collect() on the
 * FixedUpdate tick, not from the ItemSpawn hook -- that one only fires on
 * pickup/drop, so positions would almost never refresh. */
void esp_collect_items(void *item_spawn) {
	g_item_count = 0;
	g_items_alive = 0;
	g_items_active = 0;
	/* The instance is latched from a hook that may have fired a level ago,
	 * so verify it's still alive before touching it. */
	if (!unity_object_alive(item_spawn)) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	Component_get_transform_t get_transform =
	    (Component_get_transform_t)(base + OFFSET_GameObject_get_transform);
	Transform_get_position_t get_position =
	    (Transform_get_position_t)(base + OFFSET_Transform_get_position);
	GameObject_get_active_t get_active =
	    (GameObject_get_active_t)(base + OFFSET_GameObject_get_activeInHierarchy);

	int limit = esp_item_scan_limit;
	if (limit < 0) limit = 0;
	if (limit > ITEMSPAWN_ITEM_COUNT) limit = ITEMSPAWN_ITEM_COUNT;

	for (int i = 0; i < limit; i++) {
		void *object = *(void **)((uintptr_t)item_spawn + ITEMSPAWN_FIRST_ITEM_FIELD + i * 8);

		if (esp_items_verbose) {
			char line[160];
			wsprintfA(line, "[esp] slot %d %s ptr=%p", i, g_item_names[i], object);
			OutputDebugStringA(line);
		}

		/* Destroyed items keep a non-NULL managed wrapper, so a plain NULL
		 * check isn't enough -- this is what was crashing the game. */
		if (!unity_object_alive(object)) continue;
		g_items_alive++;
		/* Already picked up (or not spawned for this run's layout). */
		bool active = get_active(object, NULL);
		if (active) g_items_active++;
		if (!active && !esp_items_ignore_active) continue;

		void *transform = get_transform(object, NULL);
		if (!unity_object_alive(transform)) continue;

		esp_vec3 position;
		get_position(&position, transform, NULL);

		g_items[g_item_count].position = position;
		g_items[g_item_count].name = g_item_names[i];
		g_item_count++;
	}
}

static ItemSpawn_Update_t original_item_spawn_update = NULL;

static void __fastcall hooked_item_spawn_update(void *instance) {
	g_item_ticks++;
	/* Deliberately NOT latching the instance any more.
	 *
	 * Decompiling ItemSpawn::Update showed it isn't an item registry at all:
	 * it activates ONE item chosen by CountItem, throws it with AddForce,
	 * unparents it, then calls Destroy(this.gameObject) on itself. So every
	 * instance is a one-shot spawner that dies during its first Update.
	 *
	 * Holding that pointer was a use-after-free: once the GC reused the
	 * block, the m_CachedPtr check would pass on unrelated data and we'd
	 * read 55 garbage "pointers" out of it and call into them. That is what
	 * crashed on game version 1.8, where many spawners run at level start.
	 *
	 * The real world items are the objects these spawners activate and
	 * unparent, which have to be found some other way. */
	(void)instance;
	original_item_spawn_update(instance);
}

int esp_install_hooks(void) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) {
		OutputDebugStringA("[cheat] GameAssembly.dll not loaded, cannot hook ItemSpawn::Update");
		return 0;
	}

	void *target = (void *)(base + OFFSET_ItemSpawn_Update);

	if (MH_CreateHook(target, (void *)&hooked_item_spawn_update,
	                  (void **)&original_item_spawn_update) != MH_OK) {
		OutputDebugStringA("[cheat] MH_CreateHook(ItemSpawn::Update) failed");
		return 0;
	}
	if (MH_EnableHook(target) != MH_OK) {
		OutputDebugStringA("[cheat] MH_EnableHook(ItemSpawn::Update) failed");
		return 0;
	}

	OutputDebugStringA("[cheat] ItemSpawn::Update hooked");
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
