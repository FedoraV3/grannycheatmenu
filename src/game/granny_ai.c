#include "game/granny_ai.h"
#include "game/offsets.h"

#include <windows.h>

bool granny_speed_enabled = false;
float granny_walk_speed = 1.0f;
float granny_run_speed = 2.0f;
bool granny_freeze_enabled = false;

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

/*
 * Zeroing Walk_Speed/Run_Speed alone isn't a freeze. Those fields are what
 * her FixedUpdate feeds into the NavMeshAgent, so a new value only takes
 * effect the next time it does that -- until then the agent keeps coasting
 * along its current path at whatever speed it was last given. Telling the
 * agent directly makes the stop immediate.
 *
 * Her agent is a UnityEngine.Object like any other, so it needs the
 * m_CachedPtr check before being called into: during teardown the AI_Granny
 * can still be ticking with an already-destroyed agent.
 */
static void set_agent_speed(void *granny, float speed) {
	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	if (base == 0) return;

	void *agent = *(void **)((uintptr_t)granny + FIELD_AI_Granny_Agent);
	if (!agent) return;
	if (*(void **)((uintptr_t)agent + FIELD_UnityObject_m_CachedPtr) == NULL) return;

	((Unity_set_float_t)(base + OFFSET_NavMeshAgent_set_speed))(agent, speed, NULL);
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

	/* Freeze is the one thing that can't be written once and left alone: her
	 * FixedUpdate pushes Walk_Speed into the agent again on every tick, and
	 * ChaseAction re-arms her state on every transition, so a single write
	 * gets undone within a frame. Everything else still follows the
	 * write-only-on-change rule below. */
	if (granny_freeze_enabled) {
		if (!g_speed_applied) {
			g_original_walk_speed = read_walk_speed(instance);
			g_original_run_speed = read_run_speed(instance);
			g_speed_applied = true;
		}
		change_granny_walk_speed(0.0f);
		change_granny_run_speed(0.0f);
		set_agent_speed(instance, 0.0f);
		g_speed_dirty = false;
		return;
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
		/* Unfreezing has to reach the agent too, for the same reason
		 * freezing did -- otherwise she stays stopped until whatever her
		 * FixedUpdate does next happens to push a speed through. */
		set_agent_speed(instance, g_original_walk_speed);
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
