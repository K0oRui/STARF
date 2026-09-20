#include "overlay/overlay_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include <MinHook.h>
#include <GL/gl.h>

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
            if (!api_detected_) { api_detected_ = true; STAR_LOG("OpenGL hooked"); }
        }
    }
}

void StarOverlay::on_present_opengl(HDC hdc)
{
    if (!enabled_ || !wglGetCurrentContext() || wglGetCurrentDC() != hdc) return;
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
        if (!ImGui_ImplWin32_Init(hwnd_)) { ImGui::DestroyContext(); return; }

        if (ImGui_ImplOpenGL3_Init()) {
            style_.setup();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::OpenGL;
            STAR_LOG("ImGui ready (OpenGL) hwnd=%p", hwnd_);
        } else {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            STAR_LOG("ImGui OpenGL init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::OpenGL) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        build_frame_ui();

        // The ImGui backend preserves GL state, including in core profiles.
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        maybe_capture_opengl();
    }
}
