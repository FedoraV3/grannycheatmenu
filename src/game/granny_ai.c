#include "game/granny_ai.h"
#include "game/offsets.h"

bool granny_speed_enabled = false;
float granny_walk_speed = 1.0f;
float granny_run_speed = 2.0f;

/* Her speeds as the game set them, and which instance they came from. */
static bool g_speed_applied = false;
static bool g_speeds_seeded = false;
static float g_original_walk_speed = 0.0f;
static float g_original_run_speed = 0.0f;
static void *g_applied_instance = NULL;

/* Set by the menu when a control changes, so the next tick applies the new
 * value and then goes quiet again. */
static bool g_speed_dirty = false;

void granny_speed_mark_dirty(void) {
	g_speed_dirty = true;
}

void change_granny_walk_speed(float speed) {
	// simple memory changes
	void *granny_ai = ai_granny_current();
	if (granny_ai != NULL)
	{
		*(volatile float*)((uintptr_t)granny_ai + FIELD_AI_Granny_Walk_Speed) = speed;
	}
}

void change_granny_run_speed(float speed) {
	// simple memory changes
	void *granny_ai = ai_granny_current();
	if (granny_ai != NULL)
	{
		*(volatile float*)((uintptr_t)granny_ai + FIELD_AI_Granny_Run_Speed) = speed;
	}
}

static float read_walk_speed(void *granny) {
	return *(volatile float *)((uintptr_t)granny + FIELD_AI_Granny_Walk_Speed);
}

static float read_run_speed(void *granny) {
	return *(volatile float *)((uintptr_t)granny + FIELD_AI_Granny_Run_Speed);
}

void granny_ai_tick(void *instance) {
	if (instance == NULL) return;

	/* A different instance means the game rebuilt her -- level load, or a
	 * respawn into a new day -- so her speeds are back to the difficulty's
	 * values and anything we saved belongs to an object that's gone. */
	bool new_instance = (instance != g_applied_instance);

	if (new_instance) {
		g_applied_instance = instance;
		g_speed_applied = false;
		/* Re-seed from the fresh instance: a different day or difficulty can
		 * legitimately use different speeds. */
		g_speeds_seeded = false;
	}

	/* Nothing to do on the vast majority of ticks. The write only happens
	 * when the game rebuilt her or you moved a control, rather than every
	 * tick fighting whatever the game does. */
	if (!new_instance && !g_speed_dirty) {
		if (!g_speeds_seeded && !granny_speed_enabled) {
			granny_walk_speed = read_walk_speed(instance);
			granny_run_speed = read_run_speed(instance);
			g_speeds_seeded = true;
		}
		return;
	}
	g_speed_dirty = false;

	if (granny_speed_enabled) {
		if (!g_speed_applied) {
			/* Save what the game chose before overwriting, so switching the
			 * feature off restores the real values rather than a guess --
			 * Hollow doesn't use the same speeds as Normal. */
			g_original_walk_speed = read_walk_speed(instance);
			g_original_run_speed = read_run_speed(instance);
			g_speed_applied = true;
		}
		change_granny_walk_speed(granny_walk_speed);
		change_granny_run_speed(granny_run_speed);
		return;
	}

	if (g_speed_applied) {
		change_granny_walk_speed(g_original_walk_speed);
		change_granny_run_speed(g_original_run_speed);
		g_speed_applied = false;
		return;
	}

	/* Idle: seed the sliders once from her real speeds so enabling the
	 * feature doesn't jerk her to an arbitrary default. */
	if (!g_speeds_seeded) {
		granny_walk_speed = read_walk_speed(instance);
		granny_run_speed = read_run_speed(instance);
		g_speeds_seeded = true;
	}
}
