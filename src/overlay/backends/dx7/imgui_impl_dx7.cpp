// Native Direct3D 7 renderer. Textures are managed DirectDraw surfaces;
// draw commands use CPU vertex/index arrays, without a newer graphics API.
#define DIRECT3D_VERSION 0x0700
#define DIRECTDRAW_VERSION 0x0700
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "imgui_impl_dx7.h"

namespace {
struct Vertex { float x, y, z; DWORD color; float u, v; };
constexpr DWORD vertex_format = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
struct Backend {
    IDirect3DDevice7* device = nullptr;
    IDirectDraw7* draw = nullptr;
    IDirectDrawSurface7* font = nullptr;
    D3DDEVICEDESC7 caps{};
    DDPIXELFORMAT format{};
    std::vector<Vertex> vertices;
};
Backend* data() {
    return ImGui::GetCurrentContext() ? (Backend*)ImGui::GetIO().BackendRendererUserData : nullptr;
}
HRESULT CALLBACK choose_format(DDPIXELFORMAT* format, void* context) {
    auto* result = (DDPIXELFORMAT*)context;
    if ((format->dwFlags & (DDPF_RGB | DDPF_ALPHAPIXELS)) != (DDPF_RGB | DDPF_ALPHAPIXELS)) return D3DENUMRET_OK;
    if (format->dwRGBBitCount != 16 && format->dwRGBBitCount != 32) return D3DENUMRET_OK;
    if (!format->dwRBitMask || !format->dwGBitMask || !format->dwBBitMask || !format->dwRGBAlphaBitMask) return D3DENUMRET_OK;
    if (format->dwRGBBitCount > result->dwRGBBitCount) *result = *format;
    return D3DENUMRET_OK;
}
DWORD channel(unsigned char value, DWORD mask) {
    if (!mask) return 0;
    unsigned shift = 0;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    return ((DWORD)(((unsigned long long)value * mask + 127) / 255) << shift);
}
DWORD texture_dimension(DWORD size, DWORD maximum, bool pow2) {
    if (pow2) {
        DWORD rounded = 1;
        while (rounded < size && rounded <= maximum / 2) rounded *= 2;
        return std::min(rounded, maximum);
    }
    return std::min(size, maximum);
}
void setup_state(Backend* bd) {
    auto* d = bd->device;
    d->SetRenderState(D3DRENDERSTATE_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    d->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_SPECULARENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    d->SetRenderState(D3DRENDERSTATE_CLIPPING, TRUE);
    d->SetRenderState(D3DRENDERSTATE_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRENDERSTATE_VERTEXBLEND, D3DVBLEND_DISABLE);
    d->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.f;
    d->SetTransform(D3DTRANSFORMSTATE_WORLD, &identity);
    d->SetTransform(D3DTRANSFORMSTATE_VIEW, &identity);
}
}

bool ImGui_ImplDX7_Init(IDirect3DDevice7* device) {
    if (!device || ImGui::GetIO().BackendRendererUserData) return false;
    auto* bd = new Backend;
    IDirectDrawSurface7* target = nullptr;
    IUnknown* owner = nullptr;
    HRESULT hr = device->GetRenderTarget(&target);
    if (SUCCEEDED(hr)) hr = target->GetDDInterface((void**)&owner);
    if (SUCCEEDED(hr)) hr = owner->QueryInterface(IID_IDirectDraw7, (void**)&bd->draw);
    if (owner) owner->Release();
    if (target) target->Release();
    if (SUCCEEDED(hr)) hr = device->GetCaps(&bd->caps);
    if (SUCCEEDED(hr)) hr = device->EnumTextureFormats(choose_format, &bd->format);
    if (FAILED(hr) || !bd->format.dwRGBBitCount) {
        if (bd->draw) bd->draw->Release();
        delete bd;
        return false;
    }
    bd->device = device;
    device->AddRef();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = bd;
    io.BackendRendererName = "imgui_impl_dx7";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    return true;
}

IDirectDrawSurface7* ImGui_ImplDX7_CreateTexture(const unsigned char* rgba, int w, int h) {
    auto* bd = data();
    if (!bd || !rgba || w <= 0 || h <= 0) return nullptr;
    bool pow2 = (bd->caps.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_POW2) != 0;
    DWORD width = texture_dimension((DWORD)w, bd->caps.dwMaxTextureWidth, pow2);
    DWORD height = texture_dimension((DWORD)h, bd->caps.dwMaxTextureHeight, pow2);
    if (bd->caps.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_SQUAREONLY)
        width = height = std::min(std::max(width, height), std::min(bd->caps.dwMaxTextureWidth, bd->caps.dwMaxTextureHeight));
    if (!width || !height) return nullptr;
    DDSURFACEDESC2 desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = width;
    desc.dwHeight = height;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    desc.ddpfPixelFormat = bd->format;
    IDirectDrawSurface7* texture = nullptr;
    if (FAILED(bd->draw->CreateSurface(&desc, &texture, nullptr))) return nullptr;
    DDSURFACEDESC2 lock{};
    lock.dwSize = sizeof(lock);
    if (FAILED(texture->Lock(nullptr, &lock, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
        texture->Release();
        return nullptr;
    }
    unsigned bytes = bd->format.dwRGBBitCount / 8;
    for (DWORD y = 0; y < height; ++y) {
        auto* row = (unsigned char*)lock.lpSurface + (ptrdiff_t)y * lock.lPitch;
        for (DWORD x = 0; x < width; ++x) {
            const auto* p = rgba + (((size_t)y * h / height) * w + (size_t)x * w / width) * 4;
            DWORD pixel = channel(p[0], bd->format.dwRBitMask) | channel(p[1], bd->format.dwGBitMask) |
                channel(p[2], bd->format.dwBBitMask) | channel(p[3], bd->format.dwRGBAlphaBitMask);
            memcpy(row + x * bytes, &pixel, bytes);
        }
    }
    texture->Unlock(nullptr);
    return texture;
}

bool ImGui_ImplDX7_NewFrame() {
    auto* bd = data();
    if (!bd) return false;
    if (bd->font && bd->font->IsLost() == DDERR_SURFACELOST) ImGui_ImplDX7_InvalidateDeviceObjects();
    if (!bd->font) {
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        bd->font = ImGui_ImplDX7_CreateTexture(pixels, w, h);
        ImGui::GetIO().Fonts->SetTexID((ImTextureID)bd->font);
    }
    return bd->font != nullptr;
}

long ImGui_ImplDX7_RenderDrawData(ImDrawData* draw) {
    auto* bd = data();
    if (!bd || !draw || draw->TotalVtxCount == 0 || draw->DisplaySize.x <= 0 || draw->DisplaySize.y <= 0) return S_OK;
    static_assert(sizeof(ImDrawIdx) == sizeof(WORD), "DX7 requires 16-bit indices");
    auto* d = bd->device;
    DWORD block = 0;
    HRESULT hr = d->CreateStateBlock(D3DSBT_ALL, &block);
    if (FAILED(hr)) return hr;
    hr = d->CaptureStateBlock(block);
    if (FAILED(hr)) { d->DeleteStateBlock(block); return hr; }
    D3DMATRIX world{}, view{}, projection{};
    D3DVIEWPORT7 viewport{};
    d->GetTransform(D3DTRANSFORMSTATE_WORLD, &world);
    d->GetTransform(D3DTRANSFORMSTATE_VIEW, &view);
    d->GetTransform(D3DTRANSFORMSTATE_PROJECTION, &projection);
    d->GetViewport(&viewport);
    setup_state(bd);
    for (int n = 0; n < draw->CmdListsCount; ++n) {
        const auto* list = draw->CmdLists[n];
        bd->vertices.resize(list->VtxBuffer.Size);
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const auto& v = list->VtxBuffer[i];
            DWORD c = (DWORD)v.col;
            bd->vertices[i] = { v.pos.x, v.pos.y, 0.f,
                ((c & 0xff00ff00) | ((c & 0xff) << 16) | ((c >> 16) & 0xff)), v.uv.x, v.uv.y };
        }
        for (const auto& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState) setup_state(bd);
                else cmd.UserCallback(list, &cmd);
                continue;
            }
            // D3D7 has no scissor rectangle. A matching viewport/projection
            // clips the geometry without changing its screen position.
            float x0 = std::clamp(cmd.ClipRect.x - draw->DisplayPos.x, 0.f, draw->DisplaySize.x);
            float y0 = std::clamp(cmd.ClipRect.y - draw->DisplayPos.y, 0.f, draw->DisplaySize.y);
            float x1 = std::clamp(cmd.ClipRect.z - draw->DisplayPos.x, 0.f, draw->DisplaySize.x);
            float y1 = std::clamp(cmd.ClipRect.w - draw->DisplayPos.y, 0.f, draw->DisplaySize.y);
            if (x1 <= x0 || y1 <= y0 || cmd.ElemCount == 0) continue;
            D3DVIEWPORT7 clip{ (DWORD)x0, (DWORD)y0, (DWORD)x1 - (DWORD)x0, (DWORD)y1 - (DWORD)y0, 0.f, 1.f };
            if (!clip.dwWidth || !clip.dwHeight) continue;
            float l = draw->DisplayPos.x + clip.dwX + .5f, r = l + clip.dwWidth;
            float t = draw->DisplayPos.y + clip.dwY + .5f, b = t + clip.dwHeight;
            D3DMATRIX matrix{};
            matrix._11 = 2.f / (r - l); matrix._22 = 2.f / (t - b); matrix._33 = .5f;
            matrix._41 = (l + r) / (l - r); matrix._42 = (t + b) / (b - t); matrix._43 = .5f; matrix._44 = 1.f;
            HRESULT call = d->SetViewport(&clip);
            if (SUCCEEDED(call)) call = d->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &matrix);
            if (SUCCEEDED(call)) call = d->SetTexture(0, (IDirectDrawSurface7*)cmd.GetTexID());
            if (SUCCEEDED(call)) call = d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, vertex_format,
                bd->vertices.data() + cmd.VtxOffset, (DWORD)bd->vertices.size() - cmd.VtxOffset,
                (WORD*)list->IdxBuffer.Data + cmd.IdxOffset, cmd.ElemCount, 0);
            if (FAILED(call) && SUCCEEDED(hr)) hr = call;
        }
    }
    d->ApplyStateBlock(block);
    d->DeleteStateBlock(block);
    d->SetTransform(D3DTRANSFORMSTATE_WORLD, &world);
    d->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
    d->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &projection);
    d->SetViewport(&viewport);
    return hr;
}

void ImGui_ImplDX7_InvalidateDeviceObjects() {
    auto* bd = data();
    if (bd && bd->font) { bd->font->Release(); bd->font = nullptr; ImGui::GetIO().Fonts->SetTexID(nullptr); }
}
void ImGui_ImplDX7_Shutdown() {
    auto* bd = data();
    if (!bd) return;
    ImGui_ImplDX7_InvalidateDeviceObjects();
    bd->device->Release();
    bd->draw->Release();
    delete bd;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
}
