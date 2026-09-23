#include "overlay/overlay_internal.h"
#include "core/config.h"
#include "core/settings.h"
#include "core/storage.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "dx8/imgui_impl_dx8.h"
#include "dx7/imgui_impl_dx7.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d10.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

StarOverlay* g_overlay = nullptr;

namespace {
// Crash-pin sentinel: STAR/overlay.session holds the pid of the session that
// started hooks. A clean shutdown deletes it (only its owner may); a stale
// pid next launch means the hooks session died -> pin external for exactly
// one session, then re-evaluate.
std::string crash_pin_path()
{
    const std::string& dir = Settings::get().settings_dir;
    if (dir.empty()) return {};
    return dir + "\\overlay.session";
}

void write_crash_pin()
{
    std::string path = crash_pin_path();
    if (path.empty()) return;
    char own[32];
    snprintf(own, sizeof(own), "%lu", (unsigned long)GetCurrentProcessId());
    std::ofstream out(utf8_to_wstring(path), std::ios::trunc);
    if (out.is_open()) out << own << "\n";
}

bool pid_alive(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// True when a previous hooks session crashed: the sentinel is consumed
// (deleted) and the caller must start external this session. A pid that is
// still alive belongs to a concurrent sharer of STAR/ (e.g. launcher) and is
// re-stamped, never pinned.
bool consume_crash_pin()
{
    std::string path = crash_pin_path();
    if (path.empty()) return false;
    std::ifstream in(utf8_to_wstring(path));
    if (!in.is_open()) return false;
    std::string pid_str;
    std::getline(in, pid_str);
    in.close();
    char own[32];
    snprintf(own, sizeof(own), "%lu", (unsigned long)GetCurrentProcessId());
    if (pid_str == own) return false;
    DWORD pid = 0;
    try { pid = (DWORD)std::stoul(pid_str); } catch (...) { pid = 0; }
    if (pid != 0 && pid_alive(pid)) {
        write_crash_pin();
        return false;
    }
    DeleteFileW(utf8_to_wstring(path).c_str());
    return true;
}

void clear_crash_pin()
{
    std::string path = crash_pin_path();
    if (path.empty()) return;
    std::ifstream in(utf8_to_wstring(path));
    if (!in.is_open()) return;
    std::string pid_str;
    std::getline(in, pid_str);
    in.close();
    char own[32];
    snprintf(own, sizeof(own), "%lu", (unsigned long)GetCurrentProcessId());
    if (pid_str == own) DeleteFileW(utf8_to_wstring(path).c_str());
}
} // namespace

StarOverlay& StarOverlay::get()
{
    // Hooks retain this object for the process lifetime. Avoid static destruction
    // under the loader lock, where joining worker threads is unsafe.
    static StarOverlay* instance = new StarOverlay;
    return *instance;
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
    // "auto" (default) re-evaluates every launch: always starts on hooks and
    // falls back to external within the session if the title proves hostile
    // or tiny (see switch_to_external). Nothing persists the decision, except
    // the crash pin: a stale sentinel means the last hooks session died, so
    // play it safe for exactly one session.
    if (!enabled_) return;
    // One-time migration: pre-session-fallback builds persisted mode=external
    // together with nonzero fallback_count/fallback_level on auto-fallback.
    // An explicit user choice of external has no such keys and is respected.
    {
        IniFile ini;
        if (!Settings::get().settings_dir.empty() &&
            ini.load(Settings::get().settings_dir + "\\overlay.star")) {
            int fc = ini.get_int("", "fallback_count", 0);
            int fl = ini.get_int("", "fallback_level", 0);
            if (fc != 0 || fl != 0) {
                if (mode_ == OverlayMode::External) {
                    STAR_LOG("Migrating stale auto-fallback state to mode=auto "
                        "(was external with fallback_count=%d fallback_level=%d)", fc, fl);
                    mode_ = OverlayMode::Hook;
                    save_overlay_key("mode", "auto");
                }
                save_overlay_key("fallback_count", "0");
                save_overlay_key("fallback_level", "0");
            }
        }
    }
    if (mode_ == OverlayMode::Hook) {
        if (Settings::get().overlay_mode == "auto" && consume_crash_pin()) {
            mode_ = OverlayMode::External;
            STAR_LOG("Previous session crashed under hooks - starting external this session");
        } else {
            write_crash_pin();
        }
    }
    screenshots_.start();
    icon_decode_stop_ = false;
    icon_decode_thread_ = std::thread(&StarOverlay::icon_decode_worker, this);
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
        // Cursor/input hooks: while open_, the game is lied to about cursor
        // and keyboard state (Unity re-centers via SetCursorPos/SendInput, so
        // those warps are dropped too). Our own polling uses orig_* (truth).
        struct User32Hook { const char* name; void* detour; void** orig; };
        const User32Hook hooks[] = {
            {"ShowCursor", (void*)&hooked_ShowCursor, (void**)&orig_show_cursor_},
            {"ClipCursor", (void*)&hooked_ClipCursor, (void**)&orig_clip_cursor_},
            {"SetCursor", (void*)&hooked_SetCursor, (void**)&orig_set_cursor_},
            {"GetAsyncKeyState", (void*)&hooked_GetAsyncKeyState, (void**)&orig_get_async_key_},
            {"GetKeyboardState", (void*)&hooked_GetKeyboardState, (void**)&orig_get_keyboard_state_},
            {"GetKeyState", (void*)&hooked_GetKeyState, (void**)&orig_get_key_},
            {"GetCursorPos", (void*)&hooked_GetCursorPos, (void**)&orig_get_cursor_pos_},
            {"SetCursorPos", (void*)&hooked_SetCursorPos, (void**)&orig_set_cursor_pos_},
            {"SendInput", (void*)&hooked_SendInput, (void**)&orig_send_input_},
            {"mouse_event", (void*)&hooked_mouse_event, (void**)&orig_mouse_event_},
        };
        for (const auto& hook : hooks) {
            void* target = (void*)GetProcAddress(user32, hook.name);
            if (target && !*hook.orig && MH_CreateHook(target, hook.detour, hook.orig) == MH_OK) {
                MH_EnableHook(target);
                STAR_LOG("%s hooked", hook.name);
            }
        }
    }

    if (mode_ == OverlayMode::External) {
        // Detection hooks below are safe (proven: presents subclassing and
        // ECL interception never touch GPU state); only drawing is skipped.
        // User32 (cursor/input-blackout) hooks stay; they never touch
        // rendering and are proven safe.
        start_external_thread();
    }

    // Hook install is NOT backend use: hooks are pure passthrough detectors
    // until the first real present locks selection. Exactly one backend ever
    // initializes/draws (per-present single-backend guards + BackendSelection,
    // with GDI additionally requiring sustained drawing); the rest stay
    // passthrough and the retry thread stops once locked.
    hook_dx9();
    hook_dx8();
    hook_dx7();
    hook_dxgi();
    hook_opengl();
    hook_vulkan();
    hook_gdi();
    hooks_installed_ = true;

    // Retry late-loaded gfx modules (game loads d3d12/vulkan/opengl AFTER SteamAPI_Init).
    retry_stop_ = false;
    retry_thread_ = std::thread([this]() {
        std::unique_lock<std::mutex> lock(retry_mutex_);
        while (!retry_stop_.load()) {
            retry_cv_.wait_for(lock, std::chrono::seconds(1));
            if (retry_stop_.load() || !g_overlay) break;
            if (game_api_ != GraphicsAPI::None) break;
            lock.unlock();
            ensure_hooks();
            lock.lock();
        }
    });
}

bool StarOverlay::accept_backend(GraphicsAPI api, HWND window)
{
    if (!enabled_ || !window || window == ext_hwnd_ || !IsWindowVisible(window)) return false;
    if (game_api_ != GraphicsAPI::None && game_api_ != api) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    RECT client{};
    if (pid != GetCurrentProcessId() || !GetClientRect(window, &client) ||
        client.right < 50 || client.bottom < 50) return false;
    if (game_window_ && IsWindow(game_window_) && IsWindowVisible(game_window_) &&
        window != game_window_) return false;
    if (window != game_window_ && GetAncestor(window, GA_ROOT) != find_game_window()) return false;
    GraphicsAPI previous = game_api_;
    if (!game_api_.accept(api, (uintptr_t)window, GetTickCount64())) return false;
    game_window_ = window;
    if (previous == GraphicsAPI::None) {
        const char* names[] = {"None", "DirectX 7", "DirectX 8", "DirectX 9", "DirectX 10",
            "DirectX 11", "DirectX 12", "OpenGL", "Vulkan", "GDI"};
        STAR_LOG("Game graphics API locked: %s hwnd=%p", names[(int)api], window);
    }
    return true;
}

// Renderer resources can be rebuilt; the game's API selection cannot change.
// All callers hold render_mutex_.
void StarOverlay::shutdown_renderer()
{
#ifdef _WIN64
    if (active_api_ == GraphicsAPI::DX12 && !wait_dx12_idle()) retire_dx12_renderer();
#endif
    if (active_api_ == GraphicsAPI::Vulkan) {
        cleanup_vulkan();
    } else {
#ifdef _WIN64
        if (active_api_ == GraphicsAPI::DX12) icons_.clear();
        else
#endif
        icons_.release_all([this](ImTextureID texture) { release_icon(texture); });
        if (imgui_initialized_) {
            switch (active_api_) {
            case GraphicsAPI::DX7: ImGui_ImplDX7_Shutdown(); break;
            case GraphicsAPI::DX8: ImGui_ImplDX8_Shutdown(); break;
            case GraphicsAPI::DX9: ImGui_ImplDX9_Shutdown(); break;
            case GraphicsAPI::DX10: ImGui_ImplDX10_Shutdown(); break;
            case GraphicsAPI::DX11:
            case GraphicsAPI::GDI: ImGui_ImplDX11_Shutdown(); break;
#ifdef _WIN64
            case GraphicsAPI::DX12: ImGui_ImplDX12_Shutdown(); break;
#endif
            case GraphicsAPI::OpenGL: shutdown_opengl(); break;
            default: break;
            }
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    }
    imgui_initialized_ = false;
    active_api_ = GraphicsAPI::None;
    cleanup_rtv();
    cleanup_dx10_rtv();
#ifdef _WIN64
    cleanup_dx12();
#endif
    gdi_.reset();
    icons_.clear(); screenshot_icons_.clear(); viewer_icon_.clear();
    external_draw_snapshot_.clear();
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (dx10_device_) { dx10_device_->Release(); dx10_device_ = nullptr; }
    dx7_device_ = nullptr;
    dx8_device_ = nullptr;
    dx9_device_ = nullptr;
    dxgi_chain_ = nullptr;
    gl_context_ = nullptr;
    gl_dc_ = nullptr;
}

void StarOverlay::start_external_thread()
{
    if (ext_thread_) return;
    ext_stop_ = false;
    ext_thread_ = CreateThread(nullptr, 0, &external_thread_entry, this, 0, nullptr);
    if (!ext_thread_) STAR_LOG("External overlay thread failed");
    else STAR_LOG("External overlay started");
}

void StarOverlay::switch_to_external(const char* reason)
{
    // Hook rendering proved hostile or the frame proved tiny: fall back to
    // external for the rest of this session only. Explicit "hook" mode never
    // auto-falls back. Nothing is persisted, so the next launch re-evaluates.
    if (mode_ == OverlayMode::External) return;
    if (Settings::get().overlay_mode == "hook") {
        STAR_LOG("Hook mode hostile (%s) - staying on hooks (mode=hook is explicit)", reason);
        return;
    }
    mode_ = OverlayMode::External;
    STAR_LOG("Switching to external overlay (%s) for this session", reason);
    start_external_thread();
}

bool StarOverlay::migrate_if_tiny_frame(int w, int h)
{
    // Pre-checked so a game that stays tiny (or explicit mode=hook) never
    // spams the switch log once per present.
    if (mode_ == OverlayMode::External || Settings::get().overlay_mode == "hook") return false;
    if (w >= kTinyFrameW && h >= kTinyFrameH) return false;
    if (w <= 0 || h <= 0) return false;
    char why[64];
    snprintf(why, sizeof(why), "tiny frame %dx%d", w, h);
    switch_to_external(why);
    return mode_ == OverlayMode::External;
}

bool StarOverlay::migrate_if_tiny_frame(HWND window)
{
    if (!window) return false;
    RECT rc{};
    if (!GetClientRect(window, &rc)) return false;
    return migrate_if_tiny_frame(rc.right - rc.left, rc.bottom - rc.top);
}

void StarOverlay::ensure_hooks()
{
    if (!enabled_ || !g_overlay) return;
    // Each hook fn is idempotent, so retrying pre-lock is safe.
    if (!dxgi_hooked_) hook_dxgi();
    if (!dx9_hooked_) hook_dx9();
    if (!dx8_hooked_) hook_dx8();
    if (!dx7_hooked_) hook_dx7();
    if (!opengl_hooked_) hook_opengl();
    if (!vulkan_hooked_) hook_vulkan();
    if (!gdi_hooked_) hook_gdi();
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
    enabled_ = false;
    if (gdi_message_hook_) {
        UnhookWindowsHookEx(gdi_message_hook_);
        gdi_message_hook_ = nullptr;
        gdi_message_thread_ = 0;
    }
    retry_stop_ = true;
    retry_cv_.notify_all();
    if (retry_thread_.joinable()) retry_thread_.join();
    screenshots_.stop();
    if (icon_decode_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_stop_ = true;
        }
        icon_decode_cv_.notify_all();
        icon_decode_thread_.join();
        std::lock_guard<std::mutex> lock(icon_decode_mutex_);
        icon_decode_queue_.clear(); icon_decode_ready_.clear();
        icon_decode_pending_.clear();
    }
    if (mode_ == OverlayMode::External && ext_thread_) {
        ext_stop_ = true;
        WaitForSingleObject(ext_thread_, 5000);
        CloseHandle(ext_thread_);
        ext_thread_ = nullptr;
    }
    // Persist playtime even on early shutdown paths.
    Storage::get().save_playtime(total_playtime_sec());
    std::lock_guard<std::mutex> lock(render_mutex_);

    while (cursor_show_count_offset_ > 0) {
        if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
        cursor_show_count_offset_--;
    }

    if (hwnd_ && wnd_proc_orig_) {
        swap_wndproc(hwnd_, wnd_proc_orig_);
        wnd_proc_orig_ = nullptr;
    }

    shutdown_renderer();
    cleanup_vulkan();
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
    // Reaching here means a clean exit: a crash or kill never gets this far,
    // so its sentinel survives and pins the next launch.
    clear_crash_pin();
}

