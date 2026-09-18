#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/callbacks.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d12.h>
#include <cmath>
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <vulkan/vulkan.h>
#include <cctype>
#include <ctime>
#include <algorithm>

BOOL WINAPI StarOverlay::hooked_wglSwapBuffers(HDC hdc)
{
    if (g_overlay) g_overlay->on_present_opengl(hdc);
    return g_overlay ? g_overlay->orig_wglSwapBuffers_(hdc) : FALSE;
}

void StarOverlay::hook_opengl()
{
    if (orig_wglSwapBuffers_) { opengl_hooked_ = true; return; }
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) opengl_dll = LoadLibraryA("opengl32.dll");
    if (!opengl_dll) return;

    void* pSwapBuffers = (void*)GetProcAddress(opengl_dll, "wglSwapBuffers");
    if (pSwapBuffers) {
        MH_STATUS s = MH_CreateHook(pSwapBuffers, &hooked_wglSwapBuffers, (void**)&orig_wglSwapBuffers_);
        if (s == MH_OK) {
            MH_EnableHook(pSwapBuffers);
            opengl_hooked_ = true;
            STAR_LOG("OpenGL hooked");
        } else {
            STAR_LOG("OpenGL hook: MH=%d", (int)s);
        }
    }
}

void StarOverlay::on_present_opengl(HDC hdc)
{
    if (!enabled_) return;
    poll_hotkey();
    note_present();
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::OpenGL;
        STAR_LOG("Game graphics API: OpenGL");
    }
    if (mode_ == OverlayMode::External) return;
    hook_window_for(WindowFromDC(hdc));

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    if (!imgui_initialized_) {
        HWND new_hwnd = WindowFromDC(hdc);
        if (!new_hwnd) new_hwnd = GetActiveWindow();
        hook_window_for(new_hwnd);

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(hwnd_);

        if (ImGui_ImplOpenGL3_Init()) {
            style_.setup();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::OpenGL;
            STAR_LOG("ImGui ready (OpenGL) hwnd=%p", hwnd_);
        } else {
            STAR_LOG("ImGui OpenGL init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::OpenGL) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        build_frame_ui();

        #ifndef GL_ALL_ATTRIB_BITS
        #define GL_ALL_ATTRIB_BITS 0x000fffff
        #define GL_CLIENT_ALL_ATTRIB_BITS 0xffffffff
        #endif
        typedef void(WINAPI* glPushAttribFn)(uint32_t);
        typedef void(WINAPI* glPopAttribFn)();
        typedef void(WINAPI* glPushClientAttribFn)(uint32_t);
        typedef void(WINAPI* glPopClientAttribFn)();

        HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
        auto glPushAttrib = (glPushAttribFn)GetProcAddress(opengl_dll, "glPushAttrib");
        auto glPopAttrib = (glPopAttribFn)GetProcAddress(opengl_dll, "glPopAttrib");
        auto glPushClientAttrib = (glPushClientAttribFn)GetProcAddress(opengl_dll, "glPushClientAttrib");
        auto glPopClientAttrib = (glPopClientAttribFn)GetProcAddress(opengl_dll, "glPopClientAttrib");

        if (glPushAttrib) glPushAttrib(GL_ALL_ATTRIB_BITS);
        if (glPushClientAttrib) glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (glPopClientAttrib) glPopClientAttrib();
        if (glPopAttrib) glPopAttrib();

        maybe_capture_opengl();
    }
}
