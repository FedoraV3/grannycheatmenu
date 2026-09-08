#include "game/fullbright.h"
#include "game/offsets.h"

#include <windows.h>

bool fullbright_enabled = false;

/* Whether the override is currently in effect, and the values to put back. */
static bool g_applied = false;

/* The PickRay the current lighting state belongs to. When it changes the
 * scene was rebuilt, so the override needs re-applying. */
static void *g_scene_player = NULL;
static int g_saved_ambient_mode = 0;
static bool g_saved_fog = false;

static void apply_fullbright(uintptr_t base) {
	((RenderSettings_set_ambientMode_t)(base + OFFSET_RenderSettings_set_ambientMode))(
	    UNITY_AMBIENT_MODE_FLAT, NULL);

	const unity_color white = { 1.0f, 1.0f, 1.0f, 1.0f };
	((RenderSettings_set_color_t)(base + OFFSET_RenderSettings_set_ambientLight))(&white, NULL);

	((RenderSettings_set_float_t)(base + OFFSET_RenderSettings_set_ambientIntensity))(1.0f, NULL);

	/* Fog is what actually hides things at distance in this game, so leaving
	 * it on would undercut the whole point. */
	((RenderSettings_set_fog_t)(base + OFFSET_RenderSettings_set_fog))(false, NULL);
}

void fullbright_tick(void *player) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	/* A different PickRay means the game rebuilt the scene -- a restart, or
	 * a new game -- which resets RenderSettings along with it. Anything we
	 * saved belonged to the old scene, so drop it and treat this as fresh. */
	if (player != NULL && player != g_scene_player) {
		g_scene_player = player;
		g_applied = false;
	}

	if (fullbright_enabled) {
		if (!g_applied) {
			/* Save what's restorable before overwriting it. There are no
			 * getters for ambientLight or ambientIntensity, but restoring
			 * the mode makes both irrelevant again. */
			g_saved_ambient_mode =
			    ((RenderSettings_get_ambientMode_t)(base + OFFSET_RenderSettings_get_ambientMode))(NULL);
			g_saved_fog =
			    ((RenderSettings_get_fog_t)(base + OFFSET_RenderSettings_get_fog))(NULL);

			apply_fullbright(base);
			g_applied = true;
			OutputDebugStringA("[cheat] fullbright on");
			return;
		}

		/* Belt and braces alongside the instance check above: fog coming
		 * back on is just as visible as the ambient mode changing, and
		 * either can be reset independently. */
		int mode =
		    ((RenderSettings_get_ambientMode_t)(base + OFFSET_RenderSettings_get_ambientMode))(NULL);
		bool fog = ((RenderSettings_get_fog_t)(base + OFFSET_RenderSettings_get_fog))(NULL);
		if (mode != UNITY_AMBIENT_MODE_FLAT || fog) {
			apply_fullbright(base);
		}
		return;
	}

	if (g_applied) {
		((RenderSettings_set_ambientMode_t)(base + OFFSET_RenderSettings_set_ambientMode))(
		    g_saved_ambient_mode, NULL);
		((RenderSettings_set_fog_t)(base + OFFSET_RenderSettings_set_fog))(g_saved_fog, NULL);
		g_applied = false;
		OutputDebugStringA("[cheat] fullbright off");
	}
}
