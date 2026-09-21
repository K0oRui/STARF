#include "overlay/overlay_internal.h"
#include "dx8/imgui_impl_dx8.h"
#include "imgui_impl_win32.h"
#include <MinHook.h>
#include "dx8/d3d8.h"


HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX8Present(IDirect3DDevice8* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* rgn)
{
    if (g_overlay) g_overlay->on_present_dx8(device, window);
    return g_overlay && g_overlay->orig_dx8_present_
        ? g_overlay->orig_dx8_present_(device, src, dst, window, rgn) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX8Reset(IDirect3DDevice8* device, void* params)
{
    if (g_overlay) g_overlay->on_reset_dx8(device);
    typedef HRESULT(STDMETHODCALLTYPE* DX8ResetFn)(IDirect3DDevice8*, void*);
    auto orig = (DX8ResetFn)g_overlay->orig_dx8_reset_;
    HRESULT hr = orig(device, params);
    if (SUCCEEDED(hr) && g_overlay && g_overlay->game_api_ == GraphicsAPI::DX8 &&
        g_overlay->mode_ != OverlayMode::External) {
        std::lock_guard<std::mutex> lock(g_overlay->render_mutex_);
        if (g_overlay->enabled_ && g_overlay->mode_ != OverlayMode::External &&
            g_overlay->imgui_initialized_ && g_overlay->active_api_ == GraphicsAPI::DX8 &&
            g_overlay->dx8_device_ == device)
            ImGui_ImplDX8_CreateDeviceObjects();
    }
    return hr;
}

struct IDirect3D8* STDMETHODCALLTYPE StarOverlay::hooked_Direct3DCreate8(UINT sdk_version)
{
    IDirect3D8* d3d = g_overlay ? g_overlay->orig_direct3dcreate8_(sdk_version) : nullptr;
    if (d3d && g_overlay) {
        void** vt = *(void***)d3d;
        if (!g_overlay->orig_d3d8_create_device_) {
            MH_STATUS s = MH_CreateHook(vt[15], &hooked_D3D8CreateDevice, (void**)&g_overlay->orig_d3d8_create_device_);
            if (s == MH_OK) MH_EnableHook(vt[15]);
        }
    }
    return d3d;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_D3D8CreateDevice(IDirect3D8* d3d, UINT adapter, int devtype, HWND hwnd, DWORD behavior, void* params, IDirect3DDevice8** out)
{
    HRESULT hr = g_overlay ? g_overlay->orig_d3d8_create_device_(d3d, adapter, devtype, hwnd, behavior, params, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && g_overlay) {
        void** vt = *(void***)*out;
        if (!g_overlay->orig_dx8_present_) {
            MH_STATUS s1 = MH_CreateHook(vt[15], &hooked_DX8Present, (void**)&g_overlay->orig_dx8_present_);
            if (s1 == MH_OK) MH_EnableHook(vt[15]);
        }
        if (!g_overlay->orig_dx8_reset_) {
            MH_STATUS s2 = MH_CreateHook(vt[14], &hooked_DX8Reset, (void**)&g_overlay->orig_dx8_reset_);
            if (s2 == MH_OK) MH_EnableHook(vt[14]);
        }
        if (g_overlay->orig_dx8_present_) {
            g_overlay->dx8_hooked_ = true;
            if (!g_overlay->any_graphics_hook_installed_) { g_overlay->any_graphics_hook_installed_ = true; STAR_LOG("DX8 hooked"); }
        }
    }
    return hr;
}

void StarOverlay::hook_dx8()
{
    if (orig_direct3dcreate8_) { dx8_hooked_ = true; return; }
    HMODULE d3d8_dll = GetModuleHandleA("d3d8.dll");
    if (!d3d8_dll) d3d8_dll = LoadLibraryA("d3d8.dll");
    if (!d3d8_dll) return;

    auto pDirect3DCreate8 = (Direct3DCreate8Fn)GetProcAddress(d3d8_dll, "Direct3DCreate8");
    if (!pDirect3DCreate8) return;

    MH_STATUS s = MH_CreateHook(pDirect3DCreate8, &hooked_Direct3DCreate8, (void**)&orig_direct3dcreate8_);
    if (s == MH_OK) {
        MH_EnableHook(pDirect3DCreate8);
        dx8_hooked_ = true;
    }
}

void StarOverlay::on_present_dx8(IDirect3DDevice8* device, HWND window)
{
    if (!enabled_ || !device) return;
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(device->GetCreationParameters(&cp))) return;
    if (!window) window = cp.hFocusWindow;
    if (!accept_backend(GraphicsAPI::DX8, window)) return;
    note_present();
    if (mode_ == OverlayMode::External) return;
    poll_hotkey();
    if (imgui_initialized_ && active_api_ != GraphicsAPI::DX8) return;
    if (imgui_initialized_ && dx8_device_ != device) shutdown_renderer();
    dx8_device_ = device;

    if (!imgui_initialized_) {
        hook_window_for(window);

        ImGui::CreateContext();
        if (!ImGui_ImplWin32_Init(hwnd_)) { ImGui::DestroyContext(); return; }

        if (ImGui_ImplDX8_Init(device)) {
            style_.setup();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::DX8;
            STAR_LOG("ImGui ready (DX8) hwnd=%p", hwnd_);
        } else {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            STAR_LOG("ImGui DX8 init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::DX8) {
        if (!ImGui_ImplDX8_NewFrame()) return;
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        build_frame_ui();

        // The renderer saves and restores device state itself.
        if (SUCCEEDED(device->BeginScene())) {
            ImGui_ImplDX8_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }

        maybe_capture_dx8(device);
    }
}

void StarOverlay::on_reset_dx8(IDirect3DDevice8* device)
{
    if (mode_ == OverlayMode::External || game_api_ != GraphicsAPI::DX8) return;
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (enabled_ && mode_ != OverlayMode::External && dx8_device_ == device &&
        active_api_ == GraphicsAPI::DX8 && imgui_initialized_) {
        ImGui_ImplDX8_InvalidateDeviceObjects();
    }
}

ImTextureID StarOverlay::upload_icon_dx8(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!dx8_device_) return nullptr;
    // MANAGED pool: survives Reset, no invalidate needed. ImGui DX8 backend
    // draws IDirect3DTexture8* directly.
    IDirect3DTexture8* tex = nullptr;
    if (FAILED(dx8_device_->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &tex)) || !tex)
        return nullptr;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0))) {
        // WIC hands us RGBA bytes; D3DFMT_A8R8G8B8 stores BGRA in memory,
        // so swizzle R<->B (otherwise browns render blue/purple).
        copy_pixels32(locked.pBits, locked.Pitch, rgba.data(), (size_t)w * 4, w, h, true);
        tex->UnlockRect(0);
        return (ImTextureID)(void*)tex;
    }
    tex->Release();
    return nullptr;
}

void StarOverlay::release_icons_dx8()
{
    icons_.release_all([](ImTextureID v) { ((IDirect3DTexture8*)v)->Release(); });
    dx8_device_ = nullptr;
}

void StarOverlay::maybe_capture_dx8(IDirect3DDevice8* device)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !device) return;
    IDirect3DSurface8* rt = nullptr;
    if (FAILED(device->GetRenderTarget(&rt)) || !rt) return;
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    IDirect3DSurface8* sys = nullptr;
    HRESULT hr = device->CreateImageSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, &sys);
    if (SUCCEEDED(hr) && sys) hr = device->CopyRects(rt, nullptr, 0, sys, nullptr);
    if (FAILED(hr) && sys) {
        // Multisampled render target: fall back to the front buffer (which
        // must be created in the display mode format).
        sys->Release(); sys = nullptr;
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(device->GetDisplayMode(&mode)) &&
            SUCCEEDED(device->CreateImageSurface(desc.Width, desc.Height, mode.Format, &sys)) && sys)
            hr = device->GetFrontBuffer(sys);
    }
    rt->Release();
    if (FAILED(hr) || !sys) {
        STAR_LOG("Screenshot: DX8 capture failed hr=0x%08x", (unsigned)hr);
        if (sys) sys->Release();
        return;
    }
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::vector<uint8_t> rgba((size_t)desc.Width * desc.Height * 4);
        D3DSURFACE_DESC sdesc{};
        sys->GetDesc(&sdesc);
        if (sdesc.Format == D3DFMT_R5G6B5) {
            for (UINT y = 0; y < desc.Height; y++) {
                const uint16_t* s = (const uint16_t*)((const uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
                uint8_t* d = rgba.data() + (size_t)y * desc.Width * 4;
                for (UINT x = 0; x < desc.Width; x++) {
                    uint16_t p = s[x];
                    d[0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                    d[1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
                    d[2] = (uint8_t)((p & 0x1F) * 255 / 31);
                    d[3] = 255;
                    d += 4;
                }
            }
        } else {
            copy_pixels32(rgba.data(), (size_t)desc.Width * 4, lr.pBits, lr.Pitch,
                          desc.Width, desc.Height, true);
        }
        sys->UnlockRect();
        std::string path = ScreenshotService::next_path();
        screenshots_.save_async(path, std::move(rgba), (int)desc.Width, (int)desc.Height);
    }
    sys->Release();
}
