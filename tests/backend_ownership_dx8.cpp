// DX8 device setup for backend_ownership_smoke. Own TU: d3d8.h defines
// D3DPRESENT_PARAMETERS/D3DFORMAT/..., which clash with d3d9.h (main TU)
// and with d3dtypes.h from d3d.h (dx7 TU). Opaque void* API across.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dx8/d3d8.h"

bool legacy_create_dx8(HWND window, void** device, void** params)
{
    // No d3d8.lib on modern SDKs (and the DLL avoids it too): resolve by hand.
    // The call still routes through hooked_Direct3DCreate8: chains CreateDevice.
    HMODULE lib = LoadLibraryW(L"d3d8.dll");
    using Create8Fn = IDirect3D8*(WINAPI*)(UINT);
    auto create = lib ? (Create8Fn)GetProcAddress(lib, "Direct3DCreate8") : nullptr;
    auto d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return false;
    auto* pp = new D3DPRESENT_PARAMETERS{};
    pp->BackBufferWidth = 640;
    pp->BackBufferHeight = 480;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = window;
    pp->Windowed = TRUE;
    IDirect3DDevice8* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, pp, &dev);
    d3d->Release();
    if (FAILED(hr) || !dev) {
        delete pp;
        return false; // no HAL (headless CI) -> caller skips with 77
    }
    *device = dev;
    *params = pp;
    return true;
}

void legacy_present_dx8(void* device)
{
    // Routes through hooked_DX8Present; null window falls back to
    // hFocusWindow inside the backend, same as the dx9 test path.
    ((IDirect3DDevice8*)device)->Present(nullptr, nullptr, nullptr, nullptr);
}

bool legacy_reset_dx8(void* device, void* params)
{
    return SUCCEEDED(((IDirect3DDevice8*)device)->Reset((D3DPRESENT_PARAMETERS*)params));
}

void legacy_release_dx8(void* device, void* params)
{
    ((IUnknown*)device)->Release();
    delete (D3DPRESENT_PARAMETERS*)params;
}
