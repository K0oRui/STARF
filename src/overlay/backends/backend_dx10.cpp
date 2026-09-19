#include "overlay/overlay_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx10.h"
#include <d3d10.h>

void StarOverlay::init_imgui_dx10(IDXGISwapChain* chain, ID3D10Device* device)
{
    if (imgui_initialized_) return;
    dx10_device_ = device;
    dx10_device_->AddRef();
    DXGI_SWAP_CHAIN_DESC sd{};
    chain->GetDesc(&sd);
    hook_window_for(sd.OutputWindow);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    style_.setup();

    ImGui_ImplWin32_Init(hwnd_);
    if (!ImGui_ImplDX10_Init(dx10_device_)) {
        STAR_LOG("init_imgui DX10: ImGui_ImplDX10_Init FAILED");
        return;
    }
    imgui_initialized_ = true;
    active_api_ = GraphicsAPI::DX10;
    STAR_LOG("ImGui ready (DX10) hwnd=%p buffers=%u", hwnd_, (unsigned)sd.BufferCount);
}

void StarOverlay::cleanup_dx10_rtv()
{
    if (dx10_rtv_) { dx10_rtv_->Release(); dx10_rtv_ = nullptr; }
}

void StarOverlay::render_frame_dx10(IDXGISwapChain* chain)
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!imgui_initialized_ || active_api_ != GraphicsAPI::DX10) return;

    if (!dx10_rtv_) {
        ID3D10Texture2D* bb = nullptr;
        if (SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb))) {
            dx10_device_->CreateRenderTargetView(bb, nullptr, &dx10_rtv_);
            bb->Release();
        }
    }
    if (!dx10_rtv_) return;

    ID3D10RenderTargetView* prev_rtvs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D10DepthStencilView* prev_dsv = nullptr;
    dx10_device_->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, &prev_dsv);

    dx10_device_->OMSetRenderTargets(1, &dx10_rtv_, nullptr);

    ImGui_ImplDX10_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    build_frame_ui();
    ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());

    dx10_device_->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
    for (auto* view : prev_rtvs) if (view) view->Release();
    if (prev_dsv) prev_dsv->Release();

    maybe_capture_dx10(chain);
}