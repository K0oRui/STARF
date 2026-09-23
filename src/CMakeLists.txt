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
    ${CMAKE_CURRENT_SOURCE_DIR}/overlay/backends/dx7/imgui_impl_dx7.cpp
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
target_include_directories(imgui PRIVATE ${PROJECT_SOURCE_DIR}/sdk)
target_include_directories(imgui PUBLIC
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
    core/logging.cpp
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
    overlay/backends/backend_dx7.cpp
    overlay/backends/backend_dx8.cpp
    overlay/backends/backend_dx10.cpp
    overlay/backends/backend_dx11.cpp
    overlay/backends/backend_dx12.cpp
    overlay/backends/backend_dxgi.cpp
    overlay/backends/backend_opengl.cpp
    overlay/backends/backend_vulkan.cpp
    overlay/backends/backend_gdi.cpp
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
    dxguid
    opengl32
    bcrypt
    ws2_32
    winmm
    msimg32
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

if(STAR_BUILD_TESTS)
    add_executable(gdi_input_smoke ../tests/gdi_input_smoke.cpp)
    target_link_libraries(gdi_input_smoke PRIVATE user32 gdi32 minhook_lib)
    add_dependencies(gdi_input_smoke star)
    foreach(mode window strips letterbox single scaled)
        add_test(NAME gdi_input_${mode} COMMAND gdi_input_smoke
            $<TARGET_FILE:star> ${CMAKE_CURRENT_BINARY_DIR}/backend-tests ${mode})
        set_tests_properties(gdi_input_${mode} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(external_resize_smoke ../tests/external_resize_smoke.cpp)
    target_link_libraries(external_resize_smoke PRIVATE user32 minhook_lib)
    add_dependencies(external_resize_smoke star)
    add_test(NAME external_resize_smoke COMMAND external_resize_smoke
        $<TARGET_FILE:star> ${CMAKE_CURRENT_BINARY_DIR}/backend-tests)
    set_tests_properties(external_resize_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 RUN_SERIAL TRUE)
    add_executable(backend_selection_smoke ../tests/backend_selection_smoke.cpp)
    target_include_directories(backend_selection_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME backend_selection_smoke COMMAND backend_selection_smoke)

    add_executable(backend_ownership_smoke ../tests/backend_ownership_smoke.cpp ../tests/backend_ownership_dx7.cpp ../tests/backend_ownership_dx8.cpp)
    # No d3d8.lib/ddraw.lib on modern SDKs: the legacy TUs resolve both by hand.
    target_link_libraries(backend_ownership_smoke PRIVATE d3d11 d3d10 d3d12 d3d9 dxgi opengl32 gdi32 user32)
    target_include_directories(backend_ownership_smoke PRIVATE ${PROJECT_SOURCE_DIR}/sdk ${vulkan_headers_SOURCE_DIR}/include)
    add_dependencies(backend_ownership_smoke star)
    set(ownership_apis dx7 dx8 dx9 dx10 dx11 opengl gdi vulkan vulkan_proc vulkan_late dx11_vulkan)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND ownership_apis dx12 dx12_late)
    endif()
    foreach(api ${ownership_apis})
        add_test(NAME backend_ownership_${api} COMMAND backend_ownership_smoke
            $<TARGET_FILE:star> ${api} ${CMAKE_CURRENT_BINARY_DIR}/backend-tests)
        set_tests_properties(backend_ownership_${api} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    endforeach()

    add_executable(performance_smoke ../tests/performance_smoke.cpp)
    target_include_directories(performance_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${imgui_SOURCE_DIR})
    target_link_libraries(performance_smoke PRIVATE imgui)
    add_test(NAME performance_smoke COMMAND performance_smoke)

    add_executable(dx12_submission_smoke ../tests/dx12_submission_smoke.cpp)
    target_include_directories(dx12_submission_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(dx12_submission_smoke PRIVATE d3d12 dxgi)
    add_test(NAME dx12_submission_smoke COMMAND dx12_submission_smoke)
    set_tests_properties(dx12_submission_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

    add_executable(async_io_smoke ../tests/async_io_smoke.cpp
        core/logging.cpp core/storage.cpp core/callbacks.cpp steam/steam_remote_storage.cpp
        steam/steam_utils.cpp steam/steam_user_stats.cpp)
    target_sources(async_io_smoke PRIVATE overlay/capture/screenshot_service.cpp)
    target_include_directories(async_io_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/sdk)
    target_link_libraries(async_io_smoke PRIVATE nlohmann_json::nlohmann_json ole32 shell32 user32 winmm)
    target_compile_definitions(async_io_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_test(NAME async_io_smoke COMMAND async_io_smoke ${CMAKE_CURRENT_BINARY_DIR}/async-io-test)

    # Controller smoke: fake xinput1_4.dll shadows the system one (app dir is
    # searched first), so pads are fully scripted with no hardware attached.
    add_library(fake_xinput SHARED ../tests/fake_xinput.cpp ../tests/fake_xinput.def)
    set_target_properties(fake_xinput PROPERTIES OUTPUT_NAME "xinput1_4"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/controller-test")
    add_executable(controller_smoke ../tests/controller_smoke.cpp steam/steam_input.cpp)
    target_include_directories(controller_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/sdk)
    target_link_libraries(controller_smoke PRIVATE dinput8 dxguid user32)
    target_compile_definitions(controller_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_target_properties(controller_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/controller-test")
    add_dependencies(controller_smoke fake_xinput)
    add_test(NAME controller_smoke COMMAND controller_smoke)
    set_tests_properties(controller_smoke PROPERTIES TIMEOUT 60)

    foreach(check gdi_resources_smoke pixel_copy_smoke)
        add_executable(${check} ../tests/${check}.cpp)
        target_include_directories(${check} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${imgui_SOURCE_DIR})
        target_link_libraries(${check} PRIVATE imgui gdi32 user32)
        add_test(NAME ${check} COMMAND ${check})
    endforeach()

    add_executable(logging_smoke ../tests/logging_smoke.cpp core/logging.cpp)
    target_include_directories(logging_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME logging_smoke COMMAND logging_smoke)
endif()
