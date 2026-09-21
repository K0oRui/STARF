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
    if (orig_wglSwapBuffers_ && orig_wglDeleteContext_) { opengl_hooked_ = true; return; }
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) opengl_dll = LoadLibraryA("opengl32.dll");
    if (!opengl_dll) return;

    void* pSwapBuffers = (void*)GetProcAddress(opengl_dll, "wglSwapBuffers");
    if (pSwapBuffers && !orig_wglSwapBuffers_) {
        MH_STATUS s = MH_CreateHook(pSwapBuffers, &hooked_wglSwapBuffers, (void**)&orig_wglSwapBuffers_);
        if (s == MH_OK) {
            MH_EnableHook(pSwapBuffers);
            opengl_hooked_ = true;
            if (!any_graphics_hook_installed_) { any_graphics_hook_installed_ = true; STAR_LOG("OpenGL hooked"); }
        }
    }
    void* pDeleteContext = (void*)GetProcAddress(opengl_dll, "wglDeleteContext");
    if (pDeleteContext && !orig_wglDeleteContext_ &&
        MH_CreateHook(pDeleteContext, &hooked_wglDeleteContext, (void**)&orig_wglDeleteContext_) == MH_OK)
        MH_EnableHook(pDeleteContext);
}

BOOL WINAPI StarOverlay::hooked_wglDeleteContext(HGLRC context)
{
    auto* o = g_overlay;
    if (!o || !o->orig_wglDeleteContext_) return FALSE;
    if (o->game_api_ != GraphicsAPI::OpenGL) return o->orig_wglDeleteContext_(context);
    {
        std::lock_guard<std::mutex> lock(o->render_mutex_);
        if (o->active_api_ == GraphicsAPI::OpenGL && o->gl_context_ == context)
            o->shutdown_renderer();
    }
    return o->orig_wglDeleteContext_(context);
}

void StarOverlay::shutdown_opengl()
{
    HGLRC previous = wglGetCurrentContext();
    HDC previous_dc = wglGetCurrentDC();
    if (previous == gl_context_ || wglMakeCurrent(gl_dc_, gl_context_)) {
        icons_.release_all([this](ImTextureID texture) { release_icon(texture); });
        ImGui_ImplOpenGL3_Shutdown();
        if (previous != gl_context_) wglMakeCurrent(previous_dc, previous);
    } else {
        // The game already discarded the DC/context. Never delete GL names in
        // a different context. The pinned ImGui GL backend's data is POD;
        // release its CPU allocation, leaving GL objects to their context owner.
        icons_.clear();
        ImGuiIO& io = ImGui::GetIO();
        ImGui::MemFree(io.BackendRendererUserData);
        io.BackendRendererUserData = nullptr;
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    }
}

void StarOverlay::on_present_opengl(HDC hdc)
{
    if (!enabled_ || !wglGetCurrentContext() || wglGetCurrentDC() != hdc) return;
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !accept_backend(GraphicsAPI::OpenGL, WindowFromDC(hdc))) return;
    if (gl_context_ && gl_context_ != wglGetCurrentContext()) return;
    note_present();
    if (mode_ == OverlayMode::External) return;
    if (imgui_initialized_ && active_api_ != GraphicsAPI::OpenGL) return;
    hook_window_for(WindowFromDC(hdc));
    poll_hotkey();

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
            gl_context_ = wglGetCurrentContext();
            gl_dc_ = hdc;
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
