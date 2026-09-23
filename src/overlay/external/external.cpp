// External overlay window: own transparent topmost window + own D3D11 device
// (or D3D9 for DX9 games) on its own thread. Zero hooks into game rendering,
// for hostile titles (e.g. engines whose backbuffers fault on foreign access).
// Reuses the same panel/toast/HUD/icon code as the hook path.
#include "overlay/overlay_internal.h"
#include "overlay/core/pixel_copy.h"
#include "core/settings.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx9.h"
#include <d3d9.h>
#include <d3d11.h>
#ifdef _WIN64
#include "overlay/backends/dx12_submission.h"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
struct FindGameCtx {    DWORD pid = 0;
    HWND self = nullptr;
    HWND best = nullptr;
    int best_area = 0;
};

BOOL CALLBACK find_game_enum(HWND hwnd, LPARAM lp)
{
    auto* ctx = (FindGameCtx*)lp;
    if (hwnd == ctx->self) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->best_area) {
        ctx->best_area = area;
        ctx->best = hwnd;
    }
    return TRUE;
}
} // namespace

DWORD WINAPI StarOverlay::external_thread_entry(LPVOID self)
{
    ((StarOverlay*)self)->external_thread_proc();
    return 0;
}

HWND StarOverlay::find_game_window()
{
    FindGameCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.self = StarOverlay::get().ext_hwnd_;
    EnumWindows(&find_game_enum, (LPARAM)&ctx);
    return ctx.best;
}

LRESULT CALLBACK StarOverlay::ext_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    StarOverlay& o = StarOverlay::get();
    if (hwnd == o.ext_hwnd_ && hwnd != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return TRUE;
        // Our own fullscreen window: unhandled input stays here when open
        // (click-through flag handles the closed case at OS level).
        if (o.open_) {
            switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MOUSEMOVE:   case WM_MOUSEWHEEL:
            case WM_KEYDOWN:     case WM_KEYUP:    case WM_CHAR:
                return 0;
            }
        }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

bool StarOverlay::external_create_window(int x, int y, int w, int h)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ext_wnd_proc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "STAR_External";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExA(&wc)) return false;
        registered = true;
    }
    ext_hwnd_ = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "STAR_External", "STAR Overlay",
        WS_POPUP, x, y, w, h,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!ext_hwnd_) {
        STAR_LOG("External: CreateWindowEx failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    return true;
}

void StarOverlay::external_free_surfaces()
{
    cleanup_rtv();
    ext_w_ = ext_h_ = 0;
    external_draw_snapshot_.clear();
    ext_prev_.clear();
    ext_pixels_.reset();
    if (ext_d3d9_sys_) { ext_d3d9_sys_->Release(); ext_d3d9_sys_ = nullptr; }
    if (ext_d3d9_rt_) { ext_d3d9_rt_->Release(); ext_d3d9_rt_ = nullptr; }
    if (ext_stage_tex_) { ext_stage_tex_->Release(); ext_stage_tex_ = nullptr; }
    if (ext_rt_tex_) { ext_rt_tex_->Release(); ext_rt_tex_ = nullptr; }
}

bool StarOverlay::external_alloc_surfaces(int w, int h)
{
    external_free_surfaces();
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;

    if (ext_use_d3d9_) {
        if (!ext_d3d9_dev_) return false;
        // Offscreen RT + system-memory readback surface. A8R8G8B8 keeps the
        // premultiplied-alpha output UpdateLayeredWindow expects (AC_SRC_ALPHA).
        if (FAILED(ext_d3d9_dev_->CreateRenderTarget((UINT)w, (UINT)h, D3DFMT_A8R8G8B8,
                D3DMULTISAMPLE_NONE, 0, FALSE, &ext_d3d9_rt_, nullptr)) || !ext_d3d9_rt_)
            return false;
        if (FAILED(ext_d3d9_dev_->CreateOffscreenPlainSurface((UINT)w, (UINT)h,
                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &ext_d3d9_sys_, nullptr)) || !ext_d3d9_sys_) {
            external_free_surfaces();
            return false;
        }
    } else {
        if (!device_) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w; td.Height = (UINT)h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &ext_rt_tex_)) || !ext_rt_tex_)
            return false;

        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &ext_stage_tex_)) || !ext_stage_tex_) {
            external_free_surfaces();
            return false;
        }
    }

    if (!ext_pixels_.create(w, h)) {
        external_free_surfaces();
        return false;
    }
    // These dimensions describe the allocated buffers, not the tracked window.
    ext_w_ = w;
    ext_h_ = h;
    STAR_LOG("External: surfaces ready (%dx%d)", w, h);
    return true;
}

bool StarOverlay::external_create_device()
{
    RECT rc{};
    GetClientRect(ext_hwnd_, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    // Match the game's API when known: DX9 games get a D3D9 overlay window
    // (lighter on old machines). Everything else uses D3D11 with feature
    // level fallback (11_0 -> 9_3, then WARP software rendering).
    if (game_api_ == GraphicsAPI::DX9) {
        IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d9) {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            pp.BackBufferCount = 1;
            pp.hDeviceWindow = ext_hwnd_;
            IDirect3DDevice9* dev = nullptr;
            HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, ext_hwnd_,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
            if (FAILED(hr) || !dev) {
                hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, ext_hwnd_,
                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
            }
            d3d9->Release();
            if (SUCCEEDED(hr) && dev) {
                ext_use_d3d9_ = true;
                ext_d3d9_dev_ = dev;
                if (!external_alloc_surfaces(w, h)) {
                    STAR_LOG("External: D3D9 surface alloc failed");
                    dev->Release(); ext_d3d9_dev_ = nullptr;
                    ext_use_d3d9_ = false;
                    return false;
                }
                STAR_LOG("External: D3D9 device ready (%dx%d)", ext_w_, ext_h_);
                return true;
            }
            STAR_LOG("External: D3D9 device failed hr=0x%08x, falling back to D3D11", (unsigned)hr);
        }
    }

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3
    };
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
        &dev, &fl, &ctx);
    if (FAILED(hr) || !dev) {
        STAR_LOG("External: D3D11 hardware failed hr=0x%08x, trying WARP", (unsigned)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
            &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !dev) {
        STAR_LOG("External: D3D11CreateDevice failed hr=0x%08x", (unsigned)hr);
        return false;
    }
    device_ = dev;
    context_ = ctx;
    if (!external_alloc_surfaces(w, h)) {
        STAR_LOG("External: surface alloc failed");
        context_->Release(); context_ = nullptr;
        device_->Release(); device_ = nullptr;
        return false;
    }
    return true;
}

void StarOverlay::external_track_game_window()
{
    std::lock_guard<std::mutex> lock(render_mutex_);
    HWND game = IsWindow(game_window_) ? game_window_ :
        (game_api_ == GraphicsAPI::None ? find_game_window() : nullptr);
    ext_game_hwnd_ = game;
    {
        static HWND logged_game = nullptr;
        if (game != logged_game) {
            logged_game = game;
            STAR_LOG("External: tracking game window hwnd=%p", game);
        }
    }
    if (!game || IsIconic(game)) {
        if (ext_visible_) {
            ShowWindow(ext_hwnd_, SW_HIDE);
            ext_visible_ = false;
        }
        fg_ok_ = false;
        return;
    }
    // Only ever show over the game itself (or while interacting with us).
    // On desktop/browser the fullscreen topmost window fights the taskbar.
    HWND fg = GetForegroundWindow();
    fg_ok_ = (fg == game || fg == ext_hwnd_);
    if (!fg_ok_) {
        if (ext_visible_) {
            ShowWindow(ext_hwnd_, SW_HIDE);
            ext_visible_ = false;
        }
        return;
    }
    RECT rc{};
    GetWindowRect(game, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    // Stay topmost (games reorder) and cover the game window.
    SetWindowPos(ext_hwnd_, HWND_TOPMOST, rc.left, rc.top, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ext_visible_ = true;
}

void StarOverlay::external_cursor_open()
{
    POINT p{};
    read_cursor_pos(p);
    HWND anchor = ext_game_hwnd_ ? ext_game_hwnd_ : ext_hwnd_;
    RECT rc{};
    GetWindowRect(anchor, &rc);
    if (rc.right <= rc.left) { rc.left = 0; rc.top = 0; rc.right = 1280; rc.bottom = 720; }
    ext_cur_x_ = (float)(p.x < rc.left ? rc.left : (p.x >= rc.right ? rc.right - 1 : p.x));
    ext_cur_y_ = (float)(p.y < rc.top ? rc.top : (p.y >= rc.bottom ? rc.bottom - 1 : p.y));
    ext_last_real_ = p;
    // Park the OS cursor (game keeps it): probe count, drive hidden.
    int cur = probe_cursor_count();
    ext_show_saved_ = cur;
    int tries = 0;
    while (cur >= 0 && tries++ < 60) { show_cursor(FALSE); cur--; }
    ext_cur_init_ = true;
}

void StarOverlay::external_cursor_close()
{
    // Restore the exact pre-open visibility.
    int cur = probe_cursor_count();
    int tries = 0;
    while (cur < ext_show_saved_ && tries++ < 60) { show_cursor(TRUE); cur++; }
    // Continuity: game cursor resumes where the panel left off.
    if (orig_set_cursor_pos_) orig_set_cursor_pos_((int)ext_cur_x_, (int)ext_cur_y_);
    else SetCursorPos((int)ext_cur_x_, (int)ext_cur_y_);
    ext_cur_init_ = false;
}

void StarOverlay::external_cursor_frame()
{
    POINT real{};
    read_cursor_pos(real);
    HWND anchor = ext_game_hwnd_ ? ext_game_hwnd_ : ext_hwnd_;
    RECT rc{};
    GetWindowRect(anchor, &rc);
    if (rc.right > rc.left) {
        // Absolute mapping: while the panel is open the game's cursor warps
        // (SetCursorPos/SendInput/mouse_event) are swallowed by the input
        // hooks, so the OS cursor tracks the physical mouse 1:1. The old
        // delta accumulation drifted, froze on fast flicks that tripped the
        // warp filter, and jittered from any unhooked warp source.
        // Positions far outside the game rect (e.g. mid alt-tab) are ignored
        // rather than teleporting the panel cursor.
        if (real.x >= rc.left - 64 && real.x < rc.right + 64 &&
            real.y >= rc.top - 64 && real.y < rc.bottom + 64) {
            LONG cx = real.x < rc.left ? rc.left : (real.x >= rc.right ? rc.right - 1 : real.x);
            LONG cy = real.y < rc.top ? rc.top : (real.y >= rc.bottom ? rc.bottom - 1 : real.y);
            ext_cur_x_ = (float)cx;
            ext_cur_y_ = (float)cy;
        }
    }
    ext_last_real_ = real;
    // Keep the OS cursor parked in case the game re-showed it.
    if (probe_cursor_count() >= 0) show_cursor(FALSE);
    RECT wr{};
    GetWindowRect(ext_hwnd_, &wr);
    ImGui::GetIO().MousePos = { ext_cur_x_ - wr.left, ext_cur_y_ - wr.top };
}

void StarOverlay::external_render_frame()
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
#ifdef _WIN64
    star_dx12::reap_submissions();
#endif
    // Click-through state lives here (not below the surface checks) so a
    // bailed frame can never leave an invisible window swallowing the mouse.
    LONG_PTR ex = GetWindowLongPtrA(ext_hwnd_, GWL_EXSTYLE);
    LONG_PTR want_ex = open_ ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
    if (want_ex != ex) SetWindowLongPtrA(ext_hwnd_, GWL_EXSTYLE, want_ex);
    if (!imgui_initialized_) return;
    if (ext_use_d3d9_ ? !ext_d3d9_dev_ : (!device_ || !context_)) return;

    RECT rc{};
    GetClientRect(ext_hwnd_, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if ((w != ext_w_ || h != ext_h_) && !external_alloc_surfaces(w, h)) return;

    if (ext_use_d3d9_) {
        ImGui_ImplDX9_NewFrame();
    } else {
        if (!rtv_) device_->CreateRenderTargetView(ext_rt_tex_, nullptr, &rtv_);
        if (!rtv_) return;
        ImGui_ImplDX11_NewFrame();
    }

    ImGui_ImplWin32_NewFrame();
    if (!open_) {
        if (ext_cur_init_) external_cursor_close();
    } else {
        if (!ext_cur_init_) external_cursor_open();
        if (ext_cur_init_) external_cursor_frame();
    }
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();
    ImDrawData* dd = ImGui::GetDrawData();
    STAR_LOG_ONCE("External: first frame open=%d anim=%.3f disp=%.0fx%.0f lists=%d totalvtx=%d backend=%s",
        (int)open_, panel_anim_,
        ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y,
        dd ? dd->CmdListsCount : -1,
        dd ? dd->TotalVtxCount : -1,
        ext_use_d3d9_ ? "d3d9" : "d3d11");

    if (!external_draw_snapshot_.changed(ImGui::GetDrawData())) return;
    if (ext_use_d3d9_) {
        IDirect3DSurface9* old_rt = nullptr;
        ext_d3d9_dev_->GetRenderTarget(0, &old_rt);
        ext_d3d9_dev_->SetRenderTarget(0, ext_d3d9_rt_);
        ext_d3d9_dev_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.f, 0);
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        ext_d3d9_dev_->SetRenderTarget(0, old_rt);
        if (old_rt) old_rt->Release();
        // Readback into the layered-window DIB (premultiplied alpha output
        // matches what UpdateLayeredWindow expects with AC_SRC_ALPHA).
        if (SUCCEEDED(ext_d3d9_dev_->GetRenderTargetData(ext_d3d9_rt_, ext_d3d9_sys_))) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(ext_d3d9_sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                const uint8_t* src = (const uint8_t*)lr.pBits;
                uint8_t* dst = (uint8_t*)ext_pixels_.bits();
                copy_pixels32(dst, (size_t)w * 4, src, (size_t)lr.Pitch, (size_t)w, (size_t)h, false);
                ext_d3d9_sys_->UnlockRect();
                external_upload_layered(w, h);
            } else external_draw_snapshot_.clear();
        } else external_draw_snapshot_.clear();
    } else {
        const float clear[4]{};
        context_->OMSetRenderTargets(1, &rtv_, nullptr);
        context_->ClearRenderTargetView(rtv_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // CPU readback into the layered-window DIB (premultiplied alpha output
        // matches what UpdateLayeredWindow expects with AC_SRC_ALPHA).
        context_->CopyResource(ext_stage_tex_, ext_rt_tex_);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(context_->Map(ext_stage_tex_, 0, D3D11_MAP_READ, 0, &map))) {
            const uint8_t* src = (const uint8_t*)map.pData;
            uint8_t* dst = (uint8_t*)ext_pixels_.bits();
            // R8G8B8A8_UNORM maps as RGBA; the 32-bit BI_RGB DIB wants BGRA.
            copy_pixels32(dst, (size_t)w * 4, src, map.RowPitch, (size_t)w, (size_t)h, true);
            context_->Unmap(ext_stage_tex_, 0);
            external_upload_layered(w, h);
        } else external_draw_snapshot_.clear();
    }
}

void StarOverlay::external_upload_layered(int w, int h)
{
    // Identical pixels = skip the upload. A static HUD then costs no
    // DWM recomposite at all, which is what visibly flickered.
    uint8_t* dst = (uint8_t*)ext_pixels_.bits();
    size_t row = (size_t)w * 4;
    size_t bytes = row * (size_t)h;
    // Layered windows blend as premultiplied alpha; the renderer outputs
    // straight alpha, so convert before the compare/upload.
    premultiply_inplace(dst, (size_t)w, (size_t)h);
    if (ext_prev_.size() == bytes &&
        memcmp(ext_prev_.data(), dst, bytes) == 0)
        return;
    if (ext_prev_.size() != bytes) ext_prev_.resize(bytes);
    memcpy(ext_prev_.data(), dst, bytes);

    RECT rc{};
    POINT dst_pt{};
    GetWindowRect(ext_hwnd_, &rc);
    dst_pt.x = rc.left;
    dst_pt.y = rc.top;
    SIZE sz{ w, h };
    POINT src_pt{ 0, 0 };
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    BOOL ulw = UpdateLayeredWindow(ext_hwnd_, screen, &dst_pt, &sz, ext_pixels_.dc(),
        &src_pt, 0, &bf, ULW_ALPHA);
    DWORD ulw_err = ulw ? 0 : GetLastError();
    ReleaseDC(nullptr, screen);
    STAR_LOG_ONCE("External: first ULW ok=%d err=%lu (%dx%d)", (int)ulw,
        (unsigned long)ulw_err, w, h);
}

void StarOverlay::external_thread_proc()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (!external_create_window(0, 0, sw > 0 ? sw : 1280, sh > 0 ? sh : 720)) {
        STAR_LOG("External overlay: window failed, giving up");
        CoUninitialize();
        return;
    }
    // Wait briefly for the game's first present so the external window can
    // match its API: DX9 games get a lighter D3D9 backend, everything else
    // D3D11. The present hooks sniff the API even in external mode.
    for (int i = 0; i < 30 && game_api_ == GraphicsAPI::None; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::unique_lock<std::mutex> renderer_lock(render_mutex_);
    shutdown_renderer();
    if (!external_create_device()) {
        STAR_LOG("External overlay: device failed, giving up");
        DestroyWindow(ext_hwnd_);
        ext_hwnd_ = nullptr;
        CoUninitialize();
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    style_.setup();
    ImGui_ImplWin32_Init(ext_hwnd_);
    bool imgui_ok = ext_use_d3d9_
        ? ImGui_ImplDX9_Init(ext_d3d9_dev_)
        : ImGui_ImplDX11_Init(device_, context_);
    if (!imgui_ok) {
        STAR_LOG("External overlay: ImGui %s init failed", ext_use_d3d9_ ? "DX9" : "DX11");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (ext_use_d3d9_) {
            ext_d3d9_dev_->Release(); ext_d3d9_dev_ = nullptr;
            ext_use_d3d9_ = false;
        } else {
            context_->Release(); context_ = nullptr;
            device_->Release(); device_ = nullptr;
        }
        external_free_surfaces();
        DestroyWindow(ext_hwnd_);
        ext_hwnd_ = nullptr;
        CoUninitialize();
        return;
    }
    imgui_initialized_ = true;
    active_api_ = ext_use_d3d9_ ? GraphicsAPI::DX9 : GraphicsAPI::DX11;
    if (ext_use_d3d9_) dx9_device_ = ext_d3d9_dev_;
    renderer_lock.unlock();
    ext_visible_ = false;
    ShowWindow(ext_hwnd_, SW_HIDE);
    STAR_LOG("External overlay ready (%dx%d, %s)", ext_w_, ext_h_,
        ext_use_d3d9_ ? "d3d9" : "d3d11");

    MSG msg{};
    int frame = 0;
    while (!ext_stop_.load()) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ext_stop_ = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (ext_stop_.load()) break;

        {
            std::lock_guard<std::mutex> lock(render_mutex_);
            poll_hotkey(); // Game callbacks only detect/count presents in external mode.
        }

        // Screenshots have no present hook to ride on here: consume directly.
        // Duplication never touches game state, so this is always safe.
        if (screenshots_.consume())
            capture_desktop_duplication();

        if ((frame++ % 30) == 0) external_track_game_window();

        bool panel_active = open_ || panel_anim_ > 0.001f;
        bool want = fg_ok_ && (panel_active || notifications_.has_pending() || screenshots_.busy() ||
            Settings::get().overlay_show_fps || Settings::get().overlay_show_playtime);
        if (want != ext_visible_) {
            ShowWindow(ext_hwnd_, want ? SW_SHOWNOACTIVATE : SW_HIDE);
            ext_visible_ = want;
        }
        if (want) {
            external_render_frame();
            // Interactive while open, calm while closed (HUD/toasts only).
            // Keep the panel animating out at full rate too.
            std::this_thread::sleep_for(std::chrono::milliseconds(panel_active ? 8 : 33));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    renderer_lock.lock();
    shutdown_renderer();
    if (ext_d3d9_dev_) { ext_d3d9_dev_->Release(); ext_d3d9_dev_ = nullptr; }
    ext_use_d3d9_ = false;
    external_free_surfaces();
    if (ext_cur_init_) external_cursor_close(); // never leave the OS cursor parked hidden
    if (ext_hwnd_) { DestroyWindow(ext_hwnd_); ext_hwnd_ = nullptr; }
    CoUninitialize();
    STAR_LOG("External overlay stopped");
}
