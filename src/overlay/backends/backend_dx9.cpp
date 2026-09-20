#include "overlay/overlay_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include <MinHook.h>
#include <d3d9.h>


HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX9Present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* rgn)
{
    if (g_overlay) g_overlay->on_present_dx9(device);
    return g_overlay ? g_overlay->orig_dx9_present_(device, src, dst, window, rgn) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX9Reset(IDirect3DDevice9* device, void* params)
{
    if (g_overlay) g_overlay->on_reset_dx9();
    typedef HRESULT(STDMETHODCALLTYPE* DX9ResetFn)(IDirect3DDevice9*, void*);
    auto orig = (DX9ResetFn)g_overlay->orig_dx9_reset_;
    HRESULT hr = orig(device, params);
    if (SUCCEEDED(hr) && g_overlay && g_overlay->active_api_ == GraphicsAPI::DX9) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return hr;
}

void StarOverlay::hook_dx9()
{
    if (orig_dx9_present_ && orig_dx9_reset_) { dx9_hooked_ = true; return; }
    HMODULE d3d9_dll = GetModuleHandleA("d3d9.dll");
    if (!d3d9_dll) d3d9_dll = LoadLibraryA("d3d9.dll");
    if (!d3d9_dll) return;

    typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
    auto pDirect3DCreate9 = (Direct3DCreate9Fn)GetProcAddress(d3d9_dll, "Direct3DCreate9");
    if (!pDirect3DCreate9) return;

    IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return;

    HWND dummy = CreateWindowExA(0, "STAR_Dummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 4, 4, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dummy) { d3d->Release(); return; }

    D3DPRESENT_PARAMETERS d3dpp{};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = dummy;
    // The tiny framed probe window can have a zero-sized client area.
    // Explicit dimensions keep HAL creation from failing with INVALIDCALL
    // and falling back to a different renderer's Present implementation.
    d3dpp.BackBufferWidth = 1;
    d3dpp.BackBufferHeight = 1;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;

    // We only need the vtable, so try several device flavors (HAL/SW first,
    // NULLREF fallback). XNA/D3D9Ex games share the same vtable layout.
    struct Dx9Attempt { int devtype; DWORD behavior; const char* name; };
    const Dx9Attempt attempts[] = {
        { D3DDEVTYPE_HAL,    D3DCREATE_SOFTWARE_VERTEXPROCESSING, "HAL/SW" },
        { D3DDEVTYPE_HAL,    D3DCREATE_HARDWARE_VERTEXPROCESSING, "HAL/HW" },
        { D3DDEVTYPE_HAL,    D3DCREATE_MIXED_VERTEXPROCESSING,    "HAL/MIXED" },
        { D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "NULLREF/SW" },
        { D3DDEVTYPE_REF,    D3DCREATE_SOFTWARE_VERTEXPROCESSING, "REF/SW" },
    };
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = E_FAIL;
    for (auto& a : attempts) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, (D3DDEVTYPE)a.devtype, dummy, a.behavior, &d3dpp, &device);
        if (SUCCEEDED(hr) && device) {
            break;
        }
    }
    if (FAILED(hr) || !device) {
        DestroyWindow(dummy);
        d3d->Release();
        return;
    }

    void** vt = *(void***)device;
    if (!orig_dx9_present_) {
        MH_STATUS s1 = MH_CreateHook(vt[17], &hooked_DX9Present, (void**)&orig_dx9_present_);
        if (s1 == MH_OK) MH_EnableHook(vt[17]);
    }
    if (!orig_dx9_reset_) {
        MH_STATUS s2 = MH_CreateHook(vt[16], &hooked_DX9Reset,   (void**)&orig_dx9_reset_);
        if (s2 == MH_OK) MH_EnableHook(vt[16]);
    }

    device->Release();
    DestroyWindow(dummy);
    d3d->Release();
    if (orig_dx9_present_) {
        dx9_hooked_ = true;
        if (!api_detected_) { api_detected_ = true; STAR_LOG("DX9 hooked"); }
    }
}

void StarOverlay::on_present_dx9(IDirect3DDevice9* device)
{
    if (!enabled_ || !device) return;
    poll_hotkey();
    note_present();
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::DX9;
        STAR_LOG("Game graphics API: DirectX 9");
    }
    if (mode_ == OverlayMode::External) return;

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (imgui_initialized_ && active_api_ != GraphicsAPI::DX9) return;
    if (imgui_initialized_ && dx9_device_ != device) {
        icons_.release_all([](ImTextureID v) { ((IDirect3DTexture9*)v)->Release(); });
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
        active_api_ = GraphicsAPI::None;
    }
    dx9_device_ = device;

    if (!imgui_initialized_) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        HWND new_hwnd = cp.hFocusWindow;
        if (!new_hwnd) new_hwnd = GetActiveWindow();
        hook_window_for(new_hwnd);

        ImGui::CreateContext();
        if (!ImGui_ImplWin32_Init(hwnd_)) { ImGui::DestroyContext(); return; }
        // hook_window already done via hook_window_for

        if (ImGui_ImplDX9_Init(device)) {
            style_.setup();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::DX9;
            STAR_LOG("ImGui ready (DX9) hwnd=%p", hwnd_);
        } else {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            STAR_LOG("ImGui DX9 init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::DX9) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        build_frame_ui();

        HRESULT scene_hr = device->BeginScene();
        if (SUCCEEDED(scene_hr)) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }

        maybe_capture_dx9(device);
    }
}

void StarOverlay::on_reset_dx9()
{
    if (mode_ == OverlayMode::External) return;
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (active_api_ == GraphicsAPI::DX9 && imgui_initialized_) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}
