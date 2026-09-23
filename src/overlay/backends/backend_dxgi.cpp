#include "overlay/overlay_internal.h"
#include <MinHook.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>

void StarOverlay::hook_dxgi_early()
{
#ifdef _WIN64
    // Lightweight early-boot path: capture the DX12 presenting queue without
    // the dummy window + D3D11 HARDWARE device that hook_dxgi() needs for
    // Present/ResizeBuffers vtables. Never force-load dxgi.dll here; if the
    // game hasn't pulled it in yet, the full hook at SteamAPI_Init time plus
    // the retry thread (ensure_hooks) will cover it.
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) return;

    auto try_hook_factory = [&](const char* proc, REFIID iid) {
        using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
        auto create = (CreateFn)GetProcAddress(dxgi, proc);
        IDXGIFactory* factory = nullptr;
        if (create && SUCCEEDED(create(iid, (void**)&factory)) && factory) {
            hook_dx12_factory(factory);
            factory->Release();
            return true;
        }
        return false;
    };
    if (try_hook_factory("CreateDXGIFactory1", __uuidof(IDXGIFactory1))) return;
    // Fallback for downlevel dxgi without CreateDXGIFactory1 export.
    try_hook_factory("CreateDXGIFactory", __uuidof(IDXGIFactory));
#endif
}

void StarOverlay::hook_dxgi()
{
    static std::mutex install_mutex;
    std::lock_guard<std::mutex> install_lock(install_mutex);
    if (orig_present_ && orig_resize_) { dxgi_hooked_ = true; return; }

    // "STAR_Dummy" is registered once in init; re-registering always fails.
    HWND dummy = CreateWindowExA(0,"STAR_Dummy","",WS_OVERLAPPEDWINDOW,0,0,4,4,
                                 nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
    if (!dummy) { STAR_LOG("DXGI: dummy window failed"); return; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 4;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* ddev = nullptr; IDXGISwapChain* dsc = nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&sd,&dsc,&ddev,&fl,nullptr);
    if (FAILED(hr)||!dsc) {
        STAR_LOG("DXGI: D3D11CreateDeviceAndSwapChain failed");
        DestroyWindow(dummy);
        return;
    }

    void** vt = *(void***)dsc;
    auto hook_vt = [](void* target, void* detour, void** orig, const char* name) {
        if (*orig) return;
        if (MH_CreateHook(target, detour, orig) == MH_OK) MH_EnableHook(target);
        else STAR_LOG("DXGI: %s hook failed", name);
    };
    hook_vt(vt[8], &hooked_Present, (void**)&orig_present_, "Present");
    hook_vt(vt[13], &hooked_ResizeBuffers, (void**)&orig_resize_, "ResizeBuffers");

    IDXGISwapChain1* dsc1 = nullptr;
    if (SUCCEEDED(dsc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dsc1))) {
        hook_vt((*(void***)dsc1)[22], &hooked_Present1, (void**)&orig_present1_, "Present1");
        dsc1->Release();
    }

#ifdef _WIN64
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(dsc->GetParent(IID_PPV_ARGS(&factory)))) {
        hook_dx12_factory(factory);
        factory->Release();
    }
#endif
    IDXGISwapChain3* dsc3 = nullptr;
    if (SUCCEEDED(dsc->QueryInterface(IID_PPV_ARGS(&dsc3)))) {
        hook_vt((*(void***)dsc3)[39], &hooked_ResizeBuffers1, (void**)&orig_resize1_, "ResizeBuffers1");
        dsc3->Release();
    }
    dsc->Release(); ddev->Release(); DestroyWindow(dummy);
    if (orig_present_) {
        dxgi_hooked_ = true;
        STAR_LOG("DXGI hooked");
    }

}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present(IDXGISwapChain* sc, UINT si, UINT fl)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->on_present(sc, si, fl);
    }
    return g_overlay && g_overlay->orig_present_ ? g_overlay->orig_present_(sc, si, fl) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present1(
    IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->on_present(sc, si, fl);
    }
    return g_overlay && g_overlay->orig_present1_ ? g_overlay->orig_present1_(sc, si, fl, pp) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_ResizeBuffers(
    IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    if (g_overlay) g_overlay->on_resize_buffers(sc, bc, w, h, fmt, fl);
    return g_overlay ? g_overlay->orig_resize_(sc, bc, w, h, fmt, fl) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_ResizeBuffers1(IDXGISwapChain3* chain,
    UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
    const UINT* masks, IUnknown* const* queues)
{
    auto* o = g_overlay;
    if (!o || !o->orig_resize1_) return E_FAIL;
    o->on_resize_buffers(chain, count, width, height, format, flags);
    HRESULT hr = o->orig_resize1_(chain, count, width, height, format, flags, masks, queues);
#ifdef _WIN64
    if (SUCCEEDED(hr) && queues) o->update_dx12_queue(chain, queues);
#endif
    return hr;
}

void StarOverlay::on_present(IDXGISwapChain* chain, UINT si, UINT fl)
{
    STAR_UNREFERENCED(si); STAR_UNREFERENCED(fl);
    if (!enabled_ || !backend_mine({GraphicsAPI::DX10, GraphicsAPI::DX11, GraphicsAPI::DX12})) return;
    // Detect from the presenting device, never from loaded DLLs or hook setup.
    GraphicsAPI this_api = GraphicsAPI::None;
    {
        ID3D10Device* probe10 = nullptr;
        if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D10Device), (void**)&probe10))) {
            probe10->Release();
            this_api = GraphicsAPI::DX10;
        } else {
            ID3D11Device* probe = nullptr;
            if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device), (void**)&probe))) {
                probe->Release();
                this_api = GraphicsAPI::DX11;
            } else {
#ifdef _WIN64
                ID3D12Device* probe12 = nullptr;
                if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D12Device), (void**)&probe12))) {
                    probe12->Release();
                    this_api = GraphicsAPI::DX12;
                }
#endif
            }
        }
    }
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    DXGI_SWAP_CHAIN_DESC sd{};
    if (FAILED(chain->GetDesc(&sd)) || !accept_backend(this_api, sd.OutputWindow)) return;
    note_present();
    if (mode_ == OverlayMode::External) return;
    if (migrate_if_tiny_frame((int)sd.BufferDesc.Width, (int)sd.BufferDesc.Height)) return;
    if (imgui_initialized_ && active_api_ != this_api) return;
    if (dxgi_chain_ && dxgi_chain_ != chain) shutdown_renderer();
    dxgi_chain_ = chain;
    hook_window_for(sd.OutputWindow);
    poll_hotkey();

    if (!imgui_initialized_) {
        if (this_api == GraphicsAPI::DX11) {
            init_imgui(chain);
        } else if (this_api == GraphicsAPI::DX10) {
            ID3D10Device* d3d10_device = nullptr;
            if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10_device))) {
                init_imgui_dx10(chain, d3d10_device);
                d3d10_device->Release();
            } else {
                // DX10 device not found on this swapchain.
            }
        } else {
#ifdef _WIN64
            try_init_dx12(chain);
#endif
        }
    }

    lock.unlock(); // Each submission rechecks ownership under render_mutex_.
    if (this_api == GraphicsAPI::DX11) render_frame(chain);
    else if (this_api == GraphicsAPI::DX10) render_frame_dx10(chain);
#ifdef _WIN64
    else if (this_api == GraphicsAPI::DX12) {
        maybe_capture_dx12(chain);
        render_frame_dx12(chain);
    }
#endif
}

void StarOverlay::on_resize_buffers(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    STAR_UNREFERENCED(sc); STAR_UNREFERENCED(bc); STAR_UNREFERENCED(w);
    STAR_UNREFERENCED(h);  STAR_UNREFERENCED(fmt); STAR_UNREFERENCED(fl);
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!enabled_ || mode_ == OverlayMode::External || sc != dxgi_chain_) return;
    if (active_api_ == GraphicsAPI::DX11) cleanup_rtv();
    else if (active_api_ == GraphicsAPI::DX10) cleanup_dx10_rtv();
#ifdef _WIN64
    else if (active_api_ == GraphicsAPI::DX12) shutdown_renderer();
#endif
}
