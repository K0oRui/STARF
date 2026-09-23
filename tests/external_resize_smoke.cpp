// Exercise real external textures/readback while the tracked window changes size.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <MinHook.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

static HWND test_window;
static HWND WINAPI test_foreground() { return test_window; }

int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto dir = std::filesystem::absolute(argv[2]) /
        ("external-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(dir / "STAR");
    std::ofstream(dir / "STAR" / "overlay.star") << "enabled=true\nmode=external\nshow_fps=true\n";
    std::ofstream(dir / "STAR" / "steam_appid.txt") << "480\n";
    SetEnvironmentVariableW(L"APPDATA", dir.c_str());
    const auto dll = dir / std::filesystem::path(argv[1]).filename();
    std::filesystem::copy_file(argv[1], dll);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"STAR_External_Resize_Test";
    assert(RegisterClassW(&wc));
    // Shown without activating: visible for API detection, never steals focus.
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"STAR resize test", WS_POPUP,
        -30000, -30000, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    assert(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    // Keep this GPU/readback test offscreen and independent of desktop focus.
    test_window = window;
    assert(MH_Initialize() == MH_OK);
    void* foreground = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow");
    assert(MH_CreateHook(foreground, (void*)&test_foreground, nullptr) == MH_OK);
    assert(MH_EnableHook(foreground) == MH_OK);
    HMODULE module = LoadLibraryW(dll.c_str());
    assert(module);
    auto init = (bool(*)())GetProcAddress(module, "SteamAPI_Init");
    auto shutdown = (void(*)())GetProcAddress(module, "SteamAPI_Shutdown");
    assert(init && shutdown && init());
    auto pump = [] {
        auto until = GetTickCount64() + 2500;
        while (GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
    };
    pump();
    // Exceed the initial screen-sized buffers, then shrink and grow again.
    const int width = GetSystemMetrics(SM_CXSCREEN) + 64;
    const int height = GetSystemMetrics(SM_CYSCREEN) + 64;
    for (SIZE size : {SIZE{width, height}, SIZE{320, 240}, SIZE{900, 700}}) {
        assert(SetWindowPos(window, nullptr, -30000, -30000, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE));
        pump();
    }
    shutdown();
    DestroyWindow(window);
    MH_Uninitialize();
    std::ifstream input(dir / "STAR" / "star.log");
    std::string log((std::istreambuf_iterator<char>(input)), {});
    assert(log.find("External: first ULW ok=1") != std::string::npos);
    for (const auto& size : {std::to_string(width) + "x" + std::to_string(height),
            std::string("320x240"), std::string("900x700")})
        assert(log.find("External: surfaces ready (" + size + ")") != std::string::npos);
}
