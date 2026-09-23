#ifdef NDEBUG
#undef NDEBUG
#endif
#include "overlay/core/backend_selection.h"
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    const GraphicsAPI apis[] = {GraphicsAPI::DX7, GraphicsAPI::DX8, GraphicsAPI::DX9,
        GraphicsAPI::DX10, GraphicsAPI::DX11, GraphicsAPI::DX12,
        GraphicsAPI::OpenGL, GraphicsAPI::Vulkan, GraphicsAPI::GDI};
    for (auto selected : apis) {
        BackendSelection selection;
        assert(!selection.accept(GraphicsAPI::None, 1, 0));
        assert(!selection.accept(selected, 0, 0));
        if (selected == GraphicsAPI::GDI) {
            assert(!selection.accept(selected, 1, 0));
            assert(!selection.accept(selected, 1, 1000));
        }
        assert(selection.accept(selected, 1, 2000));
        // Unrelated presents during initialization, resize, device loss, or
        // external fallback must never change the process's game API.
        for (int recovery = 0; recovery < 4; ++recovery) {
            for (auto other : apis)
                assert(selection.accept(other, 2, 3000 + recovery) == (other == selected));
            assert(selection == selected);
        }
    }

    BackendSelection startup;
    assert(!startup.accept(GraphicsAPI::GDI, 1, 100)); // splash painting
    assert(!startup.accept(GraphicsAPI::GDI, 1, 1000));
    assert(startup.accept(GraphicsAPI::Vulkan, 1, 1200));
    assert(!startup.accept(GraphicsAPI::GDI, 1, 5000));

    BackendSelection sporadic;
    assert(!sporadic.accept(GraphicsAPI::GDI, 1, 0));
    assert(!sporadic.accept(GraphicsAPI::GDI, 1, 5000)); // isolated blits don't accumulate
    assert(!sporadic.accept(GraphicsAPI::GDI, 2, 5500)); // different window starts over
    assert(!sporadic.accept(GraphicsAPI::GDI, 2, 6500));
    assert(sporadic.accept(GraphicsAPI::GDI, 2, 7500));

    BackendSelection concurrent;
    std::mutex render_mutex;
    std::vector<std::thread> contenders;
    std::atomic<int> winners{0};
    for (auto api : apis) {
        contenders.emplace_back([&, api] {
            std::lock_guard<std::mutex> lock(render_mutex);
            if (concurrent.accept(api, 1, 0)) ++winners;
        });
    }
    for (auto& thread : contenders) thread.join();
    assert(winners == 1);
    assert(concurrent != GraphicsAPI::None);
}
