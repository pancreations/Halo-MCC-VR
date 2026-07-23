cmake_minimum_required(VERSION 3.24)
project(halomccvr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so users do not need a separate Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
include(CTest)

# Put a source identity in the first log line. The package command always
# reconfigures from a clean commit, so changing commits recompiles dllmain.cpp.
find_package(Git QUIET)
set(HALOMCCVR_BUILD_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
            rev-parse HEAD
        OUTPUT_VARIABLE HALOMCCVR_BUILD_COMMIT
        RESULT_VARIABLE HALOMCCVR_GIT_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT HALOMCCVR_GIT_RESULT EQUAL 0)
        set(HALOMCCVR_BUILD_COMMIT "unknown")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                status --porcelain --untracked-files=normal
            OUTPUT_VARIABLE HALOMCCVR_GIT_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT HALOMCCVR_GIT_STATUS STREQUAL "")
            string(APPEND HALOMCCVR_BUILD_COMMIT "-dirty")
        endif()
    endif()
endif()

# The accepted 0.2.1 product is one cumulative Halo 3 + ODST build. OFF remains
# available only for a deliberate Halo 3 isolation build.
option(HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    "Enable the cumulative Halo 3 + ODST runtime" ON)

if(HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP)
    set(HALOMCCVR_BUILD_ODST "ON")
    message(STATUS "Halo MCC VR: cumulative Halo 3 + ODST runtime enabled")
else()
    set(HALOMCCVR_BUILD_ODST "OFF")
    message(WARNING
        "Halo MCC VR: ODST is disabled; this does not match release 0.2.1")
endif()
message(STATUS
    "Halo MCC VR build identity: ${HALOMCCVR_BUILD_COMMIT}, ODST=${HALOMCCVR_BUILD_ODST}")

# Exact dependency commits used by the accepted release source. Shallow clones
# avoid pulling entire third-party histories into the ignored build tree.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG 5267613edf3d937e3d77556a106a65c2f82b25c6
    GIT_SHALLOW TRUE)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(openxr minhook imgui)

# OpenXR requests the dynamic CRT for its static loader; keep it consistent with
# the mod's static-CRT build.
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

add_library(halo3xr SHARED
    src/common/log.cpp
    src/common/config.cpp
    src/common/input_logic.cpp
    src/common/scope_logic.cpp
    src/common/title_registry.cpp
    src/dll/dllmain.cpp
    src/dll/d3d11_hook.cpp
    src/dll/vr.cpp
    src/dll/menu.cpp
    src/dll/sigscan.cpp
    src/dll/game.cpp
    src/dll/ik.cpp
    src/dll/input.cpp
    src/dll/title_adapter.cpp)
target_compile_definitions(halo3xr PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    XR_USE_GRAPHICS_API_D3D11 XR_USE_PLATFORM_WIN32
    HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=$<BOOL:${HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP}>
    HALOMCCVR_BUILD_COMMIT="${HALOMCCVR_BUILD_COMMIT}"
    HALOMCCVR_BUILD_ODST="${HALOMCCVR_BUILD_ODST}")
target_link_libraries(halo3xr PRIVATE
    openxr_loader minhook imgui_lib
    d3d11 dxgi d3dcompiler)

add_executable(halo3xr_launcher WIN32 src/launcher/launcher.cpp)
target_compile_definitions(halo3xr_launcher PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Opt-in reverse-engineering helper; never shipped or built by the default
# preset. Some diagnostic modes write to a running game process.
add_executable(camscan EXCLUDE_FROM_ALL tools/camscan.cpp)
target_compile_definitions(camscan PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(camscan PRIVATE psapi)

if(BUILD_TESTING)
    add_executable(halomccvr_core_tests
        tests/core_tests.cpp
        src/common/input_logic.cpp
        src/common/scope_logic.cpp
        src/common/title_registry.cpp
        src/common/config.cpp
        src/common/log.cpp)
    target_include_directories(halomccvr_core_tests PRIVATE src/common)
    target_compile_definitions(halomccvr_core_tests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=$<BOOL:${HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP}>)
    add_test(NAME halomccvr_core_tests COMMAND halomccvr_core_tests)
endif()

# Stage an unaccepted local candidate only. Release-specific hashes and metadata
# must never be copied into a newly rebuilt package.
install(TARGETS halo3xr halo3xr_launcher
    RUNTIME DESTINATION .
    COMPONENT dist)
install(FILES
    LICENSE
    installer/MANUAL-README.txt
    DESTINATION .
    COMPONENT dist)
