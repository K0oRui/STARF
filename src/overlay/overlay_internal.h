#pragma once
// Internal overlay implementation. The full StarOverlay definition (all
// backend, capture, and UI state) lives here so the public facade in
// overlay.h stays free of platform headers. Only the overlay's own
// translation units include this header.
#include "core/star_common.h"
#include "overlay/overlay.h"
#include "overlay/core/notes_store.h"
#include "overlay/ui/notification_queue.h"
#include "overlay/capture/screenshot_service.h"
#include "overlay/ui/icon_cache.h"
#include "overlay/ui/draw_snapshot.h"
#include "overlay/ui/overlay_style.h"
#include "overlay/core/overlay_util.h"
#include "overlay/core/pixel_copy.h"
#include "overlay/core/backend_selection.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <dxgi1_4.h>
#include "imgui.h"
#include "overlay/backends/gdi_resources.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
struct IUnityInterfaces;

class StarOverlay final : public Overlay {
public:
    static StarOverlay& get();

    void init() override;
    void shutdown() override;

    void hook_graphics_early() { hook_vulkan(); hook_dxgi(); }
    void set_unity_interfaces(IUnityInterfaces* interfaces) { unity_interfaces_ = interfaces; }

    void push_achievement(const std::string& name, const std::string& desc,
                           const std::vector<uint8_t>& icon_rgba, int iw, int ih,
                           const std::string& header = "ACHIEVEMENT UNLOCKED",
                           bool summary = false) override;

    void request_screenshot() override;

    void enqueue_icon_decode(const std::string& path, const std::string& key,
                             const std::string& toast_title = "") override;

    void note_session_unlock() override { session_unlocks_++; }
    void note_session_revoke() override { if (session_unlocks_ > 0) session_unlocks_--; }

    bool is_enabled() const override { return enabled_; }
    bool is_open()    const override { return open_; }
    // Open-only (game API bridge): never toggles an open panel shut.
    void open_panel() override;

    enum class OverlayMode { Hook, External };
    std::atomic<OverlayMode> mode_{OverlayMode::Hook};
    // Auto-fallback backoff (see switch_to_external): fallback_count_ is the
    // number of sessions to skip before retrying hook mode; fallback_level_
    // grows the skip window on repeated failures (1,3,7,15,31,63). Persisted
    // in overlay.star; cleared when a retry session ends without falling back.
    int fallback_count_ = 0;
    int fallback_level_ = 0;
    bool retry_session_ = false;
    bool fell_back_this_session_ = false;

    // ---- external window mode (no game hooks; own transparent window) ----
    HWND ext_hwnd_ = nullptr;
    HWND ext_game_hwnd_ = nullptr;
    // Layered-window compositing (no swapchain): game frame rendered to an
    // RT texture, copied to staging, blitted via UpdateLayeredWindow.
    ID3D11Texture2D* ext_rt_tex_ = nullptr;
    ID3D11Texture2D* ext_stage_tex_ = nullptr;
    star_gdi::DibSurface ext_pixels_;
    // Previous uploaded frame: skip UpdateLayeredWindow when pixels are
    // identical (static HUD = no DWM recomposite = no flicker).
    std::vector<uint8_t> ext_prev_;
    // D3D9 external backend (DX9 games, lighter on old machines): offscreen
    // render target + system-memory readback surface, same DIB/ULW path.
    bool ext_use_d3d9_ = false;
    struct IDirect3DDevice9* ext_d3d9_dev_ = nullptr;
    struct IDirect3DSurface9* ext_d3d9_rt_ = nullptr;
    struct IDirect3DSurface9* ext_d3d9_sys_ = nullptr;
    HANDLE ext_thread_ = nullptr;
    std::atomic<bool> ext_stop_{ false };
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
    void external_upload_layered(int w, int h);
    static LRESULT CALLBACK ext_wnd_proc(HWND, UINT, WPARAM, LPARAM);

private:
    std::atomic<IUnityInterfaces*> unity_interfaces_{nullptr};
    static void* WINAPI hooked_vkGetInstanceProcAddr(void* instance, const char* name);
    static void* WINAPI hooked_vkGetDeviceProcAddr(void* device, const char* name);
    static void* vulkan_hook_for(const char* name, void* address);
    StarOverlay() = default;

    GraphicsAPI active_api_ = GraphicsAPI::None;
    BackendSelection game_api_; // Survives renderer loss, resize, and fallback.
    HWND game_window_ = nullptr;
    IDXGISwapChain* dxgi_chain_ = nullptr;
    HGLRC gl_context_ = nullptr;
    HDC gl_dc_ = nullptr;
    // Call with render_mutex_ held, before input, capture, or renderer work.
    bool accept_backend(GraphicsAPI api, HWND window);
    void shutdown_renderer();

    void hook_dxgi();
    void on_present(IDXGISwapChain* chain, UINT sync_interval, UINT flags);
    void on_resize_buffers(IDXGISwapChain* chain, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl);
    void render_frame(IDXGISwapChain* chain);
    void init_imgui(IDXGISwapChain* chain);
    void cleanup_rtv();
    void init_imgui_dx10(IDXGISwapChain* chain, struct ID3D10Device* device);
    void render_frame_dx10(IDXGISwapChain* chain);
    void cleanup_dx10_rtv();
    ImTextureID get_or_create_icon(const std::string& key,
                                   const std::vector<uint8_t>& rgba, int w, int h);

#ifdef _WIN64
    void on_present_dx12(IDXGISwapChain* chain);
    void try_init_dx12(IDXGISwapChain* chain);
    void init_imgui_dx12(IDXGISwapChain* chain, void* device, void* command_queue);
    void render_frame_dx12(IDXGISwapChain* chain);
    void cleanup_dx12();
    bool wait_dx12_idle();
    void retire_dx12_renderer();
    void hook_dx12_factory(IDXGIFactory* factory);
    void update_dx12_queue(IDXGISwapChain* chain, IUnknown* const* queues);
    uint64_t dx12_missing_queue_tick_ = 0;
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
    void on_present_dx9(struct IDirect3DDevice9* device, HWND window);
    void on_reset_dx9(struct IDirect3DDevice9* device);
    ImTextureID upload_icon_dx9(const std::vector<uint8_t>& rgba, int w, int h);
    ImTextureID upload_icon_dx10(const std::vector<uint8_t>& rgba, int w, int h);
    ImTextureID upload_icon_opengl(const std::vector<uint8_t>& rgba, int w, int h);

    using DX7EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3DDevice7*);
    DX7EndSceneFn orig_dx7_end_scene_ = nullptr;
    struct IDirect3DDevice7* dx7_device_ = nullptr;
    static HRESULT STDMETHODCALLTYPE hooked_DX7EndScene(struct IDirect3DDevice7*);
    void hook_dx7();
    void on_end_scene_dx7(struct IDirect3DDevice7*);
    ImTextureID upload_icon_dx7(const std::vector<uint8_t>& rgba, int w, int h);
    void release_icons_dx7();
    void maybe_capture_dx7(struct IDirect3DDevice7*);

    using DX8PresentFn = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3DDevice8*, const RECT*, const RECT*, HWND, const struct RGNDATA*);
    using DX8ResetFn   = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3DDevice8*, void*);
    using Direct3DCreate8Fn = struct IDirect3D8* (STDMETHODCALLTYPE*)(UINT);
    using DX8CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(struct IDirect3D8*, UINT, int, HWND, DWORD, void*, struct IDirect3DDevice8**);
    Direct3DCreate8Fn orig_direct3dcreate8_ = nullptr;
    DX8CreateDeviceFn orig_d3d8_create_device_ = nullptr;
    DX8PresentFn orig_dx8_present_ = nullptr;
    DX8ResetFn   orig_dx8_reset_   = nullptr;
    struct IDirect3DDevice8* dx8_device_ = nullptr;
    static struct IDirect3D8* STDMETHODCALLTYPE hooked_Direct3DCreate8(UINT sdk_version);
    static HRESULT STDMETHODCALLTYPE hooked_D3D8CreateDevice(struct IDirect3D8*, UINT, int, HWND, DWORD, void*, struct IDirect3DDevice8**);
    static HRESULT STDMETHODCALLTYPE hooked_DX8Present(struct IDirect3DDevice8*, const RECT*, const RECT*, HWND, const struct RGNDATA*);
    static HRESULT STDMETHODCALLTYPE hooked_DX8Reset(struct IDirect3DDevice8*, void*);
    void hook_dx8();
    void on_present_dx8(struct IDirect3DDevice8* device, HWND window);
    void on_reset_dx8(struct IDirect3DDevice8* device);
    ImTextureID upload_icon_dx8(const std::vector<uint8_t>& rgba, int w, int h);
    void release_icons_dx8();
    void maybe_capture_dx8(struct IDirect3DDevice8* device);

    using wglSwapBuffersFn = BOOL(WINAPI*)(HDC);
    wglSwapBuffersFn orig_wglSwapBuffers_ = nullptr;
    using wglDeleteContextFn = BOOL(WINAPI*)(HGLRC);
    wglDeleteContextFn orig_wglDeleteContext_ = nullptr;
    static BOOL WINAPI hooked_wglSwapBuffers(HDC);
    static BOOL WINAPI hooked_wglDeleteContext(HGLRC);
    void hook_opengl();
    void shutdown_opengl();
    void on_present_opengl(HDC hdc);

    void* vk_instance_ = nullptr;
    void* vk_physical_device_ = nullptr;
    void* vk_device_ = nullptr;
    uint64_t vk_swapchain_ = 0;
    void* vk_queue_ = nullptr;
    uint64_t vk_surface_ = 0;
    void* vk_data_ = nullptr;
    uint32_t vk_width_ = 0, vk_height_ = 0, vk_image_usage_ = 0;
    uint32_t vk_queue_family_ = UINT32_MAX;
    int vk_swapchain_format_ = 0;
    uint32_t vk_min_image_count_ = 2;
    DWORD vk_first_missing_tick_ = 0;
    void* orig_vkCreateInstance_ = nullptr;
    void* orig_vkCreateDevice_ = nullptr;
    void* orig_vkCreateSwapchainKHR_ = nullptr;
    void* orig_vkQueuePresentKHR_ = nullptr;
    void* orig_vkDestroySwapchainKHR_ = nullptr;
    void* orig_vkDestroyDevice_ = nullptr;
    static void WINAPI hooked_vkDestroySwapchainKHR(void*, uint64_t, const void*);
    static void WINAPI hooked_vkDestroyDevice(void*, const void*);
    static int WINAPI hooked_vkCreateInstance(const void*, const void*, void**);
    static int WINAPI hooked_vkCreateDevice(void*, const void*, const void*, void**);
    static int WINAPI hooked_vkCreateSwapchainKHR(void*, const void*, const void*, uint64_t*);
    static int WINAPI hooked_vkQueuePresentKHR(void*, const void*);
    static int WINAPI hooked_vkEnumeratePhysicalDevices(void*, uint32_t*, void**);
    static int WINAPI hooked_vkCreateWin32SurfaceKHR(void*, const void*, const void*, uint64_t*);
    static void WINAPI hooked_vkDestroySurfaceKHR(void*, uint64_t, const void*);
    static void WINAPI hooked_vkGetDeviceQueue(void*, uint32_t, uint32_t, void**);
    static void WINAPI hooked_vkGetDeviceQueue2(void*, const void*, void**);
    void hook_vulkan();
    void on_present_vulkan(void* queue, const void* pPresentInfo);
    void init_imgui_vulkan(void* queue, const void* pPresentInfo);
    void render_frame_vulkan(void* queue, const void* pPresentInfo);
    void cleanup_vulkan();
    ImTextureID upload_icon_vulkan(const std::vector<uint8_t>& rgba, int w, int h);

    // ---- GDI backend (BitBlt/StretchBlt/StretchDIBits games) ----
    using BitBltFn            = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
    using StretchBltFn        = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    using StretchDIBitsFn     = int(WINAPI*)(HDC, int, int, int, int, int, int, int, int, const VOID*, const BITMAPINFO*, UINT, DWORD);
    using SetDIBitsToDeviceFn = int(WINAPI*)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const VOID*, const BITMAPINFO*, UINT);

    BitBltFn            orig_bitblt_            = nullptr;
    StretchBltFn        orig_stretchblt_        = nullptr;
    StretchDIBitsFn     orig_stretchdibits_     = nullptr;
    SetDIBitsToDeviceFn orig_setdibitstodevice_ = nullptr;

    static BOOL WINAPI hooked_BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    static BOOL WINAPI hooked_StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    static int  WINAPI hooked_StretchDIBits(HDC, int, int, int, int, int, int, int, int, const VOID*, const BITMAPINFO*, UINT, DWORD);
    static int  WINAPI hooked_SetDIBitsToDevice(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const VOID*, const BITMAPINFO*, UINT);

    void hook_gdi();
    void check_blit_and_present(HDC hdc, int x, int y, int cx, int cy);
    bool render_gdi(HWND hwnd, HDC dest_dc, int w, int h);
    bool gdi_init_device();
    bool gdi_alloc_surfaces(int w, int h);
    void maybe_capture_gdi(HDC src_dc, int w, int h);

    star_gdi::Resources gdi_;
    std::mutex gdi_hook_mutex_;
    DWORD gdi_last_invalidate_ = 0;
    bool gdi_wants_draw() const;
    bool prepare_gdi_present(HWND window);
    static LRESULT CALLBACK gdi_message_hook(int code, WPARAM removed, LPARAM message);
    HHOOK gdi_message_hook_ = nullptr;
    DWORD gdi_message_thread_ = 0;
    std::atomic<int> gdi_wheel_x_{0}, gdi_wheel_y_{0};

    void render_notifications(float dt);
    void render_panel();
    void panel_header(ImFont* fsmall, ImFont* ftitle, float pw);
    void panel_screenshots(ImFont* fsmall, float sw, float sh);
    void panel_achievements(ImFont* fsmall, ImFont* ftitle, float pw, float sw, float sh);
    void panel_display(ImFont* fsmall);
    void panel_notes(ImFont* fsmall);
    void panel_achievement_list(ImFont* fsmall, ImFont* ftitle);
    void render_hud();
    // Shared per-present UI build: panel animation, panel, notifications, HUD,
    // then ImGui::Render(). Backend-specific draw data submission follows.
    void build_frame_ui();
    // Software cursor while open: guarantees a visible panel cursor even if
    // the game buried the OS cursor. Off when closed (game draws its own).
    // While open, also keep the OS cursor visible: gameplay titles re-hide it
    // every frame, and without this the count drifts so close can't restore it.
    void apply_cursor_mode() {
        ImGui::GetIO().MouseDrawCursor = open_;
        if (open_ && mode_ != OverlayMode::External) {
            int c = orig_show_cursor_ ? orig_show_cursor_(TRUE) : ShowCursor(TRUE);
            if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
            if (c - 1 < 1) {
                if (orig_show_cursor_) orig_show_cursor_(TRUE); else ShowCursor(TRUE);
            }
        }
    }
    void notify_screenshot(const std::string& file, bool dark = false);
    void maybe_capture_dx11(IDXGISwapChain* chain);
    void maybe_capture_dx10(IDXGISwapChain* chain);
    void maybe_capture_dx9(struct IDirect3DDevice9* device);
    void maybe_capture_opengl();
#ifdef _WIN64
    void maybe_capture_dx12(IDXGISwapChain* chain);
#endif
    void capture_desktop_duplication();
    void maybe_capture_vulkan(void* queue, const void* pPresentInfo);
    void hook_window();
    void hook_window_for(HWND h);
    void toggle_overlay();
    void start_external_thread();
    void switch_to_external(const char* reason);
    void poll_hotkey();
    void poll_keyboard();
    void ensure_hooks();

    std::atomic<bool> enabled_{false};
    bool  imgui_initialized_ = false;
    bool  hooks_installed_   = false;
    bool  dxgi_hooked_       = false;
    bool  dx9_hooked_        = false;
    bool  dx8_hooked_        = false;
    bool  dx7_hooked_        = false;
    bool  opengl_hooked_     = false;
    bool  vulkan_hooked_     = false;
    bool  gdi_hooked_        = false;
    std::atomic<bool> any_graphics_hook_installed_{false};
    std::atomic<bool> hotkey_prev_down_{false};
    bool  f12_prev_down_     = false;
    ScreenshotService screenshots_;
    uint8_t prev_keys_[256]  = {};
    bool  prev_keys_valid_   = false;
    DWORD last_wmchar_tick_  = 0;
    OverlayStyle style_;
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
    std::mutex retry_mutex_;
    std::condition_variable retry_cv_;
    std::thread retry_thread_;
    bool  open_              = false;
    float panel_anim_        = 0.0f;
    float panel_target_      = 0.0f;
    float panel_anim_t0_     = 0.0f;
    float last_frame_time_   = 0.0f;
    int   cursor_show_count_offset_ = 0;

    char  achievement_filter_[64] = {};
    int   filter_mode_       = 0; // 0=all, 1=unlocked, 2=locked
    bool  bulk_confirm_pending_ = false;
    bool  bulk_is_unlock_ = true;
    int   session_unlocks_ = 0;
    std::string viewer_file_;
    bool  viewer_pending_ = false;
    NotesStore notes_;

    ID3D11Device*           device_        = nullptr;
    ID3D11DeviceContext*    context_       = nullptr;
    ID3D11RenderTargetView* rtv_           = nullptr;
    struct ID3D10Device*           dx10_device_ = nullptr;
    struct ID3D10RenderTargetView* dx10_rtv_   = nullptr;
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
    UINT dx12_frame_index_ = 0;
    IDXGISwapChain* dx12_chain_ = nullptr;
    UINT dx12_srv_next_slot_ = 1;
    std::vector<void*> dx12_icon_resources_;
    std::vector<UINT> dx12_free_icon_slots_;
    // Frame fence: Reset() on an allocator the GPU is still reading is
    // undefined behavior (fast GPU fault / device removed). Track one fence
    // value per backbuffer and wait before reusing its allocator.
    void* dx12_fence_ = nullptr;
    uint64_t dx12_fence_value_ = 0;
    std::vector<uint64_t> dx12_frame_fence_;
    void* dx12_fence_event_ = nullptr;
    int dx12_timeout_streak_ = 0;

#endif

    std::mutex                        render_mutex_;
    NotificationQueue notifications_;
    IconCache         icons_;
    DrawSnapshot external_draw_snapshot_;

    struct IconDecodeRequest {
        std::string path;
        std::string key;
        std::string toast_title;
    };
    struct IconDecodeResult {
        std::string key;
        std::string toast_title;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
    };
    std::deque<IconDecodeRequest> icon_decode_queue_;
    std::deque<IconDecodeResult> icon_decode_ready_;
    std::unordered_set<std::string> icon_decode_pending_;
    std::mutex                    icon_decode_mutex_;
    std::condition_variable       icon_decode_cv_;
    std::thread                   icon_decode_thread_;
    bool                          icon_decode_stop_ = false;
    std::deque<std::string> screenshot_icons_;
    std::deque<std::string> other_icons_;
    std::string viewer_icon_;
    void release_icon(ImTextureID texture);
    void release_icon_vulkan(ImTextureID texture);
#ifdef _WIN64
    void release_icon_dx12(ImTextureID texture);
#endif
    void drain_icon_decodes();
    void icon_decode_worker();

    using PresentFn       = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn      = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PresentFn       orig_present_  = nullptr;
    Present1Fn      orig_present1_ = nullptr;
    ResizeBuffersFn orig_resize_   = nullptr;
    using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
        DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    ResizeBuffers1Fn orig_resize1_ = nullptr;

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
    static BOOL caller_in_self_module(void* caller);
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
    static HRESULT STDMETHODCALLTYPE hooked_ResizeBuffers1(IDXGISwapChain3*, UINT, UINT, UINT,
        DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    static LRESULT CALLBACK          star_wnd_proc(HWND, UINT, WPARAM, LPARAM);

    static int WINAPI hooked_ShowCursor(BOOL bShow);
    static BOOL WINAPI hooked_ClipCursor(const RECT* lpRect);
    static HCURSOR WINAPI hooked_SetCursor(HCURSOR hCursor);
};

// Set once in core/overlay.cpp when the overlay initializes, cleared on
// shutdown. Every render/input hook reads it and passes through when null.
extern StarOverlay* g_overlay;
