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

shared_library('cheat', ['src/dllmain.c', 'src/patch_local.c', 'src/d3d11_hook.cpp', 'src/ai_granny_hook.c', 'src/esp.cpp'],
    include_directories: [include_directories('include'), minhook_inc, imgui_inc],
    link_with: [minhook_lib, imgui_lib],
    dependencies: [d3d11_dep, dxgi_dep, d3dcompiler_dep, dwmapi_dep],
    link_language: 'cpp',
)
