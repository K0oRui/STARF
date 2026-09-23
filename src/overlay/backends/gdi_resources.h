#pragma once
#include "overlay/ui/draw_snapshot.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace star_gdi {
using Microsoft::WRL::ComPtr;

// A selected bitmap must be removed from its DC before it is deleted.
class DibSurface {
public:
    DibSurface() = default;
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;
    ~DibSurface() { reset(); }

    void reset() {
        if (dc_ && old_) SelectObject(dc_, old_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
    }

    bool create(int w, int h) {
        reset();
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_) bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (bitmap_ && bits_) old_ = SelectObject(dc_, bitmap_);
        if (!old_ || old_ == HGDI_ERROR) {
            old_ = nullptr;
            reset();
            return false;
        }
        return true;
    }

    HDC dc() const { return dc_; }
    const void* bits() const { return bits_; }
    void* bits() { return bits_; }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
};

struct Resources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> render_texture;
    ComPtr<ID3D11RenderTargetView> render_view;
    ComPtr<ID3D11Texture2D> staging_texture;
    DibSurface pixels;
    DrawSnapshot draw_snapshot;
    int width = 0, height = 0;

    void reset_surfaces() {
        draw_snapshot.clear();
        render_view.Reset();
        staging_texture.Reset();
        render_texture.Reset();
        pixels.reset();
        width = height = 0;
    }

    void reset() {
        reset_surfaces();
        context.Reset();
        device.Reset();
    }
};
} // namespace star_gdi
