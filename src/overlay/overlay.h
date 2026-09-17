#pragma once
#include "core/star_common.h"
#include <atomic>
#include <thread>
#include <dxgi1_4.h>
#include "imgui.h"

struct AchievementNotification {
    std::string header = "ACHIEVEMENT UNLOCKED";
    bool summary = false; // all-complete celebration: gold styling + star
    std::string title;
    std::string description;
    std::vector<uint8_t> icon_rgba;
    int   icon_width  = 0;
    int   icon_height = 0;
    float time_remaining = 5.0f;
    float age = 0.0f;
};

class StarOverlay {
public:
    static StarOverlay& get();

    void init();
    void shutdown();

    void push_achievement(const std::string& name, const std::string& desc,
                           const std::vector<uint8_t>& icon_rgba, int iw, int ih,
                           const std::string& header = "ACHIEVEMENT UNLOCKED",
                           bool summary = false);

    void request_screenshot();

    void note_session_unlock() { session_unlocks_++; }
    void note_session_revoke() { if (session_unlocks_ > 0) session_unlocks_--; }

    bool is_enabled() const { return enabled_; }
    bool is_open()    const { return open_; }
    // Open-only (game API bridge): never toggles an open panel shut.
    void open_panel();

    enum class OverlayMode { Hook, External };
    OverlayMode mode_ = OverlayMode::Hook;

    // ---- external window mode (no game hooks; own transparent window) ----
    HWND ext_hwnd_ = nullptr;
    HWND ext_game_hwnd_ = nullptr;
    // Layered-window compositing (no swapchain): game frame rendered to an
    // RT texture, copied to staging, blitted via UpdateLayeredWindow.
    ID3D11Texture2D* ext_rt_tex_ = nullptr;
    ID3D11Texture2D* ext_stage_tex_ = nullptr;
    HDC ext_dib_dc_ = nullptr;
    HBITMAP ext_dib_bmp_ = nullptr;
    void* ext_dib_bits_ = nullptr;
    // Previous uploaded frame: skip UpdateLayeredWindow when pixels are
    // identical (static HUD = no DWM recomposite = no flicker).
    std::vector<uint8_t> ext_prev_;
    HANDLE ext_thread_ = nullptr;
    std::atomic<bool> ext_stop_{ false };
    int ext_track_tick_ = 0;
    int ext_w_ = 0, ext_h_ = 0;
    bool ext_visible_ = false;
    bool fg_ok_ = true;
    // Decoupled panel cursor (external mode): the game owns the OS cursor
    // (locked/hidden/warped); the panel drives a virtual one from motion
    // deltas so warps can never trap it. Screen coords.
    float ext_cur_x_ = 0, ext_cur_y_ = 0;
    POINT ext_last_real_ = {};
    bool ext_cur_init_ = false;
    int ext_show_saved_ = 0;
    void external_cursor_open();
    void external_cursor_close();
    void external_cursor_frame();
    // Direct cursor calls (bypass our own hooks): probe is net-zero.
    int show_cursor(BOOL b) { return orig_show_cursor_ ? orig_show_cursor_(b) : ShowCursor(b); }
    void read_cursor_pos(POINT& p) { if (orig_get_cursor_pos_) orig_get_cursor_pos_(&p); else GetCursorPos(&p); }
    int probe_cursor_count() { int c = show_cursor(TRUE); show_cursor(FALSE); return c - 1; }
    static DWORD WINAPI external_thread_entry(LPVOID self);
    void external_thread_proc();
    bool external_create_window(int x, int y, int w, int h);
    bool external_create_device();
    void external_free_surfaces();
    bool external_alloc_surfaces(int w, int h);
    void external_track_game_window();
    static HWND find_game_window();
    void external_render_frame();
    static LRESULT CALLBACK ext_wnd_proc(HWND, UINT, WPARAM, LPARAM);

private:
    StarOverlay() = default;

    enum class GraphicsAPI { None, DX9, DX11, DX12, OpenGL, Vulkan };
    GraphicsAPI active_api_ = GraphicsAPI::None;
    GraphicsAPI game_api_ = GraphicsAPI::None; // what the GAME renders with

    void hook_dx11();
    void on_present(IDXGISwapChain* chain, UINT sync_interval, UINT flags);
    void on_resize_buffers(IDXGISwapChain* chain, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl);
    void render_frame(IDXGISwapChain* chain);
    void init_imgui(IDXGISwapChain* chain);
    void cleanup_rtv();
    ImTextureID get_or_create_icon(const std::string& key,
                                   const std::vector<uint8_t>& rgba, int w, int h);

#ifdef _WIN64
    void on_present_dx12(IDXGISwapChain* chain);
    void init_imgui_dx12(IDXGISwapChain* chain, void* device, void* command_queue);
    void render_frame_dx12(IDXGISwapChain* chain);
    void cleanup_dx12();
    void hook_dx12_ecl();
    ImTextureID upload_icon_dx12(const std::vector<uint8_t>& rgba, int w, int h);
#endif

    using DX9PresentFn = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3DDevice9*, const RECT*, const RECT*, HWND, const struct RGNDATA*);
    using DX9ResetFn   = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3DDevice9*, void*);
    DX9PresentFn orig_dx9_present_ = nullptr;
    DX9ResetFn   orig_dx9_reset_   = nullptr;
    struct IDirect3DDevice9* dx9_device_ = nullptr;
    static HRESULT STDMETHODCALLTYPE hooked_DX9Present(struct IDirect3DDevice9*, const RECT*, const RECT*, HWND, const struct RGNDATA*);
    static HRESULT STDMETHODCALLTYPE hooked_DX9Reset(struct IDirect3DDevice9*, void*);
    void hook_dx9();
    void on_present_dx9(struct IDirect3DDevice9* device);
    void on_reset_dx9();
    ImTextureID upload_icon_dx9(const std::vector<uint8_t>& rgba, int w, int h);
    ImTextureID upload_icon_opengl(const std::vector<uint8_t>& rgba, int w, int h);

    using wglSwapBuffersFn = BOOL(WINAPI*)(HDC);
    wglSwapBuffersFn orig_wglSwapBuffers_ = nullptr;
    std::vector<unsigned int> gl_icon_textures_;
    static BOOL WINAPI hooked_wglSwapBuffers(HDC);
    void hook_opengl();
    void on_present_opengl(HDC hdc);

    void* vk_instance_ = nullptr;
    void* vk_physical_device_ = nullptr;
    void* vk_device_ = nullptr;
    void* vk_queue_ = nullptr;
    uint32_t vk_queue_family_ = 0;
    int vk_swapchain_format_ = 0;
    uint32_t vk_min_image_count_ = 2;
    bool vk_swapchain_recreated_ = false;
    void* orig_vkCreateInstance_ = nullptr;
    void* orig_vkCreateDevice_ = nullptr;
    void* orig_vkCreateSwapchainKHR_ = nullptr;
    void* orig_vkQueuePresentKHR_ = nullptr;
    static int hooked_vkCreateInstance(const void*, const void*, void**);
    static int hooked_vkCreateDevice(void*, const void*, const void*, void**);
    static int hooked_vkCreateSwapchainKHR(void*, const void*, const void*, uint64_t*);
    static int hooked_vkQueuePresentKHR(void*, const void*);
    void hook_vulkan();
    void on_present_vulkan(void* queue, const void* pPresentInfo);
    void init_imgui_vulkan(void* queue, const void* pPresentInfo);
    void render_frame_vulkan(void* queue, const void* pPresentInfo);
    void cleanup_vulkan();
    ImTextureID upload_icon_vulkan(const std::vector<uint8_t>& rgba, int w, int h);

    void render_notifications(float dt);
    void render_panel();
    void render_hud();
    // Software cursor while open: guarantees a visible panel cursor even if
    // the game buried the OS cursor. Off when closed (game draws its own).
    void apply_cursor_mode() { ImGui::GetIO().MouseDrawCursor = open_; }
    bool save_rgba_png(const std::string& path, const uint8_t* rgba, int w, int h);
    std::string next_screenshot_path();
    static std::string screenshots_dir();
    void notify_screenshot(const std::string& file, bool dark = false);
    void maybe_capture_dx11(IDXGISwapChain* chain);
    void maybe_capture_dx9(struct IDirect3DDevice9* device);
    void maybe_capture_opengl();
#ifdef _WIN64
    void maybe_capture_dx12(IDXGISwapChain* chain);
#endif
    void capture_desktop_duplication();
    void maybe_capture_vulkan(void* queue, const void* pPresentInfo);
    void setup_imgui_style_and_fonts();
    void resolve_accent();
    ImU32 acc(float a) const;
    ImVec4 vacc(float a) const;
    void hook_window();
    void hook_window_for(HWND h);
    void toggle_overlay();
    void start_external_thread();
    void switch_to_external(const char* reason);
    void poll_hotkey();
    void poll_keyboard();
    void ensure_hooks();

    bool  enabled_           = true;
    bool  imgui_initialized_ = false;
    bool  hooks_installed_   = false;
    bool  dx11_hooked_       = false;
    bool  dx9_hooked_        = false;
    bool  opengl_hooked_     = false;
    bool  vulkan_hooked_     = false;
    bool  hotkey_prev_down_  = false;
    bool  f12_prev_down_     = false;
    std::atomic<bool> screenshot_requested_{ false };
    uint8_t prev_keys_[256]  = {};
    bool  prev_keys_valid_   = false;
    DWORD last_wmchar_tick_  = 0;
    float ui_scale_          = 1.0f;
    uint8_t acc_r_ = 0x4f, acc_g_ = 0xa3, acc_b_ = 0xff;
    uint64_t base_playtime_sec_ = 0;
    DWORD last_playtime_save_ = 0;
    uint64_t total_playtime_sec() const;
    // Blur-pause: session time accrues only while the game (or overlay)
    // window is foreground. Updated from poll_hotkey(), which runs per present.
    mutable double focused_sec_ = 0;
    DWORD focus_last_tick_ = 0;
    void poll_focus();
    uint64_t session_sec() const { return (uint64_t)focused_sec_; }
    // True game present rate (ImGui framerate == overlay rate in external
    // mode). Fed from the present hooks only, never the external thread loop.
    DWORD present_fps_tick_ = 0;
    int present_frames_ = 0;
    float present_fps_ = 0;
    void note_present();
    static std::string format_playtime(uint64_t secs);
    std::atomic<bool> retry_stop_{ false };
    bool  open_              = false;
    float panel_anim_        = 0.0f;
    float scroll_target_y_   = -1.0f;
    float scroll_current_y_  = 0.0f;
    int   cursor_show_count_offset_ = 0;

    char  achievement_filter_[64] = {};
    int   filter_mode_       = 0; // 0=all, 1=unlocked, 2=locked
    bool  bulk_confirm_pending_ = false;
    bool  bulk_is_unlock_ = true;
    int   session_unlocks_ = 0;
    std::string viewer_file_;
    bool  viewer_pending_ = false;
    std::string notes_text_;
    bool  notes_loaded_ = false;
    bool  notes_dirty_ = false;
    DWORD notes_last_edit_ = 0;
    void load_notes();
    void save_notes();

    ID3D11Device*           device_        = nullptr;
    ID3D11DeviceContext*    context_       = nullptr;
    ID3D11RenderTargetView* rtv_           = nullptr;
    HWND                    hwnd_          = nullptr;
    WNDPROC                 wnd_proc_orig_ = nullptr;

#ifdef _WIN64
    void* dx12_device_ = nullptr;
    void* dx12_command_queue_ = nullptr;
    void* dx12_rtv_heap_ = nullptr;
    void* dx12_srv_heap_ = nullptr;
    void* dx12_command_list_ = nullptr;
    std::vector<void*> dx12_command_allocators_;
    std::vector<void*> dx12_resources_;
    UINT dx12_buffer_count_ = 0;
    UINT dx12_srv_next_slot_ = 1;
    std::vector<void*> dx12_icon_resources_;
    // Frame fence: Reset() on an allocator the GPU is still reading is
    // undefined behavior (fast GPU fault / device removed). Track one fence
    // value per backbuffer and wait before reusing its allocator.
    void* dx12_fence_ = nullptr;
    uint64_t dx12_fence_value_ = 0;
    std::vector<uint64_t> dx12_frame_fence_;
    void* dx12_fence_event_ = nullptr;
    int dx12_timeout_streak_ = 0;

    using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(void*, UINT, void* const*);
    ExecuteCommandListsFn orig_execute_command_lists_ = nullptr;
    static void STDMETHODCALLTYPE hooked_ExecuteCommandLists(void* queue, UINT count, void* const* lists);
    static void* g_dx12_captured_queue_;
#endif

    void* font_small_ = nullptr;
    void* font_title_ = nullptr;

    std::mutex                        render_mutex_;
    std::mutex                        notif_mutex_;
    std::vector<AchievementNotification> notifications_;
    std::unordered_map<std::string, ImTextureID> icon_textures_;

    using PresentFn       = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn      = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PresentFn       orig_present_  = nullptr;
    Present1Fn      orig_present1_ = nullptr;
    ResizeBuffersFn orig_resize_   = nullptr;

    using ShowCursorFn = int(WINAPI*)(BOOL);
    using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
    using SetCursorFn  = HCURSOR(WINAPI*)(HCURSOR);
    ShowCursorFn    orig_show_cursor_ = nullptr;
    ClipCursorFn    orig_clip_cursor_ = nullptr;
    SetCursorFn     orig_set_cursor_  = nullptr;

    // Input blackout: while open_, the game must not see KB/mouse (XNA polls
    // state directly, so swallowing window messages alone is not enough).
    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
    using GetKeyStateFn      = SHORT(WINAPI*)(int);
    using GetCursorPosFn     = BOOL(WINAPI*)(LPPOINT);
    using SetCursorPosFn     = BOOL(WINAPI*)(int, int);
    using SendInputFn        = UINT(WINAPI*)(UINT, LPINPUT, int);
    using MouseEventFn       = void(WINAPI*)(DWORD, DWORD, DWORD, DWORD, ULONG_PTR);
    GetAsyncKeyStateFn orig_get_async_key_ = nullptr;
    GetKeyboardStateFn orig_get_keyboard_state_ = nullptr;
    GetKeyStateFn      orig_get_key_ = nullptr;
    GetCursorPosFn     orig_get_cursor_pos_ = nullptr;
    SetCursorPosFn     orig_set_cursor_pos_ = nullptr;
    SendInputFn        orig_send_input_ = nullptr;
    MouseEventFn       orig_mouse_event_ = nullptr;
    POINT frozen_cursor_ = {};
    SHORT real_GetAsyncKeyState(int vk);
    BOOL  real_GetKeyboardState(PBYTE keys);
    SHORT real_GetKeyState(int vk);
    static BOOL caller_in_self_module();
    static SHORT WINAPI hooked_GetAsyncKeyState(int vkey);
    static BOOL  WINAPI hooked_GetKeyboardState(PBYTE keys);
    static SHORT WINAPI hooked_GetKeyState(int vkey);
    static BOOL  WINAPI hooked_GetCursorPos(LPPOINT pt);
    static BOOL  WINAPI hooked_SetCursorPos(int x, int y);
    static UINT  WINAPI hooked_SendInput(UINT nInputs, LPINPUT pInputs, int cbSize);
    static void  WINAPI hooked_mouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo);

    static HRESULT STDMETHODCALLTYPE hooked_Present(IDXGISwapChain*, UINT, UINT);
    static HRESULT STDMETHODCALLTYPE hooked_Present1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static HRESULT STDMETHODCALLTYPE hooked_ResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    static LRESULT CALLBACK          star_wnd_proc(HWND, UINT, WPARAM, LPARAM);

    static int WINAPI hooked_ShowCursor(BOOL bShow);
    static BOOL WINAPI hooked_ClipCursor(const RECT* lpRect);
    static HCURSOR WINAPI hooked_SetCursor(HCURSOR hCursor);
};
