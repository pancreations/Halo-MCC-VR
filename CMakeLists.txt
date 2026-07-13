cmake_minimum_required(VERSION 3.24)
project(halo3xr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so users don't need any Visual C++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# --- OpenXR loader (static) ---
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.61)

# --- MinHook (static) ---
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4)

# --- Dear ImGui (no CMakeLists upstream; we build it ourselves below) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8)

FetchContent_MakeAvailable(openxr minhook imgui)

# The OpenXR build forces the dynamic CRT onto the static loader, which
# conflicts with our static-CRT build; force it back so everything matches.
set_target_properties(openxr_loader PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

# --- The mod DLL, injected into MCC-Win64-Shipping.exe ---
add_library(halo3xr SHARED
    src/common/log.cpp
    src/common/config.cpp
    src/dll/dllmain.cpp
    src/dll/d3d11_hook.cpp
    src/dll/vr.cpp
    src/dll/menu.cpp)
target_compile_definitions(halo3xr PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    XR_USE_GRAPHICS_API_D3D11 XR_USE_PLATFORM_WIN32)
target_link_libraries(halo3xr PRIVATE
    openxr_loader minhook imgui_lib
    d3d11 dxgi d3dcompiler)

# --- The launcher: starts MCC without EAC and injects the DLL ---
add_executable(halo3xr_launcher WIN32 src/launcher/launcher.cpp)
target_compile_definitions(halo3xr_launcher PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# --- "dist" layout: everything a user needs, in one folder ---
# (installed via:  cmake --install build --config Release --prefix dist --component dist
#  so the dependencies' own install rules don't pollute the folder)
install(TARGETS halo3xr halo3xr_launcher RUNTIME DESTINATION . COMPONENT dist)
install(FILES installer/install.bat installer/uninstall.bat DESTINATION . COMPONENT dist)
