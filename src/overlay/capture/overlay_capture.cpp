#include "overlay/overlay_internal.h"
#include <algorithm>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#include <GL/gl.h>

void StarOverlay::maybe_capture_dx11(IDXGISwapChain* chain)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !device_ || !context_) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC desc{};
    bb->GetDesc(&desc);
    bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra) {
        STAR_LOG("Screenshot: unsupported DX11 format %u", (unsigned)desc.Format);
        bb->Release();
        return;
    }
    ID3D11Texture2D* src = bb;
    ID3D11Texture2D* resolved = nullptr;
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
        rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0;
        if (FAILED(device_->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) {
            STAR_LOG("Screenshot: DX11 resolve target failed");
            bb->Release();
            return;
        }
        context_->ResolveSubresource(resolved, 0, bb, 0, desc.Format);
        src = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
        if (resolved) resolved->Release();
        bb->Release();
        return;
    }
    context_->CopyResource(staging, src);
    D3D11_MAPPED_SUBRESOURCE map{};
    std::string path;
    if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        std::vector<uint8_t> rgba((size_t)desc.Width * desc.Height * 4);
        copy_pixels32(rgba.data(), (size_t)desc.Width * 4, map.pData, map.RowPitch,
                      desc.Width, desc.Height, bgra);
        context_->Unmap(staging, 0);
        path = ScreenshotService::next_path();
        screenshots_.save_async(path, std::move(rgba), (int)desc.Width, (int)desc.Height);
    }
    staging->Release();
    if (resolved) resolved->Release();
    bb->Release();
}

void StarOverlay::maybe_capture_dx10(IDXGISwapChain* chain)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !dx10_device_) return;
    ID3D10Texture2D* bb = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb)) || !bb) return;
    D3D10_TEXTURE2D_DESC desc{};
    bb->GetDesc(&desc);
    bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra) {
        STAR_LOG("Screenshot: unsupported DX10 format %u", (unsigned)desc.Format);
        bb->Release();
        return;
    }
    ID3D10Texture2D* src = bb;
    ID3D10Texture2D* resolved = nullptr;
    if (desc.SampleDesc.Count > 1) {
        D3D10_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
        rd.Usage = D3D10_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0;
        if (FAILED(dx10_device_->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) {
            STAR_LOG("Screenshot: DX10 resolve target failed");
            bb->Release();
            return;
        }
        dx10_device_->ResolveSubresource(resolved, 0, bb, 0, desc.Format);
        src = resolved;
    }
    D3D10_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Usage = D3D10_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D10Texture2D* staging = nullptr;
    if (FAILED(dx10_device_->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
        if (resolved) resolved->Release();
        bb->Release();
        return;
    }
    dx10_device_->CopyResource(staging, src);
    D3D10_MAPPED_TEXTURE2D map{};
    std::string path;
    if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &map))) {
        std::vector<uint8_t> rgba((size_t)desc.Width * desc.Height * 4);
        copy_pixels32(rgba.data(), (size_t)desc.Width * 4, map.pData, map.RowPitch,
                      desc.Width, desc.Height, bgra);
        staging->Unmap(0);
        path = ScreenshotService::next_path();
        screenshots_.save_async(path, std::move(rgba), (int)desc.Width, (int)desc.Height);
    }
    staging->Release();
    if (resolved) resolved->Release();
    bb->Release();
}

void StarOverlay::maybe_capture_dx9(IDirect3DDevice9* device)
{
    if (!screenshots_.consume()) return;
    if (!enabled_ || !device) return;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(device->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC desc{};
    rt->GetDesc(&desc);
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr) && sys) hr = device->GetRenderTargetData(rt, sys);
    if (FAILED(hr) && sys) {
        // Multisampled render target: fall back to the front buffer.
        sys->Release(); sys = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys)
            hr = device->GetFrontBufferData(0, sys);
    }
    rt->Release();
    if (FAILED(hr) || !sys) {
        STAR_LOG("Screenshot: DX9 capture failed hr=0x%08x", (unsigned)hr);
        if (sys) sys->Release();
        return;
    }
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::vector<uint8_t> rgba((size_t)desc.Width * desc.Height * 4);
        copy_pixels32(rgba.data(), (size_t)desc.Width * 4, lr.pBits, lr.Pitch,
                      desc.Width, desc.Height, true);
        sys->UnlockRect();
        std::string path = ScreenshotService::next_path();
        screenshots_.save_async(path, std::move(rgba), (int)desc.Width, (int)desc.Height);
    }
    sys->Release();
}

void StarOverlay::maybe_capture_opengl()
{
    if (!screenshots_.consume()) return;
    if (!enabled_) return;
    int vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 16384 || vp[3] > 16384) return;
    std::vector<uint8_t> px((size_t)vp[2] * vp[3] * 4);
    glReadPixels(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    // GL origin is bottom-left: flip rows for top-down PNG.
    std::vector<uint8_t> rgba(px.size());
    size_t row = (size_t)vp[2] * 4;
    for (int y = 0; y < vp[3]; y++)
        memcpy(rgba.data() + (size_t)y * row, px.data() + (size_t)(vp[3] - 1 - y) * row, row);
    std::string path = ScreenshotService::next_path();
    screenshots_.save_async(path, std::move(rgba), vp[2], vp[3]);
}

void StarOverlay::capture_desktop_duplication()
{
    // Zero game-state interaction: own D3D11 device + DXGI desktop duplication.
    // For titles whose buffers fault on any foreign access (ACEVO), and any
    // exclusive-fullscreen game. Captures the game window only: the desktop
    // frame is cropped to the game client area (full frame if the window
    // cannot be determined, so a screenshot never silently dies).
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory)
        return;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(factory->EnumAdapters1(0, &adapter)) || !adapter) {
        factory->Release();
        return;
    }
    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(0, &output)) || !output) {
        adapter->Release();
        factory->Release();
        return;
    }
    ID3D11Device* ddev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter*)adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &ddev, &fl, &ctx);
    if (FAILED(hr) || !ddev) {
        output->Release();
        adapter->Release();
        factory->Release();
        return;
    }
    auto release_dev = [&] { if (ctx) ctx->Release(); if (ddev) ddev->Release(); };
    IDXGIOutput1* out1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    // Duplication frames are in output coordinates; the crop needs the origin
    // (non-primary outputs don't start at 0,0).
    DXGI_OUTPUT_DESC out_desc{};
    if (SUCCEEDED(hr) && out1) out1->GetDesc(&out_desc);
    output->Release();
    adapter->Release();
    factory->Release();
    if (FAILED(hr) || !out1) {
        release_dev();
        return;
    }
    IDXGIOutputDuplication* dup = nullptr;
    hr = out1->DuplicateOutput(ddev, &dup);
    out1->Release();
    if (FAILED(hr) || !dup) {
        STAR_LOG_ONCE("Screenshot: desktop duplication unavailable hr=0x%08x", (unsigned)hr);
        release_dev();
        return;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* res = nullptr;
    for (int i = 0; i < 3 && !res; i++) {
        if (dup->AcquireNextFrame(400, &info, &res) != S_OK)
            res = nullptr;
    }
    if (!res) {
        dup->Release();
        release_dev();
        return;
    }
    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr) || !tex) {
        dup->ReleaseFrame();
        dup->Release();
        release_dev();
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra) {
        STAR_LOG("Screenshot: unsupported desktop format %u", (unsigned)desc.Format);
        tex->Release();
        dup->ReleaseFrame();
        dup->Release();
        release_dev();
        return;
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(ddev->CreateTexture2D(&sd, nullptr, &staging)) && staging) {
        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            // Crop to the game client area (output-relative). No overlap
            // (window on another output, minimized resolution race) falls
            // back to the full frame rather than failing the screenshot.
            RECT crop{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };
            HWND game = IsWindow(ext_game_hwnd_) ? ext_game_hwnd_ : find_game_window();
            if (game) {
                RECT client{};
                if (GetClientRect(game, &client) && client.right > 0 && client.bottom > 0) {
                    POINT org{ client.left, client.top };
                    if (ClientToScreen(game, &org)) {
                        LONG ox = out_desc.DesktopCoordinates.left;
                        LONG oy = out_desc.DesktopCoordinates.top;
                        LONG l = std::max<LONG>(org.x, ox);
                        LONG t = std::max<LONG>(org.y, oy);
                        LONG r = std::min<LONG>(org.x + client.right, ox + (LONG)desc.Width);
                        LONG b = std::min<LONG>(org.y + client.bottom, oy + (LONG)desc.Height);
                        if (r > l && b > t)
                            crop = { l - ox, t - oy, r - ox, b - oy };
                    }
                }
            }
            int cw = (int)(crop.right - crop.left), ch = (int)(crop.bottom - crop.top);
            std::vector<uint8_t> rgba((size_t)cw * ch * 4);
            uint64_t bright = 0;
            for (int y = 0; y < ch; y++) {
                const uint8_t* s = (const uint8_t*)map.pData + (size_t)(crop.top + y) * map.RowPitch +
                    (size_t)crop.left * 4;
                uint8_t* d = rgba.data() + (size_t)y * cw * 4;
                if (bgra) {
                    for (int x = 0; x < cw; x++) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        bright += (uint64_t)d[0] + d[1] + d[2];
                        s += 4; d += 4;
                    }
                } else {
                    memcpy(d, s, (size_t)cw * 4);
                    for (int i = 0; i < cw * 4; i += 4)
                        bright += (uint64_t)d[i] + d[i + 1] + d[i + 2];
                }
            }
            ctx->Unmap(staging, 0);
            std::string path = ScreenshotService::next_path();
            // All-black frame = exclusive/independent-flip fullscreen
            // bypassing DWM; tell the user how to fix it instead of silence.
            double mean = (double)bright / ((double)cw * ch * 3.0);
            bool dark = mean < 4.0;
            if (dark) STAR_LOG("Screenshot looks black (exclusive fullscreen?)");
            screenshots_.save_async(path, std::move(rgba), cw, ch, dark);
        }
        staging->Release();
    }
    tex->Release();
    dup->ReleaseFrame();
    dup->Release();
    release_dev();
}

