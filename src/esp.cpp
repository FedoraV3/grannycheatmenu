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

	/* Behind the camera (or right on the plane) -- projecting would mirror
	 * the point to the wrong side of the screen. */
	if (clip_w < 0.1f) return false;

	float ndc_x = clip_x / clip_w;
	float ndc_y = clip_y / clip_w;

	ImVec2 screen = ImGui::GetIO().DisplaySize;
	float x = (screen.x * 0.5f) * (1.0f + ndc_x);
	float y = (screen.y * 0.5f) * (1.0f - ndc_y);

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
	float box_width = box_height * 0.45f;

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

/* Main thread only. Refreshed from both hooks so the overlay still has a
 * camera when only one of them is ticking. */
static bool refresh_camera(uintptr_t base) {
	void *camera = ((Camera_get_main_t)(base + OFFSET_Camera_get_main))(NULL);
	if (!camera) {
		/* No main camera right now (menu screen, loading, scene swap). */
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
	esp_set_view_projection(&view_projection);
	return true;
}

void esp_collect(void *granny_instance) {
	/* Don't pay for IL2CPP calls every physics tick when nothing is drawn. */
	if (!esp_granny_enabled && !esp_items_enabled) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	if (!refresh_camera(base)) {
		g_have_granny_position = false;
		return;
	}

	g_have_granny_position = false;
	if (granny_instance) {
		void *transform =
		    ((Component_get_transform_t)(base + OFFSET_Component_get_transform))(granny_instance, NULL);
		if (transform) {
			esp_vec3 position;
			((Transform_get_position_t)(base + OFFSET_Transform_get_position))(&position, transform, NULL);
			g_granny_position = position;
			g_have_granny_position = true;
		}
	}
}

void esp_collect_items(void *item_spawn) {
	g_item_count = 0;
	if (!item_spawn) return;

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	Component_get_transform_t get_transform =
	    (Component_get_transform_t)(base + OFFSET_GameObject_get_transform);
	Transform_get_position_t get_position =
	    (Transform_get_position_t)(base + OFFSET_Transform_get_position);
	GameObject_get_active_t get_active =
	    (GameObject_get_active_t)(base + OFFSET_GameObject_get_activeInHierarchy);

	for (int i = 0; i < ITEMSPAWN_ITEM_COUNT; i++) {
		void *object = *(void **)((uintptr_t)item_spawn + ITEMSPAWN_FIRST_ITEM_FIELD + i * 8);
		if (!object) continue;
		/* Already picked up (or not spawned for this run's layout). */
		if (!get_active(object, NULL)) continue;

		void *transform = get_transform(object, NULL);
		if (!transform) continue;

		esp_vec3 position;
		get_position(&position, transform, NULL);

		g_items[g_item_count].position = position;
		g_items[g_item_count].name = g_item_names[i];
		g_item_count++;
	}
}

static ItemSpawn_Update_t original_item_spawn_update = NULL;

static void __fastcall hooked_item_spawn_update(void *instance) {
	/* Per-frame and independent of whether Granny is alive, so this is the
	 * better place to refresh the camera than the 50Hz FixedUpdate hook. */
	if (esp_granny_enabled || esp_items_enabled) {
		uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
		if (base != 0) refresh_camera(base);
	}
	/* Skipped entirely while Item ESP is off -- 55 items x 3 IL2CPP calls
	 * every frame is not something to pay for unless it's on screen. */
	if (esp_items_enabled) {
		esp_collect_items(instance);
	}
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
		draw_entity(g_granny_position, 1.8f, "Granny", IM_COL32(255, 64, 64, 255));
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
