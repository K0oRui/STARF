#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/callbacks.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d12.h>
#include <cmath>
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <vulkan/vulkan.h>
#include <cctype>
#include <ctime>
#include <algorithm>

void StarOverlay::hook_dx11()
{
    if (orig_present_ && orig_resize_) { dx11_hooked_ = true; return; }

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "STAR_Dummy";
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0,"STAR_Dummy","",WS_OVERLAPPEDWINDOW,0,0,4,4,
                                 nullptr,nullptr,wc.hInstance,nullptr);
    if (!dummy) { STAR_LOG("DX11 hook: dummy window failed"); return; }

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
        STAR_LOG("DX11 hook: D3D11CreateDeviceAndSwapChain failed hr=0x%08x", (unsigned)hr);
        DestroyWindow(dummy);
        return;
    }

    void** vt = *(void***)dsc;
    MH_STATUS s1 = orig_present_ ? MH_OK : MH_CreateHook(vt[8],  &hooked_Present,       (void**)&orig_present_);
    if (s1 == MH_OK) MH_EnableHook(vt[8]); else STAR_LOG("DX11 hook: Present MH=%d", (int)s1);
    MH_STATUS s2 = orig_resize_ ? MH_OK : MH_CreateHook(vt[13], &hooked_ResizeBuffers, (void**)&orig_resize_);
    if (s2 == MH_OK) MH_EnableHook(vt[13]); else STAR_LOG("DX11 hook: ResizeBuffers MH=%d", (int)s2);

    IDXGISwapChain1* dsc1 = nullptr;
    if (SUCCEEDED(dsc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dsc1))) {
        void** vt1 = *(void***)dsc1;
        if (!orig_present1_) {
            MH_STATUS s3 = MH_CreateHook(vt1[22], &hooked_Present1, (void**)&orig_present1_);
            if (s3 == MH_OK) {
                MH_EnableHook(vt1[22]);
                STAR_LOG("DX11 Present1 hooked");
            } else {
                STAR_LOG("DX11 hook: Present1 MH=%d", (int)s3);
            }
        }
        dsc1->Release();
    }

    dsc->Release(); ddev->Release(); DestroyWindow(dummy);
    if (orig_present_) { dx11_hooked_ = true; STAR_LOG("DX11 hooked"); }

#ifdef _WIN64
    hook_dx12_ecl();
#endif
}

void StarOverlay::init_imgui(IDXGISwapChain* chain)
{
    if (imgui_initialized_) return;
    HRESULT gdhr = chain->GetDevice(__uuidof(ID3D11Device),(void**)&device_);
    if (FAILED(gdhr)) { STAR_LOG("init_imgui DX11: GetDevice failed hr=0x%08x", (unsigned)gdhr); return; }
    device_->GetImmediateContext(&context_);
    DXGI_SWAP_CHAIN_DESC sd{}; chain->GetDesc(&sd);
    hook_window_for(sd.OutputWindow);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    style_.setup();

    ImGui_ImplWin32_Init(hwnd_);
    if (!ImGui_ImplDX11_Init(device_, context_)) {
        STAR_LOG("init_imgui DX11: ImGui_ImplDX11_Init FAILED");
        return;
    }
    imgui_initialized_ = true;
    active_api_ = GraphicsAPI::DX11;
    STAR_LOG("ImGui ready (DX11) hwnd=%p buffers=%u", hwnd_, (unsigned)sd.BufferCount);
}

void StarOverlay::cleanup_rtv()
{
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void StarOverlay::render_frame(IDXGISwapChain* chain)
{

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!imgui_initialized_) return;

    if (!rtv_) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            device_->CreateRenderTargetView(bb, nullptr, &rtv_);
            bb->Release();
        }
    }
    if (!rtv_) return;

    ID3D11RenderTargetView* prev_rtv = nullptr;
    ID3D11DepthStencilView* prev_dsv = nullptr;
    context_->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

    context_->OMSetRenderTargets(1, &rtv_, nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    context_->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();

    maybe_capture_dx11(chain);
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

    // Game API detection (once): DXGI present + D3D11 device = DX11, else DX12.
    if (game_api_ == GraphicsAPI::None) {
        ID3D11Device* probe = nullptr;
        if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device), (void**)&probe))) {
            probe->Release();
            game_api_ = GraphicsAPI::DX11;
            STAR_LOG("Game graphics API: DirectX 11");
        } else {
            game_api_ = GraphicsAPI::DX12;
            STAR_LOG("Game graphics API: DirectX 12");
        }
    }
    // External mode only sniffs (for the label + input); all drawing lives
    // in the external window. Hook rendering stays off entirely.
    if (mode_ == OverlayMode::External) return;

    if (!imgui_initialized_) {
        static bool logged_attempt = false;
        if (!logged_attempt) { logged_attempt = true; STAR_LOG("on_present: attempting imgui init"); }
        ID3D11Device* d3d11_device = nullptr;
        if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11_device))) {
            d3d11_device->Release();
            init_imgui(chain);
        } else {
            static bool logged_dx11_fail = false;
            if (!logged_dx11_fail) { logged_dx11_fail = true; STAR_LOG("on_present: DX11 device not found, using DX12 path"); }
#ifdef _WIN64
            if (!orig_execute_command_lists_) {
                hook_dx12_ecl();
                return;
            }
            if (g_dx12_captured_queue_) {
                ID3D12Device* d3d12_device = nullptr;
                auto* queue = (ID3D12CommandQueue*)g_dx12_captured_queue_;
                if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12_device))) {
                    init_imgui_dx12(chain, d3d12_device, g_dx12_captured_queue_);
                    d3d12_device->Release();
                } else {
                    static bool logged_queue_fail = false;
                    if (!logged_queue_fail) { logged_queue_fail = true; STAR_LOG("on_present: DX12 queue->GetDevice failed"); }
                }
            }
#endif
        }
    }

    if (imgui_initialized_) {
        if (active_api_ == GraphicsAPI::DX11) {
            render_frame(chain);
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
#ifdef _WIN64
    } else if (active_api_ == GraphicsAPI::DX12) {
        cleanup_dx12();
        imgui_initialized_ = false;
#endif
    }
}

