#include "core/crashlog.h"

#include "game/ai_granny_hook.h"
#include "game/granny_ai.h"
#include "game/player.h"
#include "game/traps.h"
#include "game/unlock.h"
#include "overlay/esp.h"

#include <windows.h>
#include <stdio.h>

/*
 * A diagnostic, so it reaches across the folder layout to name what was
 * switched on when things went wrong. That is the single most useful line in
 * the file: it turns "it crashed" into "it crashed with these four features
 * on", which is most of the way to a repro.
 */

/* Breadcrumbs. Static strings only -- storing the pointer means a mark costs
 * one store, which is what makes it affordable in a per-frame hook. */
#define CRUMB_COUNT 8
static const char *volatile g_crumbs[CRUMB_COUNT];
static volatile long g_crumb_next = 0;

static volatile long g_logged = 0;
static bool g_installed = false;

void crashlog_mark(const char *what) {
	const long slot = InterlockedIncrement(&g_crumb_next) - 1;
	g_crumbs[((slot % CRUMB_COUNT) + CRUMB_COUNT) % CRUMB_COUNT] = what;
}

static void log_line(FILE *file, const char *text) {
	if (file) fputs(text, file);
	OutputDebugStringA(text);
}

/* Resolves an address to "module+RVA", which is what can actually be looked
 * up afterwards -- a raw address means nothing once the process is gone. */
static void describe_address(void *address, char *out, int size) {
	HMODULE module = NULL;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)address, &module) &&
	    module != NULL) {
		char path[MAX_PATH];
		if (GetModuleFileNameA(module, path, MAX_PATH)) {
			const char *name = path;
			for (const char *p = path; *p; p++) {
				if (*p == '\\' || *p == '/') name = p + 1;
			}
			wsprintfA(out, "%s+0x%llX", name,
			          (unsigned long long)((uintptr_t)address - (uintptr_t)module));
			return;
		}
	}
	wsprintfA(out, "0x%llX (no module)", (unsigned long long)(uintptr_t)address);
	(void)size;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info) {
	const DWORD code = info->ExceptionRecord->ExceptionCode;

	/* Only the ones that actually mean something broke. IL2CPP raises its
	 * managed exceptions through explicit checks rather than by faulting, so
	 * an access violation here is genuine rather than routine traffic. */
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
	    code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR &&
	    code != EXCEPTION_STACK_OVERFLOW) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	/* A fault inside the handler, or a fault storm, must not spin forever. */
	if (InterlockedIncrement(&g_logged) > 4) return EXCEPTION_CONTINUE_SEARCH;

	char path[MAX_PATH];
	char appdata[MAX_PATH];
	FILE *file = NULL;
	if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) {
		wsprintfA(path, "%s\\grannycheat", appdata);
		CreateDirectoryA(path, NULL);
		wsprintfA(path, "%s\\grannycheat\\crash.log", appdata);
		file = fopen(path, "a");
	}

	char line[512];
	char where[MAX_PATH + 64];

	SYSTEMTIME now;
	GetLocalTime(&now);
	wsprintfA(line, "\n=== grannycheat fault %04d-%02d-%02d %02d:%02d:%02d ===\n",
	          now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	log_line(file, line);

	describe_address(info->ExceptionRecord->ExceptionAddress, where, (int)sizeof(where));
	wsprintfA(line, "code 0x%08X at %s\n", (unsigned int)code, where);
	log_line(file, line);

	if (code == EXCEPTION_ACCESS_VIOLATION &&
	    info->ExceptionRecord->NumberParameters >= 2) {
		wsprintfA(line, "  %s address 0x%llX\n",
		          info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
		          (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
		log_line(file, line);
	}

	/* Where our own code lives, so an RVA in the line above can be matched
	 * against the map even if the fault was inside the game. */
	HMODULE self = NULL;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)&on_exception, &self)) {
		wsprintfA(line, "  cheat base 0x%llX  GameAssembly base 0x%llX\n",
		          (unsigned long long)(uintptr_t)self,
		          (unsigned long long)(uintptr_t)GetModuleHandleW(L"GameAssembly.dll"));
		log_line(file, line);
	}

	wsprintfA(line, "features: %s%s%s%s%s%s%s%s%s%s\n",
	          immortality ? "immortality " : "",
	          granny_is_blind ? "blind " : "",
	          granny_is_deaf ? "deaf " : "",
	          granny_freeze_enabled ? "freeze " : "",
	          granny_speed_enabled ? "grannyspeed " : "",
	          player_noclip_enabled ? "noclip " : "",
	          player_speed_enabled ? "movespeed " : "",
	          player_air_control ? "aircontrol " : "",
	          traps_disabled ? "traps " : "",
	          unlock_enabled ? "unlock " : "");
	log_line(file, line);

	wsprintfA(line, "esp: %s%s%s\n",
	          esp_granny_enabled ? "granny " : "",
	          esp_items_enabled ? "items " : "",
	          esp_momspider_enabled ? "spider " : "");
	log_line(file, line);

	/* Newest crumb first: what we were doing when it went wrong. */
	const long next = g_crumb_next;
	for (int i = 1; i <= CRUMB_COUNT; i++) {
		const long slot = next - i;
		if (slot < 0) break;
		const char *crumb = g_crumbs[((slot % CRUMB_COUNT) + CRUMB_COUNT) % CRUMB_COUNT];
		if (!crumb) continue;
		wsprintfA(line, "  [-%d] %s\n", i, crumb);
		log_line(file, line);
	}

	if (file) fclose(file);

	/* Let whatever would normally happen still happen -- this only watches. */
	return EXCEPTION_CONTINUE_SEARCH;
}

void crashlog_init(void) {
	if (g_installed) return;
	g_installed = true;
	/* First in the chain, so it runs before Unity's own handler. */
	AddVectoredExceptionHandler(1, on_exception);
	OutputDebugStringA("[cheat] crash logger installed");
}
