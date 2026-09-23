#include "overlay/overlay_internal.h"
#include "overlay/core/pixel_copy.h"
#include "core/settings.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <MinHook.h>
#include <d3d11.h>
#include <wingdi.h>
#include <algorithm>

using star_gdi::ComPtr;

namespace {
thread_local bool in_gdi_present = false;
class PresentGuard {
public:
    PresentGuard() { in_gdi_present = true; }
    ~PresentGuard() { in_gdi_present = false; }
    PresentGuard(const PresentGuard&) = delete;
    PresentGuard& operator=(const PresentGuard&) = delete;
};

} // namespace

bool StarOverlay::gdi_wants_draw() const
{
    return open_ || panel_anim_ > 0.001f || (notifications_.has_pending() || screenshots_.busy()) ||
        Settings::get().overlay_show_fps || Settings::get().overlay_show_playtime;
}

bool StarOverlay::prepare_gdi_present(HWND window)
{
    if (!accept_backend(GraphicsAPI::GDI, window)) return false;
    if (mode_ != OverlayMode::External && imgui_initialized_ && active_api_ != GraphicsAPI::GDI)
        return false;
    note_present();
    if (mode_ == OverlayMode::External) return false;
    hook_window_for(window);
    const DWORD thread = GetWindowThreadProcessId(window, nullptr);
    if (thread != gdi_message_thread_) {
        if (gdi_message_hook_) UnhookWindowsHookEx(gdi_message_hook_);
        gdi_message_hook_ = SetWindowsHookExW(WH_GETMESSAGE, &gdi_message_hook, nullptr, thread);
        gdi_message_thread_ = gdi_message_hook_ ? thread : 0;
    }
    poll_hotkey();
    // Hotkeys, timing, and queued screenshots stay live without GPU work,
    // bitmap allocation, or frame copies while the overlay is idle.
    return gdi_wants_draw() || screenshots_.pending();
}

LRESULT CALLBACK StarOverlay::gdi_message_hook(int code, WPARAM removed, LPARAM message)
{
    auto* o = g_overlay;
    if (code >= 0 && removed == PM_REMOVE && o && o->enabled_ &&
        o->open_ && o->game_api_ == GraphicsAPI::GDI && o->mode_ != OverlayMode::External) {
        auto* msg = reinterpret_cast<MSG*>(message);
        if (msg && msg->hwnd && GetAncestor(msg->hwnd, GA_ROOT) == GetAncestor(o->game_window_, GA_ROOT)) {
            // RGSS can remove mouse messages without dispatching its WndProc.
            // Capture wheel deltas on removal, once, before that can happen.
            if (msg->message == WM_MOUSEWHEEL || msg->message == WM_MOUSEHWHEEL) {
                auto& wheel = msg->message == WM_MOUSEWHEEL ? o->gdi_wheel_y_ : o->gdi_wheel_x_;
                wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(msg->wParam));
                msg->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(nullptr, code, removed, message);
}

void StarOverlay::check_blit_and_present(HDC hdc, int x, int y, int cx, int cy)
{
    if (in_gdi_present || !enabled_ || cx <= 0 || cy <= 0 ||
        (game_api_ != GraphicsAPI::None && game_api_ != GraphicsAPI::GDI)) return;
    HWND window = WindowFromDC(hdc);
    if (!window) {
        // A screen DC has no process/window ownership. Only complete bitmap
        // frames are eligible for this fallback, never arbitrary desktop blits.
        if (GetObjectType(hdc) != OBJ_MEMDC) return;
        struct StripRun {
            HGDIOBJ bitmap = nullptr;
            int width = 0, height = 0, x = 0, span = 0, next_y = 0, bottom = 0;
            DWORD at = 0;
        };
        static thread_local std::unordered_map<HDC, StripRun> runs;
        const DWORD now = GetTickCount();
        HGDIOBJ bitmap = GetCurrentObject(hdc, OBJ_BITMAP);
        auto it = runs.find(hdc);
        if (it != runs.end() && (bitmap != it->second.bitmap || x != it->second.x ||
            cx != it->second.span || y != it->second.next_y ||
            (int64_t)y + cy > it->second.bottom || now - it->second.at > 250)) {
            runs.erase(it);
            it = runs.end();
        }
        if (it == runs.end()) {
            BITMAP info{};
            if (!GetObject(bitmap, sizeof(info), &info) || x < 0 || y < 0 ||
                info.bmWidth > 16384 || info.bmHeight > 16384 ||
                (int64_t)x * 2 + cx != info.bmWidth || cx < 50 ||
                (int64_t)info.bmHeight - y * 2LL < 50 ||
                (int64_t)y + cy > (int64_t)info.bmHeight - y) return;
            // RGSS uses centered letterboxing in fullscreen. A frame ends at
            // the matching bottom border, not at the bottom of its bitmap.
            // Only contiguous, full-width (or centered) strip runs qualify.
            if (runs.size() >= 32) runs.clear();
            it = runs.emplace(hdc, StripRun{bitmap, info.bmWidth, info.bmHeight,
                x, cx, y, info.bmHeight - y, now}).first;
        }
        auto& run = it->second;
        run.next_y = y + cy;
        run.at = now;
        if (run.next_y != run.bottom) return;
        const int width = run.width, height = run.height;
        runs.erase(it);
        PresentGuard guard;
        std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;
        window = IsWindow(game_window_) ? game_window_ : find_game_window();
        if (!window || !prepare_gdi_present(window)) return;
        const bool drawn = render_gdi(window, hdc, width, height);
        static int logged_width = 0, logged_height = 0;
        if (drawn && (logged_width != width || logged_height != height)) {
            logged_width = width; logged_height = height;
            STAR_LOG("GDI memory frame drawn: %dx%d hwnd=%p", width, height, window);
        }
        return;
    }
    if (!IsWindowVisible(window)) return;
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    int w = client.right, h = client.bottom;
    if (w < 50 || h < 50 || x >= w || y >= h) return;
    // Ordinary window GDI has no Present call. Use the bottom strip or a
    // partial-update timeout as the frame boundary, separately per thread/DC.
    static thread_local HDC last_dc = nullptr;
    static thread_local DWORD last_present = 0;
    DWORD now = GetTickCount();
    if ((int64_t)y + cy < h && last_dc == hdc && now - last_present < 33) return;
    last_dc = hdc;
    last_present = now;
    PresentGuard guard;
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (lock.owns_lock() && prepare_gdi_present(window))
        render_gdi(window, hdc, w, h);
}

BOOL WINAPI StarOverlay::hooked_BitBlt(HDC hdcDest, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
    auto* overlay = g_overlay;
    if (!overlay || !overlay->orig_bitblt_) return FALSE;
    BOOL ret = overlay->orig_bitblt_(hdcDest, x, y, cx, cy, hdcSrc, x1, y1, rop);
    if (ret) {
        overlay->check_blit_and_present(hdcDest, x, y, cx, cy);
    }
    return ret;
}

BOOL WINAPI StarOverlay::hooked_StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
{
    auto* overlay = g_overlay;
    if (!overlay || !overlay->orig_stretchblt_) return FALSE;
    BOOL ret = overlay->orig_stretchblt_(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
    if (ret) {
        overlay->check_blit_and_present(hdcDest, xDest, yDest, wDest, hDest);
    }
    return ret;
}

int WINAPI StarOverlay::hooked_StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight, int xSrc, int ySrc, int SrcWidth, int SrcHeight, const VOID* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop)
{
    auto* overlay = g_overlay;
    if (!overlay || !overlay->orig_stretchdibits_) return 0;
    int ret = overlay->orig_stretchdibits_(hdc, xDest, yDest, DestWidth, DestHeight, xSrc, ySrc, SrcWidth, SrcHeight, lpBits, lpbmi, iUsage, rop);
    if (ret > 0) {
        overlay->check_blit_and_present(hdc, xDest, yDest, DestWidth, DestHeight);
    }
    return ret;
}

int WINAPI StarOverlay::hooked_SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc, UINT StartScan, UINT cLines, const VOID* lpvBits, const BITMAPINFO* lpbmi, UINT ColorUse)
{
    auto* overlay = g_overlay;
    if (!overlay || !overlay->orig_setdibitstodevice_) return 0;
    int ret = overlay->orig_setdibitstodevice_(hdc, xDest, yDest, w, h, xSrc, ySrc, StartScan, cLines, lpvBits, lpbmi, ColorUse);
    if (ret > 0) {
        overlay->check_blit_and_present(hdc, xDest, yDest, (int)w, (int)h);
    }
    return ret;
}

void StarOverlay::hook_gdi()
{
    std::lock_guard<std::mutex> lock(gdi_hook_mutex_);
    if (gdi_hooked_) return;
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32) gdi32 = LoadLibraryW(L"gdi32.dll");
    if (!gdi32) { STAR_LOG("hook_gdi: gdi32.dll not found"); return; }

    auto install = [](const char* name, HMODULE module, auto hook, auto& original) {
        if (original) return true;
        void* target = (void*)GetProcAddress(module, name);
        if (!target) return false;
        MH_STATUS status = MH_CreateHook(target, (void*)hook, (void**)&original);
        if (status == MH_OK) {
            status = MH_EnableHook(target);
            if (status != MH_OK) {
                MH_RemoveHook(target);
                original = nullptr;
            }
        }
        return status == MH_OK;
    };
    bool bitblt = install("BitBlt", gdi32, &hooked_BitBlt, orig_bitblt_);
    bool stretch = install("StretchBlt", gdi32, &hooked_StretchBlt, orig_stretchblt_);
    bool dib = install("StretchDIBits", gdi32, &hooked_StretchDIBits, orig_stretchdibits_);
    bool setdib = install("SetDIBitsToDevice", gdi32, &hooked_SetDIBitsToDevice, orig_setdibitstodevice_);
    gdi_hooked_ = bitblt && stretch && dib && setdib;
    STAR_LOG("hook_gdi: bitblt=%d stretch=%d dib=%d setdib=%d hooked=%d",
        (int)bitblt, (int)stretch, (int)dib, (int)setdib, (int)gdi_hooked_);
    if (gdi_hooked_) STAR_LOG("GDI hooked");
}

bool StarOverlay::gdi_init_device()
{
    if (gdi_.device && gdi_.context) return true;
    // ImGui uses shader model 4.0. FL9 hardware must fall back to WARP.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        device.Reset();
        context.Reset();
        hr = D3D11CreateDevice(nullptr, driver, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &device, &level, &context);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        STAR_LOG("GDI renderer: device creation failed hr=0x%08x", (unsigned)hr);
        return false;
    }
    gdi_.device = std::move(device);
    gdi_.context = std::move(context);
    return true;
}

bool StarOverlay::gdi_alloc_surfaces(int w, int h)
{
    if (!gdi_.device || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    gdi_.reset_surfaces();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> render, staging;
    ComPtr<ID3D11RenderTargetView> view;
    if (FAILED(gdi_.device->CreateTexture2D(&desc, nullptr, &render)) ||
        FAILED(gdi_.device->CreateRenderTargetView(render.Get(), nullptr, &view))) return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gdi_.device->CreateTexture2D(&desc, nullptr, &staging)) ||
        !gdi_.pixels.create(w, h)) return false;
    gdi_.render_texture = std::move(render);
    gdi_.render_view = std::move(view);
    gdi_.staging_texture = std::move(staging);
    gdi_.width = w;
    gdi_.height = h;
    return true;
}

// Callers hold render_mutex_ and the recursion guard through presentation.
bool StarOverlay::render_gdi(HWND window, HDC dest_dc, int w, int h)
{
    if (!enabled_ || !dest_dc || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    // Tiny software framebuffer with visible UI: hook-drawn text would inherit
    // the game's pixels and upscale to mush, so migrate to the external window
    // (native resolution) instead. Gated on visible UI so a bare screenshot
    // never triggers it.
    if (gdi_wants_draw() && migrate_if_tiny_frame(w, h)) return false;
    maybe_capture_gdi(dest_dc, w, h);
    if (!gdi_wants_draw()) return true;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return false;
    const int render_w = client.right, render_h = client.bottom;
    if (!imgui_initialized_) {
        if (game_api_ != GraphicsAPI::None && game_api_ != GraphicsAPI::GDI) return false;
        if (!gdi_init_device()) return false;
        ImGui::CreateContext();
        if (!ImGui_ImplWin32_Init(window)) {
            ImGui::DestroyContext();
            return false;
        }
        if (!ImGui_ImplDX11_Init(gdi_.device.Get(), gdi_.context.Get())) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        style_.setup();
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::GDI;
        STAR_LOG("ImGui ready (GDI renderer) hwnd=%p", window);
    }
    if (active_api_ != GraphicsAPI::GDI) return false;
    if (render_w != gdi_.width || render_h != gdi_.height || !gdi_.render_view) {
        if (!gdi_alloc_surfaces(render_w, render_h)) return false;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    {
        POINT cursor{};
        read_cursor_pos(cursor);
        if (ScreenToClient(window, &cursor)) {
            io.AddMousePosEvent((float)cursor.x, (float)cursor.y);
        }
    }
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    const bool focused = foreground_pid == GetCurrentProcessId();
    const int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int i = 0; i < 5; ++i)
        io.AddMouseButtonEvent(i, open_ && focused && (real_GetAsyncKeyState(buttons[i]) & 0x8000));
    const int wheel_x = gdi_wheel_x_.exchange(0), wheel_y = gdi_wheel_y_.exchange(0);
    if (open_ && focused && (wheel_x || wheel_y))
        io.AddMouseWheelEvent(-(float)wheel_x / WHEEL_DELTA, (float)wheel_y / WHEEL_DELTA);
    // Keep all Win32 input and ImGui layout in client coordinates. AlphaBlend
    // scales the finished image into the game's smaller bitmap, not the input.
    io.DisplaySize = ImVec2((float)render_w, (float)render_h);
    // Rendering sleeps while idle; do not age a newly queued toast by that gap.
    io.DeltaTime = std::min(io.DeltaTime, 0.1f);
    ImGui::NewFrame();
    apply_cursor_mode();
    build_frame_ui();
    if (gdi_.draw_snapshot.changed(ImGui::GetDrawData())) {
        const float clear[4]{};
        gdi_.context->OMSetRenderTargets(1, gdi_.render_view.GetAddressOf(), nullptr);
        gdi_.context->ClearRenderTargetView(gdi_.render_view.Get(), clear);
        D3D11_VIEWPORT viewport{0.f, 0.f, (float)render_w, (float)render_h, 0.f, 1.0f};
        gdi_.context->RSSetViewports(1, &viewport);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gdi_.context->OMSetRenderTargets(0, nullptr, nullptr);
        gdi_.context->CopyResource(gdi_.staging_texture.Get(), gdi_.render_texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hr = gdi_.context->Map(gdi_.staging_texture.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) { gdi_.draw_snapshot.clear(); return false; }
        const auto* src = static_cast<const uint8_t*>(map.pData);
        auto* dst = static_cast<uint8_t*>(gdi_.pixels.bits());
        // Finish any previous GDI read of the DIB before overwriting its pixels.
        GdiFlush();
        copy_pixels32(dst, (size_t)render_w * 4, src, map.RowPitch, (size_t)render_w, (size_t)render_h, false);
        gdi_.context->Unmap(gdi_.staging_texture.Get(), 0);
        // AlphaBlend expects premultiplied alpha; the renderer is straight.
        premultiply_inplace(dst, (size_t)render_w, (size_t)render_h);
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    bool drawn = AlphaBlend(dest_dc, 0, 0, w, h, gdi_.pixels.dc(), 0, 0, render_w, render_h, blend) != FALSE;
    if (drawn) GdiFlush();
    // GDI has no Present; force a repaint so the overlay keeps animating even
    // when the game stops redrawing. Cap the rate so an idle game's message
    // loop cannot spin the CPU at thousands of frames per second.
    if (game_api_ == GraphicsAPI::GDI && gdi_wants_draw()) {
        DWORD now = GetTickCount();
        if (now - gdi_last_invalidate_ >= 16) {
            gdi_last_invalidate_ = now;
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    return drawn;
}

void StarOverlay::maybe_capture_gdi(HDC src_dc, int w, int h)
{
    if (!enabled_ || !screenshots_.pending() || !src_dc) return;
    star_gdi::DibSurface capture;
    if (!capture.create(w, h)) return;
    auto bitblt = orig_bitblt_ ? orig_bitblt_ : &BitBlt;
    if (!bitblt(capture.dc(), 0, 0, w, h, src_dc, 0, 0, SRCCOPY) || !GdiFlush()) return;
    if (!screenshots_.consume()) return;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    const auto* src = static_cast<const uint8_t*>(capture.bits());
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + 2];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + 0];
        rgba[i * 4 + 3] = 255;
    }
    std::string path = ScreenshotService::next_path();
    screenshots_.save_async(path, std::move(rgba), w, h);
}
