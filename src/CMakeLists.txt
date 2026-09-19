include(FetchContent)

# ---- Third-party dependencies ----
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.3.268
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json imgui vulkan_headers)

# minhook is built manually below (minhook_lib), so only fetch its sources
# instead of running its CMakeLists (which would add its own install rules).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()

# ---- ImGui ----
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay/backends/dx8/imgui_impl_dx8.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx10.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
endif()
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${PROJECT_SOURCE_DIR}/sdk/dx8
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${vulkan_headers_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay/backends
)
target_compile_definitions(imgui PUBLIC UNICODE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# ---- MinHook ----
add_library(minhook_lib STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook_lib PUBLIC ${minhook_SOURCE_DIR}/include)

# ---- STAR ----
set(STAR_SOURCES
    core/art_stamp.cpp
    core/callbacks.cpp
    core/config.cpp
    core/integrity_hooks.cpp
    core/settings.cpp
    core/star_api.cpp
    core/star_flat_stubs.cpp
    core/storage.cpp
    overlay/core/overlay.cpp
    overlay/core/notes_store.cpp
    overlay/core/overlay_window.cpp
    overlay/core/overlay_input.cpp
    overlay/core/overlay_util.cpp
    overlay/ui/overlay_ui.cpp
    overlay/ui/overlay_style.cpp
    overlay/ui/overlay_icons.cpp
    overlay/ui/icon_cache.cpp
    overlay/ui/overlay_panel.cpp
    overlay/capture/overlay_screenshot.cpp
    overlay/capture/screenshot_service.cpp
    overlay/capture/overlay_capture.cpp
    overlay/backends/backend_dx9.cpp
    overlay/backends/backend_dx8.cpp
    overlay/backends/backend_dx10.cpp
    overlay/backends/backend_dx11.cpp
    overlay/backends/backend_dx12.cpp
    overlay/backends/backend_dxgi.cpp
    overlay/backends/backend_opengl.cpp
    overlay/backends/backend_vulkan.cpp
    overlay/external/external.cpp
    steam/steam_applist.cpp
    steam/steam_apps.cpp
    steam/steam_client.cpp
    steam/steam_controller.cpp
    steam/steam_friends.cpp
    steam/steam_game_server.cpp
    steam/steam_game_server_stats.cpp
    steam/steam_html_surface.cpp
    steam/steam_http.cpp
    steam/steam_input.cpp
    steam/steam_inventory.cpp
    steam/steam_matchmaking.cpp
    steam/steam_matchmaking_servers.cpp
    steam/steam_music.cpp
    steam/steam_music_remote.cpp
    steam/steam_networking.cpp
    steam/steam_remote_storage.cpp
    steam/steam_screenshots.cpp
    steam/steam_ugc.cpp
    steam/steam_unified_messages.cpp
    steam/steam_user.cpp
    steam/steam_user_stats.cpp
    steam/steam_utils.cpp
    steam/steam_video.cpp
)

add_library(star SHARED ${STAR_SOURCES})

target_compile_definitions(star PRIVATE
    STAR_API_EXPORTS
    STEAM_API_EXPORTS
    STEAM_API_FUNCTIONS_IMPL  # makes S_API = extern "C" __declspec(dllexport),
                              # matching the extern "C" definitions in star_api.cpp
                              # (without it S_API is empty -> C++ linkage -> C2732)
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

target_include_directories(star PRIVATE
    ${PROJECT_SOURCE_DIR}/sdk/dx8
    ${PROJECT_SOURCE_DIR}/sdk
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay/backends
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${vulkan_headers_SOURCE_DIR}/include
    ${minhook_SOURCE_DIR}/include
)

target_link_libraries(star PRIVATE
    imgui
    minhook_lib
    nlohmann_json::nlohmann_json
    d3d11
    d3d12
    d3d10
    dxgi
    d3dcompiler
    d3d9
    opengl32
    bcrypt
    ws2_32
    winmm
)

# Output name depends on architecture: steam_api64.dll (x64) vs steam_api.dll (x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(star PROPERTIES OUTPUT_NAME "steam_api64")
else()
    set_target_properties(star PROPERTIES OUTPUT_NAME "steam_api")
endif()

# DEF file for clean exports (no LIBRARY line > no LNK4070 mismatch on x64)
set_target_properties(star PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /DEF:${CMAKE_CURRENT_SOURCE_DIR}/steam_api.def"
)

if(MSVC)
    target_compile_options(star PRIVATE /W3 /MP)
    target_link_options(star PRIVATE /MAP)
    # Debug info in release for debugging if needed
    target_compile_options(star PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(star PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()

install(TARGETS star RUNTIME DESTINATION .)

add_subdirectory(proxy)
