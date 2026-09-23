// DX7 device setup for backend_ownership_smoke. Own TU: d3d.h pulls
// d3dtypes.h, whose enums clash with both d3d9.h (main TU) and the bundled
// d3d8.h (dx8 TU). Opaque void* API across.
#define WIN32_LEAN_AND_MEAN
#define INITGUID // define our own IID_IDirectDraw7/... instead of linking dxguid
#include <windows.h>
#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <d3d.h>

bool legacy_create_dx7(HWND window, void** device)
{
    // No ddraw.lib guarantee either: resolve by hand like the backends do.
    HMODULE lib = LoadLibraryW(L"ddraw.dll");
    using DDCreateFn = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
    auto create = lib ? (DDCreateFn)GetProcAddress(lib, "DirectDrawCreateEx") : nullptr;
    IDirectDraw7* dd = nullptr;
    HRESULT hr = create ? create(nullptr, (void**)&dd, IID_IDirectDraw7, nullptr) : E_FAIL;
    if (FAILED(hr) || !dd)
        return false;
    IDirect3DDevice7* dev = nullptr;
    if (SUCCEEDED(dd->SetCooperativeLevel(window, DDSCL_NORMAL))) {
        IDirect3D7* d3d = nullptr;
        if (SUCCEEDED(dd->QueryInterface(IID_IDirect3D7, (void**)&d3d))) {
            // Offscreen 3DDEVICE surface (same shape as the hook's own probe):
            // no primary/clipper dance needed in windowed mode, EndScene still fires.
            DDSURFACEDESC2 ddsd{};
            ddsd.dwSize = sizeof(ddsd);
            ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            ddsd.dwWidth = 640;
            ddsd.dwHeight = 480;
            ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
            IDirectDrawSurface7* surf = nullptr;
            if (SUCCEEDED(dd->CreateSurface(&ddsd, &surf, nullptr))) {
                const IID* guids[] = {&IID_IDirect3DHALDevice, &IID_IDirect3DTnLHalDevice, &IID_IDirect3DRGBDevice};
                for (auto guid : guids) {
                    if (SUCCEEDED(d3d->CreateDevice(*guid, surf, &dev)) && dev) break;
                }
                surf->Release();
            }
            d3d->Release();
        }
    }
    dd->Release();
    if (!dev) return false; // no usable rasterizer -> caller skips with 77
    *device = dev;
    return true;
}

void legacy_end_scene_dx7(void* device)
{
    // Routes through hooked_DX7EndScene (vtable slot 6, patched at hook time).
    auto* dev = (IDirect3DDevice7*)device;
    if (SUCCEEDED(dev->BeginScene())) dev->EndScene();
}

void legacy_release_dx7(void* device)
{
    ((IUnknown*)device)->Release();
}
