#include "overlay/overlay_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>

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