#define DIRECT3D_VERSION 0x0700
#define DIRECTDRAW_VERSION 0x0700
#include "overlay/overlay_internal.h"
#include "dx7/imgui_impl_dx7.h"
#include "imgui_impl_win32.h"
#include <d3d.h>
#include <MinHook.h>

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX7EndScene(IDirect3DDevice7* device)
{
    auto* overlay = g_overlay;
    if (!overlay || !overlay->orig_dx7_end_scene_) return E_FAIL;
    // The game's scene is still open. DX7 cannot nest BeginScene calls.
    static thread_local bool rendering = false;
    if (!rendering) {
        rendering = true;
        overlay->on_end_scene_dx7(device);
        rendering = false;
    }
    HRESULT hr = overlay->orig_dx7_end_scene_(device);
    // Surface readback is only valid after the scene has ended.
    if (SUCCEEDED(hr) && !rendering) overlay->maybe_capture_dx7(device);
    return hr;
}

void StarOverlay::hook_dx7()
{
    if (dx7_hooked_) return;
    // Do not load DirectDraw into every modern game. Retry when it appears.
    HMODULE module = GetModuleHandleW(L"ddraw.dll");
    if (!module) return;
    using CreateFn = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
    auto create = (CreateFn)GetProcAddress(module, "DirectDrawCreateEx");
    if (!create) return;
    IDirectDraw7* draw = nullptr;
    IDirect3D7* d3d = nullptr;
    IDirectDrawSurface7* target = nullptr;
    IDirect3DDevice7* device = nullptr;
    HWND dummy = CreateWindowExA(0, "STAR_Dummy", "", WS_OVERLAPPEDWINDOW,
        0, 0, 16, 16, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dummy) return;
    HRESULT hr = create(nullptr, (void**)&draw, IID_IDirectDraw7, nullptr);
    if (SUCCEEDED(hr)) hr = draw->SetCooperativeLevel(dummy, DDSCL_NORMAL);
    if (SUCCEEDED(hr)) hr = draw->QueryInterface(IID_IDirect3D7, (void**)&d3d);
    DDSURFACEDESC2 desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = desc.dwHeight = 16;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    if (SUCCEEDED(hr)) hr = draw->CreateSurface(&desc, &target, nullptr);
    if (SUCCEEDED(hr)) hr = d3d->CreateDevice(IID_IDirect3DHALDevice, target, &device);
    if (SUCCEEDED(hr)) {
        // IDirect3DDevice7: IUnknown(0..2), GetCaps, EnumTextureFormats,
        // BeginScene(5), EndScene(6). Patch the implementation, so devices
        // that already existed before SteamAPI_Init are covered as well.
        void* end_scene = (*(void***)device)[6];
        MH_STATUS status = MH_CreateHook(end_scene, &hooked_DX7EndScene, (void**)&orig_dx7_end_scene_);
        if (status == MH_OK) {
            status = MH_EnableHook(end_scene);
            if (status == MH_OK) {
                dx7_hooked_ = true;
                STAR_LOG("DX7 hooked");
            } else {
                MH_RemoveHook(end_scene);
                orig_dx7_end_scene_ = nullptr;
            }
        }
    }
    if (device) device->Release();
    if (target) target->Release();
    if (d3d) d3d->Release();
    if (draw) draw->Release();
    DestroyWindow(dummy);
}

void StarOverlay::on_end_scene_dx7(IDirect3DDevice7* device)
{
    if (!enabled_ || !device || !backend_mine({GraphicsAPI::DX7})) return;
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    HWND window = IsWindow(game_window_) ? game_window_ : find_game_window();
    if (!accept_backend(GraphicsAPI::DX7, window)) return;
    note_present();
    if (mode_ == OverlayMode::External) return;
    if (migrate_if_tiny_frame(window)) return;
    if (imgui_initialized_ && active_api_ != GraphicsAPI::DX7) return;
    if (imgui_initialized_ && dx7_device_ != device) shutdown_renderer();
    hook_window_for(window);
    if (!imgui_initialized_) {
        ImGui::CreateContext();
        if (!ImGui_ImplWin32_Init(window)) { ImGui::DestroyContext(); return; }
        if (!ImGui_ImplDX7_Init(device)) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            STAR_LOG("ImGui DX7 init failed");
            return;
        }
        dx7_device_ = device;
        active_api_ = GraphicsAPI::DX7;
        style_.setup();
        imgui_initialized_ = true;
        STAR_LOG("ImGui ready (DX7) hwnd=%p", window);
    }
    poll_hotkey();
    IDirectDrawSurface7* current_target = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(&current_target))) {
        HRESULT surface_status = current_target->IsLost();
        current_target->Release();
        if (surface_status == DDERR_SURFACELOST) {
            ImGui_ImplDX7_InvalidateDeviceObjects();
            release_icons_dx7();
            dx7_device_ = device;
            return;
        }
    }
    if (!ImGui_ImplDX7_NewFrame()) {
        STAR_LOG_ONCE("DX7 font texture upload failed");
        return;
    }
    ImGui_ImplWin32_NewFrame();
    // Draw against the target's dimensions, not the desktop or launcher size.
    IDirectDrawSurface7* target = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(&target))) {
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        if (SUCCEEDED(target->GetSurfaceDesc(&desc))) {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 old = io.DisplaySize;
            io.DisplaySize = ImVec2((float)desc.dwWidth, (float)desc.dwHeight);
            if (old.x > 0 && old.y > 0 && io.MousePos.x > -FLT_MAX && io.MousePos.y > -FLT_MAX) {
                io.MousePos.x *= io.DisplaySize.x / old.x;
                io.MousePos.y *= io.DisplaySize.y / old.y;
            }
        }
        target->Release();
    }
    ImGui::NewFrame();
    apply_cursor_mode();
    build_frame_ui();
    HRESULT hr = ImGui_ImplDX7_RenderDrawData(ImGui::GetDrawData());
    static HRESULT last_error = S_OK;
    if (FAILED(hr) && hr != last_error) STAR_LOG("DX7 draw failed hr=0x%08x", (unsigned)hr);
    last_error = hr;
}

ImTextureID StarOverlay::upload_icon_dx7(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!dx7_device_ || w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return nullptr;
    return (ImTextureID)ImGui_ImplDX7_CreateTexture(rgba.data(), w, h);
}

void StarOverlay::release_icons_dx7()
{
    icons_.release_all([](ImTextureID texture) { ((IDirectDrawSurface7*)texture)->Release(); });
    dx7_device_ = nullptr;
}

void StarOverlay::maybe_capture_dx7(IDirect3DDevice7* device)
{
    std::lock_guard<std::mutex> guard(render_mutex_);
    if (!enabled_ || mode_ == OverlayMode::External || active_api_ != GraphicsAPI::DX7 ||
        !imgui_initialized_ || !device || device != dx7_device_ || !screenshots_.consume()) return;
    IDirectDrawSurface7* target = nullptr;
    HRESULT hr = device->GetRenderTarget(&target);
    if (FAILED(hr) || !target) return;
    DDSURFACEDESC2 desc{};
    desc.dwSize = sizeof(desc);
    hr = target->GetSurfaceDesc(&desc);
    if (FAILED(hr) || !desc.dwWidth || !desc.dwHeight) { target->Release(); return; }

    IDirectDrawSurface7* readback = nullptr;
    DDSURFACEDESC2 pixels{};
    pixels.dwSize = sizeof(pixels);
    IDirectDrawSurface7* source = target;
    hr = target->Lock(nullptr, &pixels, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (FAILED(hr)) {
        // Some drivers cannot lock a video-memory render target directly.
        IUnknown* owner = nullptr;
        IDirectDraw7* draw = nullptr;
        hr = target->GetDDInterface((void**)&owner);
        if (SUCCEEDED(hr)) hr = owner->QueryInterface(IID_IDirectDraw7, (void**)&draw);
        if (owner) owner->Release();
        DDSURFACEDESC2 sys{};
        sys.dwSize = sizeof(sys);
        sys.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        sys.dwWidth = desc.dwWidth;
        sys.dwHeight = desc.dwHeight;
        sys.ddpfPixelFormat = desc.ddpfPixelFormat;
        sys.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        if (SUCCEEDED(hr)) hr = draw->CreateSurface(&sys, &readback, nullptr);
        if (draw) draw->Release();
        if (SUCCEEDED(hr)) hr = readback->Blt(nullptr, target, nullptr, DDBLT_WAIT, nullptr);
        if (SUCCEEDED(hr)) {
            source = readback;
            pixels = {};
            pixels.dwSize = sizeof(pixels);
            hr = source->Lock(nullptr, &pixels, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
        }
    }
    std::vector<uint8_t> rgba;
    if (SUCCEEDED(hr)) {
        const auto& fmt = pixels.ddpfPixelFormat;
        unsigned bytes = fmt.dwRGBBitCount / 8;
        if ((fmt.dwFlags & DDPF_RGB) && bytes >= 2 && bytes <= 4) {
            auto unpack = [](DWORD value, DWORD mask) -> uint8_t {
                if (!mask) return 0;
                unsigned shift = 0;
                while (!(mask & 1)) { mask >>= 1; ++shift; }
                return (uint8_t)((((value >> shift) & mask) * 255ull + mask / 2) / mask);
            };
            rgba.resize((size_t)desc.dwWidth * desc.dwHeight * 4);
            for (DWORD y = 0; y < desc.dwHeight; ++y) {
                const auto* row = (const uint8_t*)pixels.lpSurface + (ptrdiff_t)y * pixels.lPitch;
                auto* output = rgba.data() + (size_t)y * desc.dwWidth * 4;
                for (DWORD x = 0; x < desc.dwWidth; ++x) {
                    DWORD value = 0;
                    memcpy(&value, row + (size_t)x * bytes, bytes);
                    output[x * 4] = unpack(value, fmt.dwRBitMask);
                    output[x * 4 + 1] = unpack(value, fmt.dwGBitMask);
                    output[x * 4 + 2] = unpack(value, fmt.dwBBitMask);
                    output[x * 4 + 3] = 255;
                }
            }
        } else hr = DDERR_UNSUPPORTEDFORMAT;
        source->Unlock(nullptr);
    }
    if (readback) readback->Release();
    target->Release();
    if (FAILED(hr)) {
        STAR_LOG("Screenshot: DX7 readback failed hr=0x%08x", (unsigned)hr);
        return;
    }
    std::string path = ScreenshotService::next_path();
    screenshots_.save_async(path, std::move(rgba), (int)desc.dwWidth, (int)desc.dwHeight);
}
