#include "overlay/notify.h"

#include <windows.h>

#include "imgui.h"

bool notify_enabled = true;
int notify_position = NOTIFY_TOP_RIGHT;
float notify_duration = 2.5f;

/* Six is more than you can read at once but enough that spamming a bind
 * doesn't silently drop the last one you actually cared about. */
#define NOTIFY_MAX 6
#define NOTIFY_TEXT_MAX 64

/* How long the toast spends fading, out of its total lifetime. Long enough
 * to read as a fade rather than a flicker, short enough that the text is at
 * full opacity for most of the time it's up. */
#define NOTIFY_FADE_SECONDS 0.6f

typedef struct {
	char text[NOTIFY_TEXT_MAX];
	unsigned long long posted_ms;
	ImU32 color;
} notify_entry;

/* A ring: the oldest entry is overwritten rather than the newest dropped,
 * so the most recent toast is always the one you see. */
static notify_entry g_entries[NOTIFY_MAX];
static int g_next = 0;

/* Pushed from whichever thread noticed the change -- keybinds run on the
 * present thread, but a feature that reverts itself does so on the game
 * thread -- and read by the present thread while drawing. */
static CRITICAL_SECTION g_lock;
static bool g_lock_ready = false;

static const char *const g_position_names[NOTIFY_POSITION_COUNT] = {
	"Top right",
	"Top left",
	"Top centre",
	"Bottom right",
	"Bottom left",
};

const char *notify_position_name(int position) {
	if (position < 0 || position >= NOTIFY_POSITION_COUNT) return "?";
	return g_position_names[position];
}

void notify_init(void) {
	if (g_lock_ready) return;
	InitializeCriticalSection(&g_lock);
	g_lock_ready = true;
}

/* lstrcatA with a destination limit, which the Win32 one lacks. */
static void lstrcatA_bounded(char *dst, const char *suffix, int size) {
	int len = lstrlenA(dst);
	if (len >= size - 1) return;
	lstrcpynA(dst + len, suffix, size - len);
}

static void push_colored(const char *text, ImU32 color) {
	if (!text || !text[0]) return;
	if (!g_lock_ready) return;

	EnterCriticalSection(&g_lock);
	notify_entry *entry = &g_entries[g_next];
	g_next = (g_next + 1) % NOTIFY_MAX;

	lstrcpynA(entry->text, text, NOTIFY_TEXT_MAX);
	entry->posted_ms = (unsigned long long)GetTickCount64();
	entry->color = color;
	LeaveCriticalSection(&g_lock);
}

void notify_push(const char *text) {
	push_colored(text, IM_COL32(235, 235, 235, 255));
}

void notify_toggle(const char *name, bool on) {
	if (!name) return;

	/* Built by hand rather than with wsprintfA, which has no output bound --
	 * a label longer than this buffer would smash the stack instead of
	 * truncating, and nothing at the call site hints that a limit exists. */
	char line[NOTIFY_TEXT_MAX];
	lstrcpynA(line, name, NOTIFY_TEXT_MAX);
	lstrcatA_bounded(line, on ? ": ON" : ": OFF", NOTIFY_TEXT_MAX);
	/* Colour carries the state as well as the word, so a toast is readable
	 * at a glance without actually reading it. */
	push_colored(line, on ? IM_COL32(120, 255, 140, 255) : IM_COL32(170, 170, 170, 255));
}

void notify_warn(const char *text) {
	push_colored(text, IM_COL32(255, 170, 80, 255));
}

/* Seconds a toast has been up, or a large number if the slot is empty. */
static float entry_age(const notify_entry *entry, unsigned long long now_ms) {
	if (entry->posted_ms == 0) return 1e9f;
	return (float)(now_ms - entry->posted_ms) / 1000.0f;
}

bool notify_active(void) {
	if (!notify_enabled || !g_lock_ready) return false;

	const unsigned long long now = (unsigned long long)GetTickCount64();
	bool live = false;

	EnterCriticalSection(&g_lock);
	for (int i = 0; i < NOTIFY_MAX; i++) {
		if (entry_age(&g_entries[i], now) < notify_duration) {
			live = true;
			break;
		}
	}
	LeaveCriticalSection(&g_lock);
	return live;
}

void notify_render(void) {
	if (!notify_enabled || !g_lock_ready) return;

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	if (!(screen.x > 0.0f) || !(screen.y > 0.0f)) return;

	const float margin = 14.0f;
	const float spacing = 4.0f;
	const bool bottom = (notify_position == NOTIFY_BOTTOM_RIGHT ||
	                     notify_position == NOTIFY_BOTTOM_LEFT);

	ImDrawList *draw = ImGui::GetBackgroundDrawList();
	const unsigned long long now = (unsigned long long)GetTickCount64();
	float offset = 0.0f;

	EnterCriticalSection(&g_lock);
	/* Newest first, walking the ring backwards, so the toast that just
	 * appeared is the one nearest the corner rather than the one pushed
	 * furthest along by whatever came before it. */
	for (int n = 0; n < NOTIFY_MAX; n++) {
		const int i = ((g_next - 1 - n) % NOTIFY_MAX + NOTIFY_MAX) % NOTIFY_MAX;
		const notify_entry *entry = &g_entries[i];

		const float age = entry_age(entry, now);
		if (age >= notify_duration) continue;

		/* Full opacity until the tail end, then out. */
		const float remaining = notify_duration - age;
		float alpha = 1.0f;
		if (remaining < NOTIFY_FADE_SECONDS) alpha = remaining / NOTIFY_FADE_SECONDS;
		if (alpha < 0.0f) alpha = 0.0f;
		if (alpha > 1.0f) alpha = 1.0f;

		const ImVec2 size = ImGui::CalcTextSize(entry->text);

		float x;
		switch (notify_position) {
		case NOTIFY_TOP_LEFT:
		case NOTIFY_BOTTOM_LEFT:
			x = margin;
			break;
		case NOTIFY_TOP_CENTER:
			x = (screen.x - size.x) * 0.5f;
			break;
		default: /* the two right-hand corners */
			x = screen.x - margin - size.x;
			break;
		}

		const float y = bottom ? (screen.y - margin - size.y - offset)
		                       : (margin + offset);
		offset += size.y + spacing;

		/* Same shadow the ESP labels use -- this lands on whatever the game
		 * is drawing, which is often near-black and occasionally not. */
		const ImU32 shadow = IM_COL32(0, 0, 0, (int)(200.0f * alpha));
		const ImU32 color = (entry->color & 0x00FFFFFF) |
		                    ((ImU32)(255.0f * alpha) << IM_COL32_A_SHIFT);
		draw->AddText(ImVec2(x + 1.0f, y + 1.0f), shadow, entry->text);
		draw->AddText(ImVec2(x, y), color, entry->text);
	}
	LeaveCriticalSection(&g_lock);
}
