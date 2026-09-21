#pragma once
#include <atomic>
#include <cstdint>

enum class GraphicsAPI { None, DX7, DX8, DX9, DX10, DX11, DX12, OpenGL, Vulkan, GDI };

// accept() is serialized by render_mutex_. Readers (including the hook retry
// thread) can observe the selection without touching renderer state.
class BackendSelection {
public:
    operator GraphicsAPI() const { return selected_.load(); }

    bool accept(GraphicsAPI api, uintptr_t window, uint64_t now) {
        if (api == GraphicsAPI::None || !window) return false;
        GraphicsAPI selected = selected_.load();
        if (selected != GraphicsAPI::None) return selected == api;
        if (api == GraphicsAPI::GDI) {
            // GDI also paints launch screens. Require sustained drawing before
            // selecting it; a real GPU presentation can win during this grace.
            if (gdi_window_ != window || now - gdi_last_ > 1000) {
                gdi_window_ = window;
                gdi_since_ = now;
                gdi_frames_ = 0;
            }
            gdi_last_ = now;
            if (++gdi_frames_ < 3 || now - gdi_since_ < 2000) return false;
        }
        selected_.store(api);
        return true;
    }

private:
    std::atomic<GraphicsAPI> selected_{GraphicsAPI::None};
    uintptr_t gdi_window_ = 0;
    uint64_t gdi_since_ = 0, gdi_last_ = 0;
    unsigned gdi_frames_ = 0;
};
