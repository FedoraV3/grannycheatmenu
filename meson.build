project('grannycheat', 'c', 'cpp')

minhook_inc = include_directories('third_party/minhook/include')

minhook_lib = static_library('minhook',
    [
        'third_party/minhook/src/buffer.c',
        'third_party/minhook/src/hook.c',
        'third_party/minhook/src/trampoline.c',
        'third_party/minhook/src/hde/hde32.c',
        'third_party/minhook/src/hde/hde64.c',
    ],
    include_directories: minhook_inc,
)

# Core Dear ImGui plus the Win32 + DX11 backends, since that's the pair
# Granny (Unity/DX11) needs -- the platform backend feeds window messages
# in, the renderer backend draws ImGui's output with the game's own device.
imgui_inc = include_directories('third_party/imgui')

imgui_lib = static_library('imgui',
    [
        'third_party/imgui/imgui.cpp',
        'third_party/imgui/imgui_draw.cpp',
        'third_party/imgui/imgui_tables.cpp',
        'third_party/imgui/imgui_widgets.cpp',
        'third_party/imgui/imgui_demo.cpp',
        'third_party/imgui/backends/imgui_impl_win32.cpp',
        'third_party/imgui/backends/imgui_impl_dx11.cpp',
    ],
    include_directories: imgui_inc,
)

cpp = meson.get_compiler('cpp')
d3d11_dep = cpp.find_library('d3d11')
dxgi_dep = cpp.find_library('dxgi')
d3dcompiler_dep = cpp.find_library('d3dcompiler')   # D3DCompile, used by ImGui's DX11 backend for its shaders
dwmapi_dep = cpp.find_library('dwmapi')             # Dwm* calls, used by ImGui's Win32 backend

# Sources are grouped by role, and includes are path-qualified to match
# ("game/offsets.h" rather than "offsets.h"), so each include states which
# layer it reaches into.
#
#   core    -- entry point and game-agnostic memory utilities
#   game    -- IL2CPP offsets and the hooks onto Granny's own classes
#   overlay -- the D3D11 present hook, ImGui menu, and ESP rendering
cheat_sources = [
    'src/core/dllmain.c',
    'src/core/config.c',
    'src/core/keybinds.c',
    'src/core/patch_local.c',

    'src/game/ai_granny_hook.c',
    'src/game/fullbright.c',
    'src/game/granny_ai.c',
    'src/game/player.c',
    'src/game/spawn.c',
    'src/game/traps.c',

    'src/overlay/d3d11_hook.cpp',
    'src/overlay/esp.cpp',
]

shared_library('cheat', cheat_sources,
    include_directories: [include_directories('include'), minhook_inc, imgui_inc],
    link_with: [minhook_lib, imgui_lib],
    dependencies: [d3d11_dep, dxgi_dep, d3dcompiler_dep, dwmapi_dep],
    link_language: 'cpp',
)
