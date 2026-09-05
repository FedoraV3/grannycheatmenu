#include "d3d11_hook.h"
#include "ai_granny_hook.h"
#include "offsets.h"
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

bool granny_cannot_kill_you = false;
bool stop_granny_ai = false;

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
    if (g_menu_visible) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            return TRUE;
        }
        ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
            return TRUE;
        }
        if (io.WantCaptureKeyboard && msg >= WM_KEYFIRST && msg <= WM_KEYLAST) {
            return TRUE;
        }
    }
    return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
}

/* Runs once, the first time hooked_present fires -- this is the earliest
 * point we have a live device/context/window to hand ImGui. */
static void init_imgui(IDXGISwapChain *swap_chain) {
    swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&g_device));
    g_device->GetImmediateContext(&g_context);

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

static void draw_menu() {
    ImGui::Begin("grannycheat");

	if (ImGui::Checkbox("Granny cannot kill you", &granny_cannot_kill_you)) {
		uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
		if (granny_cannot_kill_you) {
			MH_EnableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_GrannyCaughtYou));
			MH_EnableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_GrannyCaughtYouBed));

			MH_EnableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_NormalDeath));
			MH_EnableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_KnockDeath));
		} else {
			MH_DisableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_GrannyCaughtYou));
			MH_DisableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_GrannyCaughtYouBed));

			MH_DisableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_KnockDeath));
			MH_DisableHook(reinterpret_cast<LPVOID>(base + OFFSET_PlayerStatus_NormalDeath));
		}
	}
	
	if (ImGui::Checkbox("Stop granny", &stop_granny_ai)) {
		if (stop_granny_ai) {
			void *granny = ai_granny_current();
			if (granny) {
				uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
				AI_Granny_StopAI_t stop_ai = (AI_Granny_StopAI_t)(base + OFFSET_AI_Granny_StopAI);
				stop_ai(granny);
				OutputDebugStringA("[cheat] StopAI called");
			} else {
				OutputDebugStringA("[cheat] no AI_Granny instance yet, can't call StopAI");
				stop_granny_ai = false;
			}
		} else {
			/* StopAI tears down her components one-way (see offsets.h) --
			 * there's no confirmed function that undoes it yet, so
			 * unchecking just stops re-triggering it. She stays stopped. */
			OutputDebugStringA("[cheat] Stop granny unchecked -- no known re-enable function yet, she stays stopped");
		}
	}

    ImGui::End();
}

static void render_frame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    draw_menu();

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

static HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags) {
    if (!g_imgui_initialized) {
        init_imgui(swap_chain);
    }

    /* Insert toggles the menu; rising-edge check so holding the key down
     * doesn't flicker it open/closed every frame. */
    static bool insert_was_down = false;
    bool insert_is_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insert_is_down && !insert_was_down) {
        g_menu_visible = !g_menu_visible;
        ImGui::GetIO().MouseDrawCursor = g_menu_visible;
    }
    insert_was_down = insert_is_down;

    if (g_menu_visible) {
        render_frame();
    }

    return original_present(swap_chain, sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain *swap_chain, UINT buffer_count,
                                                          UINT width, UINT height, DXGI_FORMAT new_format,
                                                          UINT swap_chain_flags) {
    /* The render target holds a reference into the old back buffer --
     * ResizeBuffers fails if anything still references it, so drop it
     * first and recreate against the new buffers afterward. */
    release_render_target();
    HRESULT hr = original_resize_buffers(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    if (g_imgui_initialized) {
        create_render_target(swap_chain);
    }
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
