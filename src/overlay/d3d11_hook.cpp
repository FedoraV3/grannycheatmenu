#include "overlay/d3d11_hook.h"
#include "game/ai_granny_hook.h"
#include "game/fullbright.h"
#include "game/granny_ai.h"
#include "game/offsets.h"
#include "game/player.h"
#include "game/traps.h"
#include "game/spawn.h"
#include "core/keybinds.h"
#include "core/config.h"
#include "overlay/esp.h"
#include "MinHook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

/* imgui_impl_win32.h deliberately omits this declaration to avoid dragging
 * <windows.h> into every consumer -- we already include it above, so we
 * forward-declare it ourselves, exactly as the header's comment instructs. */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HRESULT(STDMETHODCALLTYPE *present_fn_t)(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags);
typedef HRESULT(STDMETHODCALLTYPE *resize_buffers_fn_t)(IDXGISwapChain *swap_chain, UINT buffer_count,
                                                          UINT width, UINT height, DXGI_FORMAT new_format,
                                                          UINT swap_chain_flags);

static present_fn_t original_present = nullptr;
static resize_buffers_fn_t original_resize_buffers = nullptr;

static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_context = nullptr;
static ID3D11RenderTargetView *g_render_target = nullptr;
static HWND g_game_hwnd = nullptr;
static WNDPROC g_original_wndproc = nullptr;
static bool g_imgui_initialized = false;
static bool g_menu_visible = false;

/* ImGui has one global context and is not thread-safe, but we touch it from
 * two threads: menu_wndproc runs on whichever thread pumps the game's
 * message loop (Unity's main thread), while render_frame runs on the D3D11
 * present thread.
 *
 * ImGui_ImplWin32_WndProcHandler pushes into g.InputEventsQueue -- an
 * ImVector -- exactly while NewFrame() is draining and clearing it. That
 * race trips an "i >= 0 && i < Size" assert deep inside ImVector, far from
 * anything that looks like the cause. This serialises the two. */
static CRITICAL_SECTION g_imgui_lock;
static bool g_imgui_lock_ready = false;

static bool g_stop_granny_failed = false;

/* Read from the game thread; see overlay_wants_keyboard(). A lone bool, and
 * a frame of staleness either way costs nothing. */
static volatile bool g_overlay_wants_keyboard = false;

extern "C" int overlay_wants_keyboard(void) {
    return g_overlay_wants_keyboard ? 1 : 0;
}

static void create_render_target(IDXGISwapChain *swap_chain) {
    ID3D11Texture2D *back_buffer = nullptr;
    swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
    if (back_buffer) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
        back_buffer->Release();
    }
}

static void release_render_target() {
    if (g_render_target) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

/**
 * @brief Subclassed window proc, installed over the game's own once ImGui
 * is initialized.
 *
 * Feeds every message to ImGui's Win32 backend first. While the menu is
 * open, mouse/keyboard messages ImGui says it wants (io.WantCaptureMouse /
 * io.WantCaptureKeyboard -- true while hovering/typing into a widget) are
 * swallowed here instead of reaching the game, so clicking a button doesn't
 * also fire a click in-game. Everything else, and everything while the menu
 * is closed, passes through to the original window proc untouched.
 *
 * Note: a game reading mouse look via raw input (RegisterRawInputDevices)
 * rather than WM_MOUSEMOVE can bypass this entirely -- swallowing window
 * messages won't stop camera movement in that case. Not handled here.
 */
static LRESULT CALLBACK menu_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_menu_visible && g_imgui_lock_ready) {
        bool swallow = false;

        /* Held only across the ImGui calls, not CallWindowProcW -- passing a
         * message down to the game while holding this would invite a
         * deadlock. */
        EnterCriticalSection(&g_imgui_lock);
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            swallow = true;
        } else {
            ImGuiIO &io = ImGui::GetIO();
            if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
                swallow = true;
            } else if (io.WantCaptureKeyboard && msg >= WM_KEYFIRST && msg <= WM_KEYLAST) {
                swallow = true;
            }
        }
        LeaveCriticalSection(&g_imgui_lock);

        if (swallow) return TRUE;
    }
    return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
}

/* Runs once, the first time hooked_present fires -- this is the earliest
 * point we have a live device/context/window to hand ImGui. */
static void init_imgui(IDXGISwapChain *swap_chain) {
    /* Must be ready before the window proc is subclassed at the end of this
     * function, since that's the moment the other thread can start calling
     * into ImGui. Guarded because the failure paths below leave
     * g_imgui_initialized false, so this function can be entered again on a
     * later Present -- re-initialising a live CRITICAL_SECTION is undefined. */
    if (!g_imgui_lock_ready) {
        InitializeCriticalSection(&g_imgui_lock);
        g_imgui_lock_ready = true;
    }

    /* Bail rather than dereference a null device -- and leave
     * g_imgui_initialized false so the next Present retries instead of
     * faulting here every frame. */
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_device))) ||
        !g_device) {
        OutputDebugStringA("[cheat] IDXGISwapChain::GetDevice failed, ImGui not initialized");
        return;
    }
    g_device->GetImmediateContext(&g_context);
    if (!g_context) {
        OutputDebugStringA("[cheat] GetImmediateContext failed, ImGui not initialized");
        g_device->Release();
        g_device = nullptr;
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    swap_chain->GetDesc(&desc);
    g_game_hwnd = desc.OutputWindow;

    create_render_target(swap_chain);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_game_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(menu_wndproc)));

    g_imgui_initialized = true;
    OutputDebugStringA("[cheat] ImGui initialized (Insert to toggle menu)");
}

/*
 * The toggles whose side effects are more than a bool -- Immortality's hook
 * state and death patch, the trap patches -- live with the code they touch
 * (granny_apply_immortality, traps_apply). Both read a flag that has already
 * been flipped and make the game agree with it, so a keybind, a checkbox and
 * a loaded config all take the same path instead of three copies that drift.
 */

static void draw_player_tab() {
	if (ImGui::Checkbox("Immortality", &immortality)) {
		granny_apply_immortality();
	}

	ImGui::Separator();

	/* Both of these are applied on the game thread by player_tick(); the
	 * widgets only set flags. */
	ImGui::Checkbox("Noclip", &player_noclip_enabled);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Walk through walls. Space rises, Ctrl descends.\n"
		                  "Switching it off while inside geometry can wedge you.");
	}

	ImGui::Checkbox("No hard landing", &player_no_hard_landing);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Drop from any height and keep walking.\n"
		                  "Removes the stagger and the get-up animation.\n"
		                  "Fatal falls stop being fatal too -- same counter.");
	}

	bool speed_changed = ImGui::Checkbox("Override move speed", &player_speed_enabled);
	ImGui::BeginDisabled(!player_speed_enabled);
	/* A multiplier rather than an absolute speed: the game keeps a separate
	 * standing and crouched speed, and scaling both preserves the difference
	 * instead of flattening them to one number. */
	speed_changed |= ImGui::SliderFloat("Move speed", &player_speed_multiplier, 0.1f, 10.0f, "%.2fx");
	ImGui::EndDisabled();
	if (speed_changed) player_mark_dirty();
	if (player_speed_enabled) {
		ImGui::TextDisabled("Unchecking restores the game's own speeds.");
	}
}

static void draw_granny_tab() {
	/* A button, not a checkbox: StopAI is one-shot and irreversible, so a
	 * checkbox would imply unchecking undoes it, which it never did. The old
	 * one also silently reverted itself when she wasn't spawned, which just
	 * looked like it refused to turn on. */
	if (granny_is_stopped()) {
		ImGui::BeginDisabled();
		ImGui::Button("Stop granny");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "stopped");
	} else if (ImGui::Button("Stop granny")) {
		if (!granny_stop_ai()) {
			/* Only fails when nothing is ticking -- she isn't spawned, or
			 * she's switched off in the game's own options. */
			g_stop_granny_failed = true;
		} else {
			g_stop_granny_failed = false;
		}
	}

	ImGui::TextDisabled("Permanent -- she returns only on respawn or restart.");
	if (g_stop_granny_failed) {
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
		                   "No Granny in the level right now.");
	}

	/* Re-applied every FixedUpdate tick rather than on toggle, since the
	 * game's BlindTimer clears IsBlind by itself. */
	ImGui::Checkbox("Blind (ignore sight)", &granny_is_blind);

	/* Applied on the game thread by granny_ai_tick(). The sliders seed
	 * themselves from her real speeds the first time she's seen, so they
	 * start at the difficulty's values rather than arbitrary ones. */
	bool speed_changed = ImGui::Checkbox("Override speed", &granny_speed_enabled);
	ImGui::BeginDisabled(!granny_speed_enabled);
	speed_changed |= ImGui::SliderFloat("Walk speed", &granny_walk_speed, 0.0f, 500.0f, "%.2f");
	speed_changed |= ImGui::SliderFloat("Run speed", &granny_run_speed, 0.0f, 500.0f, "%.2f");
	ImGui::EndDisabled();
	if (granny_speed_enabled) {
		/* Ctrl+click a slider to type an exact value -- dragging to a
		 * specific number is hopeless over a 0..500 range. */
		ImGui::TextDisabled("Ctrl+click a slider to type a value.");
	}
	/* The write happens on the game thread, and only when something has
	 * actually changed -- not every tick. */
	if (speed_changed) granny_speed_mark_dirty();
	if (granny_speed_enabled) {
		ImGui::TextDisabled("Unchecking restores her original speeds.");
	}

	ImGui::Separator();

	/* Freeze shares the speed override's saved originals, so it has to raise
	 * the same dirty flag -- without it, unchecking freeze would never reach
	 * the restore branch and she'd stay pinned at zero. */
	if (ImGui::Checkbox("Freeze in place", &granny_freeze_enabled)) {
		granny_speed_mark_dirty();
	}
	if (granny_freeze_enabled) {
		ImGui::TextDisabled("Overrides the speed sliders while it's on.");
	}

	/* Re-applied every FixedUpdate tick like Blind -- the game keeps handing
	 * her fresh noises, so a single write wouldn't hold. */
	ImGui::Checkbox("Deaf (ignore sound)", &granny_is_deaf);
}

static void draw_world_tab() {
	/* Writes to code pages, never into IL2CPP, so it's safe straight from
	 * this thread -- unlike the spawn below. */
	if (ImGui::Checkbox("Disable traps", &traps_disabled)) {
		traps_apply();
	}
	ImGui::TextDisabled("Bear traps, poison, explosives and the generic trigger.");
	ImGui::TextDisabled("The traps stay visible -- they just stop firing.");

	ImGui::Separator();
	ImGui::TextDisabled("Spawn item");

	/* The list is ItemSpawn's own 55 slots, in its dispatch order, so what
	 * you pick here is exactly what CountItem selects. */
	if (ImGui::BeginCombo("Item", spawn_item_name(spawn_selected_index))) {
		for (int i = 0; i < spawn_item_count(); i++) {
			const bool selected = (i == spawn_selected_index);
			if (ImGui::Selectable(spawn_item_name(i), selected)) spawn_selected_index = i;
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	/* Only queues the request -- Instantiate is an IL2CPP call, so the work
	 * happens on the next PickRay tick. */
	if (ImGui::Button("Spawn")) {
		spawn_request_item(spawn_selected_index);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Lands at your drop point, same as dropping it.");

	/* Paced one per tick rather than 55 at once -- see spawn_request_all().
	 * While it runs the button turns into a cancel, since a mistaken click
	 * otherwise buries you before you can do anything about it. */
	if (spawn_bulk_active()) {
		char label[64];
		wsprintfA(label, "Stop (%d left)", spawn_bulk_remaining());
		if (ImGui::Button(label)) spawn_cancel_all();
	} else if (ImGui::Button("Spawn every item")) {
		spawn_request_all();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("All 55, dropped over about a second.");

	if (spawn_last_failed()) {
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
		                   "Last spawn failed -- is a level loaded?");
	}
}

/* Rebindable actions. The bindings themselves live in keybinds.c; this is
 * just the editor for them. */
static void draw_keybinds_tab() {
	ImGui::TextDisabled("Click a key to rebind it.");
	ImGui::TextDisabled("Esc cancels, Backspace or Delete clears.");
	ImGui::Separator();

	if (ImGui::BeginTable("keybinds", 2,
	                      ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		for (int i = 0; i < KEYBIND_COUNT; i++) {
			const keybind_id id = static_cast<keybind_id>(i);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(keybind_name(id));

			ImGui::TableNextColumn();
			/* The ##i suffix keeps two actions bound to the same key from
			 * colliding in ImGui's ID stack. */
			char label[96];
			if (keybind_capturing() == id) {
				wsprintfA(label, "press a key...##kb%d", i);
			} else {
				char key[32];
				keybind_key_name(keybind_keys[i], key, sizeof(key));
				wsprintfA(label, "%s##kb%d", key, i);
			}

			if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) {
				if (keybind_capturing() == id) {
					keybind_cancel_capture();
				} else {
					keybind_begin_capture(id);
				}
			}
		}
		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::TextDisabled("Defaults avoid the letter keys -- Granny uses most of");
	ImGui::TextDisabled("them, and a cheat key that also drops your item is");
	ImGui::TextDisabled("worse than no cheat key.");
}

static void draw_visuals_tab() {
	/* These two are live -- the ESP layer renders as soon as they're on.
	 * Until the camera matrix is sourced from the game it just draws a
	 * status line saying so, rather than nothing at all. */
	ImGui::Checkbox("Granny ESP", &esp_granny_enabled);
	ImGui::Checkbox("Item ESP", &esp_items_enabled);

	/* The cellar's AI_MomSpider -- a different class from the attic spider
	 * that stings you, and the only other thing in the game that hunts. */
	ImGui::Checkbox("Mom Spider ESP", &esp_momspider_enabled);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The big spider in the cellar.\n"
		                  "Nothing to show anywhere else in the house.");
	}

	ImGui::Checkbox("Off-screen arrows", &esp_arrows_enabled);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Points at whichever enemies are switched on above,\n"
		                  "while they're off the screen or behind you.\n"
		                  "Distance in metres beside each arrow.");
	}
	if (esp_arrows_enabled) {
		ImGui::Indent();
		ImGui::SliderFloat("Arrow size", &esp_arrow_size, 10.0f, 60.0f, "%.0f px");
		ImGui::SliderFloat("Arrow inset", &esp_arrow_margin, 20.0f, 200.0f, "%.0f px");
		ImGui::Unindent();
	}

	/* Categories are the game's own, straight off ItemSeedData::category. */
	ImGui::Indent();
	ImGui::TextDisabled("Item categories");
	ImGui::Checkbox("Escape only", &esp_show_escape_items);
	ImGui::Checkbox("Escape + puzzle", &esp_show_escape_puzzle_items);
	ImGui::Checkbox("Puzzle only", &esp_show_puzzle_items);
	ImGui::Checkbox("Other / free", &esp_show_other_items);
	ImGui::Unindent();

	/* Her model's real height in world units isn't something we can read
	 * cheaply, so tune the box by eye instead of rebuilding to guess. */
	ImGui::SliderFloat("Box height", &esp_box_height, 0.5f, 5.0f, "%.2f");
	ImGui::SliderFloat("Box width", &esp_box_width_ratio, 0.1f, 1.5f, "%.2f");

	/* Separate from Granny's: the spider is low and wide where she is tall
	 * and narrow, so one pair of numbers can't flatter both. */
	ImGui::SliderFloat("Spider height", &esp_spider_box_height, 0.3f, 4.0f, "%.2f");
	ImGui::SliderFloat("Spider width", &esp_spider_box_width_ratio, 0.2f, 3.0f, "%.2f");

	ImGui::SliderInt("Items: scan limit", &esp_item_scan_limit, 0, 35);
	ImGui::Checkbox("Items: log each slot", &esp_items_verbose);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Logs every slot to DebugView before touching it.\n"
		                  "After a crash, the last line names the culprit.");
	}
	ImGui::Checkbox("Items: ignore active check", &esp_items_ignore_active);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Debug: draw items even if inactive.\n"
		                  "Sensible positions = the active check is wrong.\n"
		                  "All at one spot = they're prefabs, not scene objects.");
	}

	ImGui::Separator();
	/* Applied on the game's main thread by fullbright_tick(); this only
	 * flips the flag. */
	ImGui::Checkbox("Fullbright", &fullbright_enabled);
}

/* Live state, for working out what's actually resolved at runtime while
 * reverse engineering. */
static void draw_debug_tab() {
	static const struct {
		const char *name;
		uintptr_t rva;
	} offset_table[] = {
		{ "PlayerStatus::GrannyCaughtYou",    OFFSET_PlayerStatus_GrannyCaughtYou },
		{ "PlayerStatus::GrannyCaughtYouBed", OFFSET_PlayerStatus_GrannyCaughtYouBed },
		{ "PlayerStatus::KnockDeath",         OFFSET_PlayerStatus_KnockDeath },
		{ "PlayerStatus::NormalDeath",        OFFSET_PlayerStatus_NormalDeath },
		{ "AI_Granny::ResetAIDecision",       OFFSET_AI_Granny_ResetAIDecision },
		{ "AI_Granny::StopAI",                OFFSET_AI_Granny_StopAI },
		{ "AI_Granny::ChaseAction",           OFFSET_AI_Granny_ChaseAction },
		{ "AI_Granny::SmackTimer",            OFFSET_AI_Granny_SmackTimer },
		{ "AI_Granny::FixedUpdate",           OFFSET_AI_Granny_FixedUpdate },
	};

	uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
	void *granny = ai_granny_current();

	ImGui::Text("GameAssembly.dll  0x%llX", (unsigned long long)base);
	if (granny) {
		ImGui::Text("AI_Granny inst.   0x%llX", (unsigned long long)(uintptr_t)granny);
	} else {
		ImGui::TextDisabled("AI_Granny inst.   <none ticking yet>");
	}

	esp_debug_info esp;
	esp_get_debug_info(&esp);
	ImGui::Separator();
	ImGui::Text("FixedUpdate hook   %lu ticks", esp.granny_ticks);
	ImGui::Text("PickRay hook       %lu ticks", esp.pickray_ticks);
	ImGui::Text("MomSpider hook     %lu ticks", esp.spider_ticks);
	ImGui::Text("ItemSeed Awake     %lu ticks", esp.item_ticks);
	ImGui::Text("camera matrix      %s", esp.have_view_projection ? "ok" : "MISSING");
	ImGui::Text("granny position    %s", esp.have_granny_position ? "ok" : "MISSING");
	ImGui::Text("spider position    %s", esp.have_spider_position ? "ok" : "MISSING");
	ImGui::Text("player position    %s", esp.have_player_position ? "ok" : "MISSING");
	ImGui::Text("ItemSeed instance  %s  0x%llX",
	            !esp.have_item_spawn ? "MISSING" : (esp.item_spawn_alive ? "alive" : "DEAD"),
	            (unsigned long long)(uintptr_t)esp.item_spawn);
	ImGui::Text("item[0] Pliers     0x%llX", (unsigned long long)(uintptr_t)esp.item_slot0);
	ImGui::Text("items alive        %d  (of 35)", esp.items_alive);
	ImGui::Text("items visible      %d", esp.item_count);

	ImGui::Separator();
	if (ImGui::BeginTable("offsets", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
		for (const auto &entry : offset_table) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(entry.name);
			ImGui::TableNextColumn();
			ImGui::Text("0x%llX", (unsigned long long)(base + entry.rva));
		}
		ImGui::EndTable();
	}
}

static void draw_menu() {
	ImGui::SetNextWindowSize(ImVec2(440, 340), ImGuiCond_FirstUseEver);
	ImGui::Begin("grannycheat");

	if (ImGui::BeginTabBar("##tabs")) {
		if (ImGui::BeginTabItem("Player")) {
			draw_player_tab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Granny")) {
			draw_granny_tab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("World")) {
			draw_world_tab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Visuals")) {
			draw_visuals_tab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Keybinds")) {
			draw_keybinds_tab();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Debug")) {
			draw_debug_tab();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	/* Below the tabs rather than inside one: the config covers everything in
	 * the window, so burying it in a tab would make it look like it only
	 * applied to that tab's settings. */
	ImGui::Separator();
	if (ImGui::Button("Save config")) {
		config_save();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload config")) {
		config_load();
		config_apply();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%s", config_status());
	ImGui::TextDisabled("Loads by itself on injection. %s", config_path());

	ImGui::End();
}

static void render_frame() {
    /* Locked for the whole frame: NewFrame drains the input queue that the
     * window proc pushes into from the game's thread. */
    EnterCriticalSection(&g_imgui_lock);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_menu_visible) {
        draw_menu();
    }
    /* ESP draws whether or not the menu is open -- it's meant to be up
     * while you're actually playing. */
    esp_render();

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    LeaveCriticalSection(&g_imgui_lock);
}

/*
 * Polls the bindings once per frame and acts on whatever went down.
 *
 * Each case flips the same flag the matching checkbox does and then runs the
 * same apply function, so a key and a click are indistinguishable to the
 * rest of the cheat. The ones that need the game thread (a speed change, a
 * spawn) only raise their flag here, exactly as the widgets do.
 */
static void handle_keybinds() {
    ImGuiIO &io = ImGui::GetIO();

    /* Ctrl+clicking a slider turns it into a text box, and the digits you
     * type there must not also fire cheats. */
    const bool typing = io.WantTextInput || keybind_capturing() != KEYBIND_COUNT;
    keybinds_update(!typing);

    /* Published as a plain flag so noclip's rise and descend -- read off the
     * keyboard on the game thread, bypassing the window proc -- can stay out
     * of the way while you're typing, without touching ImGui from there. */
    g_overlay_wants_keyboard = typing;

    if (keybind_pressed(KEYBIND_MENU)) {
        g_menu_visible = !g_menu_visible;
        io.MouseDrawCursor = g_menu_visible;
    }

    if (keybind_pressed(KEYBIND_GRANNY_ESP)) esp_granny_enabled = !esp_granny_enabled;
    if (keybind_pressed(KEYBIND_ITEM_ESP))   esp_items_enabled = !esp_items_enabled;
    if (keybind_pressed(KEYBIND_MOMSPIDER_ESP)) esp_momspider_enabled = !esp_momspider_enabled;
    if (keybind_pressed(KEYBIND_ARROWS))     esp_arrows_enabled = !esp_arrows_enabled;
    if (keybind_pressed(KEYBIND_FULLBRIGHT)) fullbright_enabled = !fullbright_enabled;
    if (keybind_pressed(KEYBIND_BLIND))      granny_is_blind = !granny_is_blind;
    if (keybind_pressed(KEYBIND_DEAF))       granny_is_deaf = !granny_is_deaf;
    if (keybind_pressed(KEYBIND_NOCLIP))     player_noclip_enabled = !player_noclip_enabled;

    if (keybind_pressed(KEYBIND_IMMORTALITY)) {
        immortality = !immortality;
        granny_apply_immortality();
    }
    if (keybind_pressed(KEYBIND_DISABLE_TRAPS)) {
        traps_disabled = !traps_disabled;
        traps_apply();
    }

    if (keybind_pressed(KEYBIND_MOVE_SPEED)) {
        player_speed_enabled = !player_speed_enabled;
        player_mark_dirty();
    }
    if (keybind_pressed(KEYBIND_GRANNY_SPEED)) {
        granny_speed_enabled = !granny_speed_enabled;
        granny_speed_mark_dirty();
    }
    /* Freeze shares the speed override's saved originals, so it raises the
     * same dirty flag -- see the checkbox in the Granny tab. */
    if (keybind_pressed(KEYBIND_FREEZE)) {
        granny_freeze_enabled = !granny_freeze_enabled;
        granny_speed_mark_dirty();
    }

    /* The two one-shots. Stop granny is irreversible, so it reports failure
     * through the same flag the button uses. */
    if (keybind_pressed(KEYBIND_STOP_GRANNY)) {
        g_stop_granny_failed = !granny_stop_ai();
    }
    if (keybind_pressed(KEYBIND_SPAWN_ITEM)) {
        spawn_request_item(spawn_selected_index);
    }
    if (keybind_pressed(KEYBIND_SPAWN_ALL)) {
        /* Same button semantics: pressing it again while it runs stops it. */
        if (spawn_bulk_active()) {
            spawn_cancel_all();
        } else {
            spawn_request_all();
        }
    }
}

static HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags) {
    if (!g_imgui_initialized) {
        init_imgui(swap_chain);
    }

    /* Every path below this point touches ImGui's global context or the
     * device context, and init_imgui has two early returns that leave both
     * unset (a failed GetDevice or GetImmediateContext, e.g. after a device
     * reset). Without this guard the next Insert press dereferences a NULL
     * GImGui, and render_frame() then calls OMSetRenderTargets on a nullptr
     * context. Bail and let the next Present retry the init instead. */
    if (!g_imgui_initialized) {
        return original_present(swap_chain, sync_interval, flags);
    }

    handle_keybinds();

    /* Skip the whole ImGui frame when there's nothing to show at all. */
    if (g_menu_visible || esp_granny_enabled || esp_items_enabled) {
        render_frame();
    }

    return original_present(swap_chain, sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain *swap_chain, UINT buffer_count,
                                                          UINT width, UINT height, DXGI_FORMAT new_format,
                                                          UINT swap_chain_flags) {
    /* The render target holds a reference into the old back buffer --
     * ResizeBuffers fails if anything still references it, so drop it
     * first and recreate against the new buffers afterward.
     *
     * Locked because this runs on the game's thread while render_frame may
     * be mid-draw on the present thread: releasing the view out from under
     * an in-flight OMSetRenderTargets/RenderDrawData pair frees a COM
     * object the GPU submit still refers to. */
    bool locked = g_imgui_lock_ready;
    if (locked) EnterCriticalSection(&g_imgui_lock);

    release_render_target();
    HRESULT hr = original_resize_buffers(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    if (g_imgui_initialized) {
        create_render_target(swap_chain);
    }

    if (locked) LeaveCriticalSection(&g_imgui_lock);
    return hr;
}

/* Present and ResizeBuffers's addresses only exist inside a live
 * IDXGISwapChain's vtable, so we create a disposable one against an
 * invisible window just to read them out. */
static bool get_swapchain_functions(void **out_present, void **out_resize_buffers) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"grannycheat_dummy_wnd";
    RegisterClassExW(&wc);

    HWND dummy_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                       0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy_hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 100;
    desc.BufferDesc.Height = 100;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = dummy_hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swap_chain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL feature_level;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc,
        &swap_chain, &device, &feature_level, &context);

    bool ok = false;
    if (SUCCEEDED(hr) && swap_chain) {
        void **vtable = *reinterpret_cast<void ***>(swap_chain);
        *out_present = vtable[8];         /* IDXGISwapChain::Present */
        *out_resize_buffers = vtable[13]; /* IDXGISwapChain::ResizeBuffers */
        ok = true;
    }

    if (context) context->Release();
    if (device) device->Release();
    if (swap_chain) swap_chain->Release();
    DestroyWindow(dummy_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return ok;
}

extern "C" int d3d11_hook_install(void) {
    void *present_addr = nullptr;
    void *resize_buffers_addr = nullptr;
    if (!get_swapchain_functions(&present_addr, &resize_buffers_addr)) {
        OutputDebugStringA("[cheat] failed to resolve IDXGISwapChain vtable");
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("[cheat] MH_Initialize failed");
        return 0;
    }

    if (MH_CreateHook(present_addr, reinterpret_cast<void *>(&hooked_present),
                       reinterpret_cast<void **>(&original_present)) != MH_OK ||
        MH_CreateHook(resize_buffers_addr, reinterpret_cast<void *>(&hooked_resize_buffers),
                       reinterpret_cast<void **>(&original_resize_buffers)) != MH_OK) {
        OutputDebugStringA("[cheat] MH_CreateHook failed");
        return 0;
    }

    if (MH_EnableHook(present_addr) != MH_OK || MH_EnableHook(resize_buffers_addr) != MH_OK) {
        OutputDebugStringA("[cheat] MH_EnableHook failed");
        return 0;
    }

    OutputDebugStringA("[cheat] Present/ResizeBuffers hooked");
    return 1;
}

extern "C" void d3d11_hook_remove(void) {
    if (!original_present) {
        return;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imgui_initialized) {
        if (g_original_wndproc) {
            SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        release_render_target();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_imgui_initialized = false;
    }

    original_present = nullptr;
    original_resize_buffers = nullptr;
}
