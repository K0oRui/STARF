#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <MinHook.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Exercise the real DLL without moving the user's mouse or pressing keys.
static HWND game;
static bool chord;
static POINT cursor{-29800, -29800};
static int warps;
static SHORT WINAPI key_state(int key) { return chord && (key == VK_SHIFT || key == VK_TAB) ? SHORT(0x8000) : 0; }
static HWND WINAPI foreground() { return game; }
static BOOL WINAPI cursor_pos(POINT* point) { *point = cursor; return TRUE; }
static BOOL WINAPI warp_cursor(int, int) { ++warps; return TRUE; }

int main(int argc, char** argv)
{
    assert(argc == 4);
    const std::string mode = argv[3];
    auto dir = std::filesystem::absolute(argv[2]) /
        ("gdi-input-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(dir / "STAR");
    std::ofstream(dir / "STAR" / "overlay.star") << "enabled=true\nmode=hook\nshow_fps=false\nshow_playtime=false\n";
    std::ofstream(dir / "STAR" / "steam_appid.txt") << "480\n";
    SetEnvironmentVariableW(L"APPDATA", dir.c_str());
    auto dll = dir / std::filesystem::path(argv[1]).filename();
    std::filesystem::copy_file(argv[1], dll);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"STAR_GDI_Input_Test";
    assert(RegisterClassW(&wc));
    // Shown without activating (SW_SHOWNOACTIVATE): IsWindowVisible passes for
    // API detection, but the window never steals foreground focus.
    game = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Game", WS_POPUP,
        -30000, -30000, mode == "scaled" ? 1280 : 640, mode == "scaled" ? 960 : 480,
        nullptr, nullptr, wc.hInstance, nullptr);
    assert(game);
    ShowWindow(game, SW_SHOWNOACTIVATE);
    assert(MH_Initialize() == MH_OK);
    auto hook = [](const char* name, void* detour) {
        auto address = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
        assert(address && MH_CreateHook(address, detour, nullptr) == MH_OK);
        assert(MH_EnableHook(address) == MH_OK);
    };
    hook("GetForegroundWindow", (void*)&foreground);
    hook("GetAsyncKeyState", (void*)&key_state);
    hook("GetCursorPos", (void*)&cursor_pos);
    hook("SetCursorPos", (void*)&warp_cursor);
    HMODULE module = LoadLibraryW(dll.c_str());
    assert(module);
    auto init = (bool(*)())GetProcAddress(module, "SteamAPI_Init");
    auto shutdown = (void(*)())GetProcAddress(module, "SteamAPI_Shutdown");
    assert(init && shutdown && init());
    HDC dc = GetDC(game);
    HDC window_dc = dc;
    HBITMAP memory_bitmap = nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    const bool memory = mode != "window";
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 640; info.bmiHeader.biHeight = -480;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    std::vector<unsigned> pixels(640 * 480, 0xff202020);
    if (memory) {
        dc = CreateCompatibleDC(window_dc);
        void* bits = nullptr;
        memory_bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        assert(memory_bitmap && bits);
        previous_bitmap = SelectObject(dc, memory_bitmap);
    }
    auto present = [&] {
        const int x = mode == "letterbox" ? 48 : 0;
        const int top = mode == "letterbox" ? 32 : 0;
        const int step = mode == "single" || !memory ? 480 : 8;
        for (int y = top; y < 480 - top; y += step)
            assert(StretchDIBits(dc, x, y, 640 - 2*x, step, 0, 0, 640 - 2*x, step,
                pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY) > 0);
    };
    for (int i = 0; i < 24; ++i) { present(); Sleep(100); }
    for (int cycle = 0; cycle < 3; ++cycle) {
        chord = true; present(); // Open.
        assert(GetAsyncKeyState(VK_TAB) == 0); // Input is blocked for the game.
        // RGSS may discard removed messages instead of dispatching WndProc.
        // The overlay must consume wheel events on removal, once per event.
        assert(PostMessageW(game, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0));
        MSG wheel{};
        assert(PeekMessageW(&wheel, game, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE));
        assert(wheel.message == WM_MOUSEWHEEL);
        assert(PeekMessageW(&wheel, game, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE));
        assert(wheel.message == WM_NULL);
        int before = warps;
        assert(SetCursorPos(0, 0));
        assert(warps == before); // Game cannot park the cursor while open.
        present(); // Held chord must not toggle twice.
        chord = false; present(); // Release must re-arm the edge.
        chord = true; present(); // Close.
        assert(GetAsyncKeyState(VK_TAB) & 0x8000);
        assert(PostMessageW(game, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0));
        assert(PeekMessageW(&wheel, game, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE));
        assert(wheel.message == WM_MOUSEWHEEL); // Closed: game retains its input.
        assert(SetCursorPos(0, 0));
        assert(warps == before + 1);
        chord = false; present();
    }
    shutdown();
    if (memory) {
        SelectObject(dc, previous_bitmap);
        DeleteObject(memory_bitmap);
        DeleteDC(dc);
    }
    ReleaseDC(game, window_dc);
    DestroyWindow(game);
    // Hooks in the DLL still refer to our trampolines until process exit.
    std::ifstream input(dir / "STAR" / "star.log");
    std::string log((std::istreambuf_iterator<char>(input)), {});
    assert(log.find("ImGui ready (GDI renderer)") != std::string::npos);
}
