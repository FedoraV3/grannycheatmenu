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
extern bool esp_fullbright_enabled;

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
