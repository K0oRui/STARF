#include "overlay/overlay_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include <MinHook.h>
#include <d3d10.h>
#include <d3d11.h>
#include <set>

void StarOverlay::hook_dxgi()
{
    if (orig_present_ && orig_resize_) { dxgi_hooked_ = true; return; }

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "STAR_Dummy";
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0,"STAR_Dummy","",WS_OVERLAPPEDWINDOW,0,0,4,4,
                                 nullptr,nullptr,wc.hInstance,nullptr);
    if (!dummy) { STAR_LOG("DXGI hook: dummy window failed"); return; }

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
        STAR_LOG("DXGI hook: D3D11CreateDeviceAndSwapChain failed hr=0x%08x", (unsigned)hr);
        DestroyWindow(dummy);
        return;
    }

    void** vt = *(void***)dsc;
    MH_STATUS s1 = orig_present_ ? MH_OK : MH_CreateHook(vt[8],  &hooked_Present,       (void**)&orig_present_);
    if (s1 == MH_OK) MH_EnableHook(vt[8]); else STAR_LOG("DXGI hook: Present MH=%d", (int)s1);
    MH_STATUS s2 = orig_resize_ ? MH_OK : MH_CreateHook(vt[13], &hooked_ResizeBuffers, (void**)&orig_resize_);
    if (s2 == MH_OK) MH_EnableHook(vt[13]); else STAR_LOG("DXGI hook: ResizeBuffers MH=%d", (int)s2);

    IDXGISwapChain1* dsc1 = nullptr;
    if (SUCCEEDED(dsc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dsc1))) {
        void** vt1 = *(void***)dsc1;
        if (!orig_present1_) {
            MH_STATUS s3 = MH_CreateHook(vt1[22], &hooked_Present1, (void**)&orig_present1_);
            if (s3 == MH_OK) {
                MH_EnableHook(vt1[22]);
                STAR_LOG("DXGI Present1 hooked");
            } else {
                STAR_LOG("DXGI hook: Present1 MH=%d", (int)s3);
            }
        }
        dsc1->Release();
    }

    dsc->Release(); ddev->Release(); DestroyWindow(dummy);
    if (orig_present_) { dxgi_hooked_ = true; STAR_LOG("DXGI hooked"); }

#ifdef _WIN64
    hook_dx12_ecl();
#endif
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present(IDXGISwapChain* sc, UINT si, UINT fl)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
        g_overlay->on_present(sc, si, fl);
    }
    return g_overlay && g_overlay->orig_present_ ? g_overlay->orig_present_(sc, si, fl) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present1(
    IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
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

void StarOverlay::on_present(IDXGISwapChain* chain, UINT si, UINT fl)
{
    STAR_UNREFERENCED(si); STAR_UNREFERENCED(fl);
    if (!enabled_) return;
    // Window recreated (mode switch / multi-window): re-hook so input keeps working.
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(chain->GetDesc(&sd))) hook_window_for(sd.OutputWindow);
    }

    // Game API detection: DXGI present + D3D10 device = DX10, D3D11 device =
    // DX11, else DX12. Re-detected every present so a title that presents
    // multiple swapchains (D3D11 splash -> D3D10 game) lands on the visible one.
    //
    // Probe DX10 FIRST: on Windows 8+ the D3D10 runtime is layered on D3D11, so
    // a swapchain created by D3D10CreateDeviceAndSwapChain answers BOTH
    // GetDevice(ID3D10Device) and GetDevice(ID3D11Device). A swapchain created
    // by D3D11CreateDeviceAndSwapChain answers only ID3D11Device. Checking DX11
    // first therefore mislabels every genuine DX10 title as DX11.
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
                this_api = GraphicsAPI::DX12;
            }
        }
    }
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = this_api;
        switch (this_api) {
        case GraphicsAPI::DX10: STAR_LOG("Game graphics API: DirectX 10"); break;
        case GraphicsAPI::DX11: STAR_LOG("Game graphics API: DirectX 11"); break;
        default:                STAR_LOG("Game graphics API: DirectX 12"); break;
        }
    }
    // Ground-truth probe: the game may present multiple swapchains (D3D10
    // visible window + D3D11 splash/helper). Log EVERY chain once so we can
    // tell which API the visible (foreground) window truly presents.
    {
        static std::mutex probe_mutex;
        std::lock_guard<std::mutex> probe_lock(probe_mutex);
        static std::set<IDXGISwapChain*> probed{};
        if (probed.insert(chain).second) {
            DXGI_SWAP_CHAIN_DESC sd{};
            bool have_desc = SUCCEEDED(chain->GetDesc(&sd));
            STAR_LOG("chain probe: %p api=%d hwnd=%p fg=%d%s", (void*)chain,
                     (int)this_api, (void*)(have_desc ? sd.OutputWindow : nullptr),
                     have_desc && GetForegroundWindow() == sd.OutputWindow ? 1 : 0,
                     have_desc ? "" : " (GetDesc failed)");
        }
    }
    // External mode only sniffs (for the label + input); all drawing lives
    // in the external window. Hook rendering stays off entirely.
    if (mode_ == OverlayMode::External) return;

    // The game switched to a different API on a different, now-foreground
    // window (e.g. D3D11 splash -> D3D10 main). Re-init so the overlay draws
    // on the visible swapchain. Same-window presents never switch (no
    // flip-flop when a game presents two swapchains on one window).
    if (imgui_initialized_ && active_api_ != this_api) {
        DXGI_SWAP_CHAIN_DESC sd{};
        HWND new_hwnd = nullptr;
        if (SUCCEEDED(chain->GetDesc(&sd))) new_hwnd = sd.OutputWindow;
        if (new_hwnd && new_hwnd != hwnd_ && GetForegroundWindow() == new_hwnd) {
            STAR_LOG("Game switched graphics API - re-initializing overlay");
            if (active_api_ == GraphicsAPI::DX11) {
                ImGui_ImplDX11_Shutdown();
                cleanup_rtv();
            } else if (active_api_ == GraphicsAPI::DX10) {
                ImGui_ImplDX10_Shutdown();
                cleanup_dx10_rtv();
            }
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            imgui_initialized_ = false;
            active_api_ = GraphicsAPI::None;
            if (context_) { context_->Release(); context_ = nullptr; }
            if (device_)  { device_->Release();  device_  = nullptr; }
            if (dx10_device_) { dx10_device_->Release(); dx10_device_ = nullptr; }
        }
    }

    if (!imgui_initialized_) {
        std::unique_lock<std::mutex> init_lock(render_mutex_, std::try_to_lock);
        if (!init_lock.owns_lock() || imgui_initialized_) return;
        static bool logged_attempt = false;
        if (!logged_attempt) { logged_attempt = true; STAR_LOG("on_present: attempting imgui init"); }
        if (this_api == GraphicsAPI::DX11) {
            init_imgui(chain);
        } else if (this_api == GraphicsAPI::DX10) {
            ID3D10Device* d3d10_device = nullptr;
            if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10_device))) {
                init_imgui_dx10(chain, d3d10_device);
                d3d10_device->Release();
            } else {
                static bool logged_dx10_fail = false;
                if (!logged_dx10_fail) { logged_dx10_fail = true; STAR_LOG("on_present: DX10 device query failed"); }
            }
        } else {
            static bool logged_dx11_fail = false;
            if (!logged_dx11_fail) { logged_dx11_fail = true; STAR_LOG("on_present: DX11 device not found, using DX12 path"); }
#ifdef _WIN64
            try_init_dx12(chain);
#endif
        }
    }

    if (imgui_initialized_) {
        if (active_api_ == GraphicsAPI::DX11) {
            render_frame(chain);
        } else if (active_api_ == GraphicsAPI::DX10) {
            render_frame_dx10(chain);
#ifdef _WIN64
        } else if (active_api_ == GraphicsAPI::DX12) {
            // Capture first: with dx12_render=false this is the ONLY consumer
            // (render_frame_dx12 early-returns), routing to desktop duplication.
            maybe_capture_dx12(chain);
            render_frame_dx12(chain);
#endif
        }
    }
}

void StarOverlay::on_resize_buffers(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    STAR_UNREFERENCED(sc); STAR_UNREFERENCED(bc); STAR_UNREFERENCED(w);
    STAR_UNREFERENCED(h);  STAR_UNREFERENCED(fmt); STAR_UNREFERENCED(fl);
    // External mode owns its own RTV; a game-chain resize must never touch it.
    if (mode_ == OverlayMode::External) return;
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (active_api_ == GraphicsAPI::DX11) {
        cleanup_rtv();
    } else if (active_api_ == GraphicsAPI::DX10) {
        cleanup_dx10_rtv();
#ifdef _WIN64
    } else if (active_api_ == GraphicsAPI::DX12) {
        if (sc != dx12_chain_) return;
        wait_dx12_idle();
        if (imgui_initialized_) {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        icons_.clear();
        cleanup_dx12();
        imgui_initialized_ = false;
        active_api_ = GraphicsAPI::None;
#endif
    }
}