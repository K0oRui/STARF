#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "core/storage.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d10.h>
#include <algorithm>

StarOverlay* g_overlay = nullptr;

StarOverlay& StarOverlay::get()
{
    static StarOverlay instance;
    return instance;
}

Overlay& Overlay::get()
{
    return StarOverlay::get();
}

void StarOverlay::init()
{
    g_overlay = this;
    enabled_  = Settings::get().overlay_enabled;
    focus_last_tick_ = GetTickCount();
    last_playtime_save_ = GetTickCount();
    Storage::get().load_playtime(base_playtime_sec_);
    notes_.load();
    mode_ = (Settings::get().overlay_mode == "external") ? OverlayMode::External : OverlayMode::Hook;
    // "auto" (default) starts on hooks and falls back to external if the
    // title proves hostile (see switch_to_external).
    if (!enabled_) return;
    icon_decode_stop_ = false;
    icon_decode_thread_ = std::thread(&StarOverlay::icon_decode_worker, this);
    fallback_count_ = Settings::get().overlay_fallback_count;
    fallback_level_ = Settings::get().overlay_fallback_level;
    retry_session_ = false;
    fell_back_this_session_ = false;
    if (mode_ == OverlayMode::External && fallback_count_ > 0) {
        // A previous session fell back to external. Skip this one too, then
        // retry hooks once the skip window runs out (exponential backoff).
        // The persisted mode stays "external" until a retry session either
        // ends cleanly (shutdown clears it) or falls back again (which grows
        // the window), so a crash mid-retry just skips again next launch.
        if (--fallback_count_ == 0) {
            mode_ = OverlayMode::Hook;
            retry_session_ = true;
            STAR_LOG("Overlay backoff done - retrying hook mode this session");
        } else {
            save_overlay_key("fallback_count", std::to_string(fallback_count_));
            STAR_LOG("Overlay staying external (backoff %d sessions left)", fallback_count_);
        }
    }
    if (hooks_installed_) return;
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) STAR_LOG("MinHook init failed status=%d", (int)mh_init);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "STAR_Dummy";
    RegisterClassExA(&wc);

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (user32) {
        void* pShowCursor = (void*)GetProcAddress(user32, "ShowCursor");
        if (!orig_show_cursor_ && MH_CreateHook(pShowCursor, &hooked_ShowCursor, (void**)&orig_show_cursor_) == MH_OK) {
            MH_EnableHook(pShowCursor);
            STAR_LOG("ShowCursor hooked");
        }
        void* pClipCursor = (void*)GetProcAddress(user32, "ClipCursor");
        if (!orig_clip_cursor_ && MH_CreateHook(pClipCursor, &hooked_ClipCursor, (void**)&orig_clip_cursor_) == MH_OK) {
            MH_EnableHook(pClipCursor);
            STAR_LOG("ClipCursor hooked");
        }
        void* pSetCursor = (void*)GetProcAddress(user32, "SetCursor");
        if (!orig_set_cursor_ && MH_CreateHook(pSetCursor, &hooked_SetCursor, (void**)&orig_set_cursor_) == MH_OK) {
            MH_EnableHook(pSetCursor);
            STAR_LOG("SetCursor hooked");
        }

        // Input blackout hooks: lie to game-side polling while open_.
        // Our own polling below always goes through orig_* (real state).
        void* pAsyncKey = (void*)GetProcAddress(user32, "GetAsyncKeyState");
        if (pAsyncKey && !orig_get_async_key_ &&
            MH_CreateHook(pAsyncKey, &hooked_GetAsyncKeyState, (void**)&orig_get_async_key_) == MH_OK) {
            MH_EnableHook(pAsyncKey);
            STAR_LOG("GetAsyncKeyState hooked");
        }
        void* pKbState = (void*)GetProcAddress(user32, "GetKeyboardState");
        if (pKbState && !orig_get_keyboard_state_ &&
            MH_CreateHook(pKbState, &hooked_GetKeyboardState, (void**)&orig_get_keyboard_state_) == MH_OK) {
            MH_EnableHook(pKbState);
            STAR_LOG("GetKeyboardState hooked");
        }
        void* pKeyState = (void*)GetProcAddress(user32, "GetKeyState");
        if (pKeyState && !orig_get_key_ &&
            MH_CreateHook(pKeyState, &hooked_GetKeyState, (void**)&orig_get_key_) == MH_OK) {
            MH_EnableHook(pKeyState);
            STAR_LOG("GetKeyState hooked");
        }
        void* pCursorPos = (void*)GetProcAddress(user32, "GetCursorPos");
        if (pCursorPos && !orig_get_cursor_pos_ &&
            MH_CreateHook(pCursorPos, &hooked_GetCursorPos, (void**)&orig_get_cursor_pos_) == MH_OK) {
            MH_EnableHook(pCursorPos);
            STAR_LOG("GetCursorPos hooked");
        }
        // Unity-style cursor lock warps to center every frame via SetCursorPos.
        // Drop those warps while open so the panel mouse stays free; physical
        // mouse movement never goes through here so ImGui still tracks.
        void* pSetCursorPos = (void*)GetProcAddress(user32, "SetCursorPos");
        if (pSetCursorPos && !orig_set_cursor_pos_ &&
            MH_CreateHook(pSetCursorPos, &hooked_SetCursorPos, (void**)&orig_set_cursor_pos_) == MH_OK) {
            MH_EnableHook(pSetCursorPos);
            STAR_LOG("SetCursorPos hooked");
        }
        // Same warps can arrive as injected input (Unity centers via
        // SendInput/mouse_event on some versions). Drop injected mouse
        // motion while open; physical mouse never travels this path.
        void* pSendInput = (void*)GetProcAddress(user32, "SendInput");
        if (pSendInput && !orig_send_input_ &&
            MH_CreateHook(pSendInput, &hooked_SendInput, (void**)&orig_send_input_) == MH_OK) {
            MH_EnableHook(pSendInput);
            STAR_LOG("SendInput hooked");
        }
        void* pMouseEvent = (void*)GetProcAddress(user32, "mouse_event");
        if (pMouseEvent && !orig_mouse_event_ &&
            MH_CreateHook(pMouseEvent, &hooked_mouse_event, (void**)&orig_mouse_event_) == MH_OK) {
            MH_EnableHook(pMouseEvent);
            STAR_LOG("mouse_event hooked");
        }
    }

    if (mode_ == OverlayMode::External) {
        // Detection hooks below are safe (proven: presents subclassing and
        // ECL interception never touch GPU state); only drawing is skipped.
        // User32 (cursor/input-blackout) hooks stay; they never touch
        // rendering and are proven safe.
        hooks_installed_ = true;
        start_external_thread();
    }

    hook_dx9();
hook_dxgi();
    hook_opengl();
    hook_vulkan();
    hooks_installed_ = true;

    // Retry late-loaded gfx modules (game loads d3d12/vulkan/opengl AFTER SteamAPI_Init).
    retry_stop_ = false;
    std::thread([this]() {
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (retry_stop_.load() || !g_overlay) break;
            ensure_hooks();
            if (dxgi_hooked_ && dx9_hooked_ && opengl_hooked_ && vulkan_hooked_) break;
            if (imgui_initialized_) break;
        }
    }).detach();
}

void StarOverlay::start_external_thread()
{
    if (ext_thread_) return;
    ext_stop_ = false;
    ext_thread_ = CreateThread(nullptr, 0, &external_thread_entry, this, 0, nullptr);
    if (!ext_thread_) STAR_LOG("External overlay thread failed");
    else STAR_LOG("External overlay started");
}

void StarOverlay::switch_to_external(const char* reason, bool dx12_hostile)
{
    // Hook rendering proved hostile (GPU fault / endless fence timeouts).
    // Remember "external" itself (not just render-off) so the next launch
    // goes straight to the working UI. API emulation + input hooks stay.
    // The fallback is not permanent: an exponential backoff (1,3,7,15,31,63
    // skipped sessions) retries hook mode on later launches, so a transient
    // GPU hiccup heals itself. Explicit "hook" mode never auto-falls back.
    if (mode_ == OverlayMode::External) return;
    if (Settings::get().overlay_mode == "hook") {
        STAR_LOG("Hook mode hostile (%s) - staying on hooks (mode=hook is explicit)", reason);
        return;
    }
    mode_ = OverlayMode::External;
    fell_back_this_session_ = true;
    fallback_level_ = std::min(fallback_level_ + 1, 6);
    fallback_count_ = (1 << fallback_level_) - 1;
    save_overlay_key("mode", "external");
    save_overlay_key("fallback_count", std::to_string(fallback_count_));
    save_overlay_key("fallback_level", std::to_string(fallback_level_));
    if (dx12_hostile) {
        // The title faults on DX12 in-backbuffer drawing: keep drawing off
        // so the retry (and any manual hook mode) stays safe.
        save_overlay_key("dx12_render", "false");
    }
    STAR_LOG("Switching to external overlay (%s) - backoff level %d (%d sessions)",
        reason, fallback_level_, fallback_count_);
    start_external_thread();
}

void StarOverlay::ensure_hooks()
{
    if (!enabled_ || !g_overlay) return;
    // Each hook fn is now idempotent (checks orig_* / hooked flag), so safe to retry.
    if (!dxgi_hooked_) hook_dxgi();
    if (!dx9_hooked_) hook_dx9();
    if (!opengl_hooked_) hook_opengl();
    if (!vulkan_hooked_) hook_vulkan();
#ifdef _WIN64
    hook_dx12_ecl();
#endif
}

uint64_t StarOverlay::total_playtime_sec() const
{
    if (focused_sec_ < 0) focused_sec_ = 0;
    return base_playtime_sec_ + (uint64_t)focused_sec_;
}

void StarOverlay::note_present()
{
    DWORD now = GetTickCount();
    if (present_fps_tick_ == 0) present_fps_tick_ = now;
    present_frames_++;
    if (now - present_fps_tick_ >= 500) {
        present_fps_ = present_frames_ * 1000.f / (float)(now - present_fps_tick_);
        present_frames_ = 0;
        present_fps_tick_ = now;
    }
}

void StarOverlay::poll_focus()
{    DWORD now = GetTickCount();
    if (focus_last_tick_ == 0) focus_last_tick_ = now;
    // Same foreground rule as keyboard input: game window or our own overlay
    // window counts as "playing"; anything else pauses the clock.
    HWND fg = GetForegroundWindow();
    HWND ref = hwnd_ ? hwnd_ : ext_game_hwnd_;
    if (fg && (!ref || fg == ref || fg == ext_hwnd_))
        focused_sec_ += (now - focus_last_tick_) / 1000.0;
    focus_last_tick_ = now;
}

std::string StarOverlay::format_playtime(uint64_t secs)
{
    char buf[32];
    if (secs >= 3600) snprintf(buf, sizeof(buf), "%lluh %02llum",
        (unsigned long long)(secs / 3600), (unsigned long long)((secs / 60) % 60));
    else if (secs >= 60) snprintf(buf, sizeof(buf), "%llum",
        (unsigned long long)(secs / 60));
    else snprintf(buf, sizeof(buf), "%llus", (unsigned long long)secs);
    return buf;
}

void StarOverlay::shutdown()
{
    // A retry session that survived without falling back again means the
    // hostile title healed: clear the backoff so future launches use hooks.
    if (retry_session_ && !fell_back_this_session_) {
        save_overlay_key("mode", "auto");
        save_overlay_key("fallback_count", "0");
        save_overlay_key("fallback_level", "0");
        STAR_LOG("Overlay retry session clean - backoff cleared");
    }
    if (icon_decode_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_stop_ = true;
        }
        icon_decode_cv_.notify_all();
        icon_decode_thread_.join();
    }
    if (mode_ == OverlayMode::External && ext_thread_) {
        ext_stop_ = true;
        WaitForSingleObject(ext_thread_, 5000);
        CloseHandle(ext_thread_);
        ext_thread_ = nullptr;
    }
    // Persist playtime even on early shutdown paths.
    Storage::get().save_playtime(total_playtime_sec());
    bool was_enabled = enabled_;
    std::lock_guard<std::mutex> lock(render_mutex_);
    GraphicsAPI api_snapshot = active_api_;

    while (cursor_show_count_offset_ > 0) {
        if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
        cursor_show_count_offset_--;
    }

    if (hwnd_ && wnd_proc_orig_) {
        if (IsWindowUnicode(hwnd_)) {
            SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
        } else {
            SetWindowLongPtrA(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
        }
        wnd_proc_orig_ = nullptr;
    }

    if (imgui_initialized_) {
        if (active_api_ == GraphicsAPI::DX11) {
            ImGui_ImplDX11_Shutdown();
        } else if (active_api_ == GraphicsAPI::DX10) {
            ImGui_ImplDX10_Shutdown();
#ifdef _WIN64
        } else if (active_api_ == GraphicsAPI::DX12) {
            ImGui_ImplDX12_Shutdown();
#endif
        } else if (active_api_ == GraphicsAPI::DX9) {
            ImGui_ImplDX9_Shutdown();
        } else if (active_api_ == GraphicsAPI::OpenGL) {
            ImGui_ImplOpenGL3_Shutdown();
        } else if (active_api_ == GraphicsAPI::Vulkan) {
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
        active_api_ = GraphicsAPI::None;
    }

    cleanup_rtv();
    cleanup_dx10_rtv();
#ifdef _WIN64
    cleanup_dx12();
#endif
    cleanup_vulkan();

    if (api_snapshot == GraphicsAPI::DX11) {
        icons_.release_all([](ImTextureID v) { ((ID3D11ShaderResourceView*)v)->Release(); });
    } else if (api_snapshot == GraphicsAPI::DX10) {
        icons_.release_all([](ImTextureID v) { ((ID3D10ShaderResourceView*)v)->Release(); });
    } else if (api_snapshot == GraphicsAPI::DX9) {
        // MANAGED-pool IDirect3DTexture9* icons.
        icons_.release_all([](ImTextureID v) { ((IDirect3DTexture9*)v)->Release(); });
        dx9_device_ = nullptr;
    }
    // OpenGL icon textures belong to the game's GL context, which may be gone
    // at shutdown; the OS/driver reclaims them with the context.
    icons_.clear_gl();
    icons_.clear();
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_)  { device_->Release();  device_  = nullptr; }
    if (dx10_device_) { dx10_device_->Release(); dx10_device_ = nullptr; }
    hooks_installed_ = false;
    // NOTE: do NOT MH_DisableHook(MH_ALL_HOOKS)/Remove/Uninitialize here.
    // MinHook is shared with integrity hooks (STAR_install_integrity_hooks).
    // Disabling ALL hooks kills CreateFile/GetFileAttributes redirects and
    // double-uninitializes MinHook (DllMain DETACH calls uninstall after this).
    // Leave gfx hooks installed; they passthrough via orig_* when disabled.
    // Keep g_overlay alive for passthrough (hooked Present needs orig pointers).
    retry_stop_ = true;
    enabled_ = false;
    open_ = false;
    if (!was_enabled) return;
}

