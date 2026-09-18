#include "overlay.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/callbacks.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_vulkan.h"
#include <MinHook.h>
#include <d3d9.h>
#include <d3d12.h>
#include <cmath>
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <vulkan/vulkan.h>
#include <cctype>
#include <ctime>
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static StarOverlay* g_overlay = nullptr;
#ifdef _WIN64
void* StarOverlay::g_dx12_captured_queue_ = nullptr;
#endif

static float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static float easeOut(float t) { float f = 1.f - t; return 1.f - f * f * f; }

// 5-point star (procedural trophy for the all-complete toast).
static void draw_star(ImDrawList* dl, ImVec2 c, float r_out, float r_in, ImU32 col)
{
    const float PI = 3.14159265f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        float r = (i % 2 == 0) ? r_out : r_in;
        float ang = -PI / 2.f + (float)i * PI / 5.f;
        pts[i] = { c.x + cosf(ang) * r, c.y + sinf(ang) * r };
    }
    dl->AddConvexPolyFilled(pts, 10, col);
}

// Unlock timestamp as compact "dd.mm HH:MM" (fits under the row button).
static std::string fmt_unlock_time(uint32_t t)
{
    if (!t) return {};
    time_t tt = (time_t)t;
    struct tm lt{};
    if (localtime_s(&lt, &tt) != 0) return {};
    char buf[16];
    strftime(buf, sizeof(buf), "%d.%m %H:%M", &lt);
    return buf;
}

// Word-wrap text into at most two lines fitting max_w (measured at the given
// font size). Overlong remainder is ellipsized onto line 2. Returns 0/1/2.
static int wrap_two_lines(ImFont* font, float size, const char* text, float max_w,
                           std::string& l1, std::string& l2)
{
    l1.clear(); l2.clear();
    if (!text || !*text || !font || max_w <= 8.f || size <= 0.f) return 0;
    std::vector<std::string> words;
    for (const char* p = text; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != ' ') e++;
        words.emplace_back(p, e);
        p = e;
    }
    if (words.empty()) return 0;
    auto width = [&](const std::string& s) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.f, s.c_str()).x;
    };
    size_t i = 0;
    while (i < words.size()) {
        std::string t = l1.empty() ? words[i] : l1 + " " + words[i];
        if (width(t) <= max_w) { l1 = t; i++; } else break;
    }
    if (l1.empty()) { l1 = words[0]; i = 1; } // single overlong word: clip as before
    if (i >= words.size()) return 1;
    while (i < words.size()) {
        std::string t = l2.empty() ? words[i] : l2 + " " + words[i];
        if (width(t) <= max_w) { l2 = t; i++; } else break;
    }
    if (i < words.size()) {
        while (!l2.empty() && width(l2 + "...") > max_w) {
            size_t sp = l2.find_last_of(' ');
            if (sp == std::string::npos) { l2.clear(); break; }
            l2.resize(sp);
        }
        l2 += "...";
    }
    return 2;
}

static ImU32 col(uint8_t r, uint8_t g, uint8_t b, float a = 1.f) {
    return IM_COL32(r, g, b, (int)(a * 255.f + .5f));
}
static ImVec4 v4(uint8_t r, uint8_t g, uint8_t b, float a = 1.f) {
    return { r / 255.f, g / 255.f, b / 255.f, a };
}

// Persist one key in overlay.star (keeps art header/comments, replaces or appends).
static void save_overlay_key(const std::string& key, const std::string& value)
{
    std::string path = Settings::get().settings_dir + "\\overlay.star";
    std::vector<std::string> lines;
    {
        std::ifstream in(utf8_to_wstring(path));
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    bool found = false;
    for (auto& ln : lines) {
        size_t p = 0;
        while (p < ln.size() && isspace((unsigned char)ln[p])) p++;
        if (ln.compare(p, key.size(), key) == 0) {
            size_t q = p + key.size();
            while (q < ln.size() && isspace((unsigned char)ln[q])) q++;
            if (q < ln.size() && ln[q] == '=') {
                ln = ln.substr(0, p) + key + " = " + value;
                found = true;
            }
        }
    }
    if (!found) lines.push_back(key + " = " + value);
    std::ofstream out(utf8_to_wstring(path), std::ios::trunc);
    for (auto& ln : lines) out << ln << "\n";
}

void StarOverlay::load_notes()
{
    notes_text_.clear();
    notes_loaded_ = true;
    notes_dirty_ = false;
    std::string path = Settings::get().settings_dir + "\\notes.txt";
    std::ifstream in(utf8_to_wstring(path), std::ios::binary);
    if (!in.is_open()) return;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 65536) data.resize(65536);
    // Strip UTF-8 BOM if present.
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    notes_text_ = data;
}

void StarOverlay::save_notes()
{
    if (!notes_dirty_) return;
    notes_dirty_ = false;
    notes_last_edit_ = GetTickCount();
    std::string path = Settings::get().settings_dir + "\\notes.txt";
    std::ofstream out(utf8_to_wstring(path), std::ios::binary | std::ios::trunc);
    if (out.is_open()) out << notes_text_;
}

#define P_BG0    0x1a,0x1a,0x1a
#define P_BG1    0x22,0x22,0x22
#define P_BG2    0x2a,0x2a,0x2a
#define P_SEP    0x35,0x35,0x35
#define P_TXT    0xf2,0xf2,0xf2
#define P_MUT    0x80,0x80,0x80
#define P_LGT    0x9e,0x9e,0x9e
#define P_DIM    0x4a,0x4a,0x4a
#define P_GRN    0x4c,0xb8,0x4c

void StarOverlay::open_panel()
{
    if (enabled_ && !open_) toggle_overlay();
}

void StarOverlay::toggle_overlay()
{    open_ = !open_;
    if (open_) {
        // Freeze the cursor where it is: polled reads (XNA Mouse.GetState)
        // keep seeing this spot instead of following the panel mouse.
        if (orig_get_cursor_pos_) orig_get_cursor_pos_(&frozen_cursor_);
        else GetCursorPos(&frozen_cursor_);
        if (orig_clip_cursor_) orig_clip_cursor_(nullptr); else ClipCursor(nullptr);

        int current = orig_show_cursor_ ? orig_show_cursor_(TRUE) : ShowCursor(TRUE);
        if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
        current--; // net display count before we touched anything

        // Locked/hidden gameplay cursors sit at deep negative counts with the
        // game re-hiding every frame. Drive to visible with margin and
        // remember the debt so close restores the exact original count.
        // External mode skips this: the virtual panel cursor owns the
        // cursor there, and double-managing the count drifts it per cycle.
        cursor_show_count_offset_ = 0;
        if (mode_ != OverlayMode::External) {
            int tries = 0;
            while (current < 1 && tries++ < 40) {
                if (orig_show_cursor_) orig_show_cursor_(TRUE); else ShowCursor(TRUE);
                cursor_show_count_offset_++;
                current++;
            }
        }
        HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
        if (orig_set_cursor_) orig_set_cursor_(arrow); else SetCursor(arrow);
        if (cursor_show_count_offset_ > 0)
            STAR_LOG("Overlay opened (cursor floor +%d)", cursor_show_count_offset_);
        else
            STAR_LOG("Overlay opened");
    } else {
        save_notes(); // flush unsaved notes text
        // Leave the OS cursor visible. The game re-asserts its own cursor
        // state (hidden in gameplay, shown in menus) on its next ShowCursor
        // call; restoring the pre-open count here hides the cursor even when
        // the game now wants it shown.
        int current = orig_show_cursor_ ? orig_show_cursor_(TRUE) : ShowCursor(TRUE);
        if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
        current--;
        while (current < 1) {
            if (orig_show_cursor_) orig_show_cursor_(TRUE); else ShowCursor(TRUE);
            current++;
        }
        STAR_LOG("Overlay closed");
    }
    GameOverlayActivated_t active{};
    active.m_bActive = open_ ? 1 : 0;
    STAR_DispatchCallback(GameOverlayActivated_t::k_iCallback, &active, sizeof(active));
}

void StarOverlay::poll_hotkey()
{
    poll_focus();
    // Edge-triggered on shared state with star_wnd_proc so one physical
    // press toggles exactly once no matter which path sees it first.
    // (WndProc path covers normal pumps, this covers RawInput/DirectInput
    // games that never deliver WM_KEYDOWN.) Called every present.
    SHORT shift = real_GetAsyncKeyState(VK_SHIFT);
    SHORT tab = real_GetAsyncKeyState(VK_TAB);
    bool down = ((shift & 0x8000) != 0) && ((tab & 0x8000) != 0);
    // Also allow Shift+` (backtick) as alt combo for keyboards/layouts where Tab is swallowed
    if (!down) {
        SHORT oem3 = real_GetAsyncKeyState(VK_OEM_3);
        SHORT shiftOnly = real_GetAsyncKeyState(VK_SHIFT);
        if (((shiftOnly & 0x8000) != 0) && ((oem3 & 0x8000) != 0))
            down = true;
    }
    if (down && !hotkey_prev_down_) {
        toggle_overlay();
    }
    hotkey_prev_down_ = down;
    // Steam-style screenshot key, works with panel open or closed.
    {
        bool fdown = (real_GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (fdown && !f12_prev_down_) request_screenshot();
        f12_prev_down_ = fdown;
    }
    // Text input for games (like XNA/Terraria) that never deliver WM_KEYDOWN/
    // WM_CHAR to this window: synthesize ImGui key events from polled state.
    if (open_ && imgui_initialized_ && !down)
        poll_keyboard();
    else if (!open_)
        prev_keys_valid_ = false;
}

void StarOverlay::poll_keyboard()
{
    // Only feed the game window (or our own overlay window in external
    // mode), not whatever the user alt-tabbed to.
    HWND fg = GetForegroundWindow();
    HWND ref = hwnd_ ? hwnd_ : ext_game_hwnd_;
    if (!fg || ((ref && fg != ref) && fg != ext_hwnd_)) {
        prev_keys_valid_ = false;
        return;
    }

    // If real WM_CHAR messages are flowing (other games), don't double-type.
    bool msgs_alive = (last_wmchar_tick_ != 0) &&
        (GetTickCount() - last_wmchar_tick_ < 1000);

    BYTE ks[256];
    if (!real_GetKeyboardState(ks)) return;
    if (!prev_keys_valid_) {
        memcpy(prev_keys_, ks, sizeof(ks));
        prev_keys_valid_ = true;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    HKL layout = GetKeyboardLayout(0);
    for (int vk = 0; vk < 256; vk++) {
        bool is_down = (ks[vk] & 0x80) != 0;
        bool was_down = (prev_keys_[vk] & 0x80) != 0;
        if (is_down == was_down) continue;
        // Map VK -> ImGuiKey for the keys ImGui cares about.
        ImGuiKey key = ImGuiKey_None;
        switch (vk) {
        case VK_TAB: key = ImGuiKey_Tab; break;
        case VK_LEFT: key = ImGuiKey_LeftArrow; break;
        case VK_RIGHT: key = ImGuiKey_RightArrow; break;
        case VK_UP: key = ImGuiKey_UpArrow; break;
        case VK_DOWN: key = ImGuiKey_DownArrow; break;
        case VK_PRIOR: key = ImGuiKey_PageUp; break;
        case VK_NEXT: key = ImGuiKey_PageDown; break;
        case VK_HOME: key = ImGuiKey_Home; break;
        case VK_END: key = ImGuiKey_End; break;
        case VK_INSERT: key = ImGuiKey_Insert; break;
        case VK_DELETE: key = ImGuiKey_Delete; break;
        case VK_BACK: key = ImGuiKey_Backspace; break;
        case VK_SPACE: key = ImGuiKey_Space; break;
        case VK_RETURN: key = ImGuiKey_Enter; break;
        case VK_ESCAPE: key = ImGuiKey_Escape; break;
        case VK_SHIFT: key = ImGuiKey_ModShift; break;
        case VK_CONTROL: key = ImGuiKey_ModCtrl; break;
        case VK_MENU: key = ImGuiKey_ModAlt; break;
        case VK_CAPITAL: key = ImGuiKey_CapsLock; break;
        default:
            if (vk >= '0' && vk <= '9') key = (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
            else if (vk >= 'A' && vk <= 'Z') key = (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
            else if (vk >= VK_F1 && vk <= VK_F12) key = (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) key = (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
            break;
        }
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, is_down);
        // Note: unmapped VKs (e.g. OEM punctuation) still produce characters
        // below via ToUnicodeEx; key events aren't needed for typing.

        // Printable characters via layout-aware translation (key press only).
        if (is_down && !msgs_alive && vk != VK_TAB) {
            UINT sc = MapVirtualKeyExA((UINT)vk, MAPVK_VK_TO_VSC, layout);
            if (sc != 0) {
                WCHAR buf[8] = {};
                int n = ToUnicodeEx((UINT)vk, sc, ks, buf, 7, 0, layout);
                for (int i = 0; i < n; i++) {
                    if (buf[i] >= 0x20 && buf[i] != 0x7f)
                        io.AddInputCharacter(buf[i]);
                }
            }
        }
    }
    memcpy(prev_keys_, ks, sizeof(ks));
}

LRESULT CALLBACK StarOverlay::star_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_overlay) goto passthrough;

    // Diagnostics: first few input messages prove the hook is alive and show
    // whether the panel was open when the user typed/clicked.
    if (msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE) {
        static int logged_inputs = 0;
        static DWORD first_tick = 0;
        if (logged_inputs < 8) {
            if (first_tick == 0) first_tick = GetTickCount();
            // Log mouse-move only once (it floods), keys/clicks a few times.
            if (msg != WM_MOUSEMOVE || logged_inputs == 0) {
                logged_inputs++;
                STAR_LOG("WndProc msg=0x%04x wp=0x%llx open=%d hwnd=%p t+%lums",
                    msg, (unsigned long long)wp, (int)g_overlay->open_, hwnd,
                    (unsigned long)(GetTickCount() - first_tick));
            }
        }
    }

    if (msg == WM_KEYDOWN && wp == VK_TAB && (g_overlay->real_GetKeyState(VK_SHIFT) & 0x8000)) {
        // Shared edge with poll_hotkey(): skip if polling already claimed it.
        if (!g_overlay->hotkey_prev_down_) {
            g_overlay->toggle_overlay();
            g_overlay->hotkey_prev_down_ = true;
        }
        return 0;
    }

    if (g_overlay->open_ && g_overlay->mode_ != OverlayMode::External && msg == WM_SETCURSOR) {
        HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
        if (g_overlay->orig_set_cursor_) g_overlay->orig_set_cursor_(arrow); else SetCursor(arrow);
        return TRUE;
    }

    if (g_overlay->open_) {
        if (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_IME_CHAR)
            g_overlay->last_wmchar_tick_ = GetTickCount();
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return TRUE;
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_MOUSEMOVE:   case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_INPUT: // RawInput (Unity mouse deltas/buttons) must not reach the game either
        case WM_KEYDOWN:     case WM_KEYUP:      case WM_CHAR:
        case WM_SYSKEYDOWN:  case WM_SYSKEYUP:   case WM_SYSCHAR:
        case WM_IME_CHAR:    case WM_IME_KEYDOWN: case WM_IME_KEYUP:
            return 0;
        }
    }

passthrough:
    return g_overlay
        ? (IsWindowUnicode(hwnd)
            ? CallWindowProcW(g_overlay->wnd_proc_orig_, hwnd, msg, wp, lp)
            : CallWindowProcA(g_overlay->wnd_proc_orig_, hwnd, msg, wp, lp))
        : (IsWindowUnicode(hwnd)
            ? DefWindowProcW(hwnd, msg, wp, lp)
            : DefWindowProcA(hwnd, msg, wp, lp));
}

StarOverlay& StarOverlay::get()
{
    static StarOverlay instance;
    return instance;
}

void StarOverlay::init()
{
    g_overlay = this;
    enabled_  = Settings::get().overlay_enabled;
    ui_scale_ = Settings::get().overlay_scale;
    focus_last_tick_ = GetTickCount();
    last_playtime_save_ = GetTickCount();
    Storage::get().load_playtime(base_playtime_sec_);
    load_notes();
    mode_ = (Settings::get().overlay_mode == "external") ? OverlayMode::External : OverlayMode::Hook;
    // "auto" (default) starts on hooks and falls back to external if the
    // title proves hostile (see switch_to_external).
    if (!enabled_) return;
    icon_decode_stop_ = false;
    icon_decode_thread_ = std::thread(&StarOverlay::icon_decode_worker, this);
    fallback_count_ = Settings::get().overlay_fallback_count;
    fallback_level_ = Settings::get().overlay_fallback_level;
    retry_session_ = false;
    fell_back_this_session_ = false;
    if (mode_ == OverlayMode::External && fallback_count_ > 0) {
        // A previous session fell back to external. Skip this one too, then
        // retry hooks once the skip window runs out (exponential backoff).
        // The persisted mode stays "external" until a retry session either
        // ends cleanly (shutdown clears it) or falls back again (which grows
        // the window), so a crash mid-retry just skips again next launch.
        if (--fallback_count_ == 0) {
            mode_ = OverlayMode::Hook;
            retry_session_ = true;
            STAR_LOG("Overlay backoff done - retrying hook mode this session");
        } else {
            save_overlay_key("fallback_count", std::to_string(fallback_count_));
            STAR_LOG("Overlay staying external (backoff %d sessions left)", fallback_count_);
        }
    }
    if (hooks_installed_) return;
    MH_STATUS mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) STAR_LOG("MinHook init failed status=%d", (int)mh_init);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "STAR_Dummy";
    RegisterClassExA(&wc);

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (user32) {
        void* pShowCursor = (void*)GetProcAddress(user32, "ShowCursor");
        if (!orig_show_cursor_ && MH_CreateHook(pShowCursor, &hooked_ShowCursor, (void**)&orig_show_cursor_) == MH_OK) {
            MH_EnableHook(pShowCursor);
            STAR_LOG("ShowCursor hooked");
        }
        void* pClipCursor = (void*)GetProcAddress(user32, "ClipCursor");
        if (!orig_clip_cursor_ && MH_CreateHook(pClipCursor, &hooked_ClipCursor, (void**)&orig_clip_cursor_) == MH_OK) {
            MH_EnableHook(pClipCursor);
            STAR_LOG("ClipCursor hooked");
        }
        void* pSetCursor = (void*)GetProcAddress(user32, "SetCursor");
        if (!orig_set_cursor_ && MH_CreateHook(pSetCursor, &hooked_SetCursor, (void**)&orig_set_cursor_) == MH_OK) {
            MH_EnableHook(pSetCursor);
            STAR_LOG("SetCursor hooked");
        }

        // Input blackout hooks: lie to game-side polling while open_.
        // Our own polling below always goes through orig_* (real state).
        void* pAsyncKey = (void*)GetProcAddress(user32, "GetAsyncKeyState");
        if (pAsyncKey && !orig_get_async_key_ &&
            MH_CreateHook(pAsyncKey, &hooked_GetAsyncKeyState, (void**)&orig_get_async_key_) == MH_OK) {
            MH_EnableHook(pAsyncKey);
            STAR_LOG("GetAsyncKeyState hooked");
        }
        void* pKbState = (void*)GetProcAddress(user32, "GetKeyboardState");
        if (pKbState && !orig_get_keyboard_state_ &&
            MH_CreateHook(pKbState, &hooked_GetKeyboardState, (void**)&orig_get_keyboard_state_) == MH_OK) {
            MH_EnableHook(pKbState);
            STAR_LOG("GetKeyboardState hooked");
        }
        void* pKeyState = (void*)GetProcAddress(user32, "GetKeyState");
        if (pKeyState && !orig_get_key_ &&
            MH_CreateHook(pKeyState, &hooked_GetKeyState, (void**)&orig_get_key_) == MH_OK) {
            MH_EnableHook(pKeyState);
            STAR_LOG("GetKeyState hooked");
        }
        void* pCursorPos = (void*)GetProcAddress(user32, "GetCursorPos");
        if (pCursorPos && !orig_get_cursor_pos_ &&
            MH_CreateHook(pCursorPos, &hooked_GetCursorPos, (void**)&orig_get_cursor_pos_) == MH_OK) {
            MH_EnableHook(pCursorPos);
            STAR_LOG("GetCursorPos hooked");
        }
        // Unity-style cursor lock warps to center every frame via SetCursorPos.
        // Drop those warps while open so the panel mouse stays free; physical
        // mouse movement never goes through here so ImGui still tracks.
        void* pSetCursorPos = (void*)GetProcAddress(user32, "SetCursorPos");
        if (pSetCursorPos && !orig_set_cursor_pos_ &&
            MH_CreateHook(pSetCursorPos, &hooked_SetCursorPos, (void**)&orig_set_cursor_pos_) == MH_OK) {
            MH_EnableHook(pSetCursorPos);
            STAR_LOG("SetCursorPos hooked");
        }
        // Same warps can arrive as injected input (Unity centers via
        // SendInput/mouse_event on some versions). Drop injected mouse
        // motion while open; physical mouse never travels this path.
        void* pSendInput = (void*)GetProcAddress(user32, "SendInput");
        if (pSendInput && !orig_send_input_ &&
            MH_CreateHook(pSendInput, &hooked_SendInput, (void**)&orig_send_input_) == MH_OK) {
            MH_EnableHook(pSendInput);
            STAR_LOG("SendInput hooked");
        }
        void* pMouseEvent = (void*)GetProcAddress(user32, "mouse_event");
        if (pMouseEvent && !orig_mouse_event_ &&
            MH_CreateHook(pMouseEvent, &hooked_mouse_event, (void**)&orig_mouse_event_) == MH_OK) {
            MH_EnableHook(pMouseEvent);
            STAR_LOG("mouse_event hooked");
        }
    }

    if (mode_ == OverlayMode::External) {
        // Detection hooks below are safe (proven: presents subclassing and
        // ECL interception never touch GPU state); only drawing is skipped.
        // User32 (cursor/input-blackout) hooks stay; they never touch
        // rendering and are proven safe.
        hooks_installed_ = true;
        start_external_thread();
    }

    hook_dx9();
    hook_dx11();
    hook_opengl();
    hook_vulkan();
    hooks_installed_ = true;

    // Retry late-loaded gfx modules (game loads d3d12/vulkan/opengl AFTER SteamAPI_Init).
    retry_stop_ = false;
    std::thread([this]() {
        for (int i = 0; i < 30; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (retry_stop_.load() || !g_overlay) break;
            ensure_hooks();
            if (dx11_hooked_ && dx9_hooked_ && opengl_hooked_ && vulkan_hooked_) break;
            if (imgui_initialized_) break;
        }
    }).detach();
}

void StarOverlay::start_external_thread()
{
    if (ext_thread_) return;
    ext_stop_ = false;
    ext_thread_ = CreateThread(nullptr, 0, &external_thread_entry, this, 0, nullptr);
    if (!ext_thread_) STAR_LOG("External overlay thread failed");
    else STAR_LOG("External overlay started");
}

void StarOverlay::switch_to_external(const char* reason, bool dx12_hostile)
{
    // Hook rendering proved hostile (GPU fault / endless fence timeouts).
    // Remember "external" itself (not just render-off) so the next launch
    // goes straight to the working UI. API emulation + input hooks stay.
    // The fallback is not permanent: an exponential backoff (1,3,7,15,31,63
    // skipped sessions) retries hook mode on later launches, so a transient
    // GPU hiccup heals itself. Explicit "hook" mode never auto-falls back.
    if (mode_ == OverlayMode::External) return;
    if (Settings::get().overlay_mode == "hook") {
        STAR_LOG("Hook mode hostile (%s) - staying on hooks (mode=hook is explicit)", reason);
        return;
    }
    mode_ = OverlayMode::External;
    fell_back_this_session_ = true;
    fallback_level_ = std::min(fallback_level_ + 1, 6);
    fallback_count_ = (1 << fallback_level_) - 1;
    save_overlay_key("mode", "external");
    save_overlay_key("fallback_count", std::to_string(fallback_count_));
    save_overlay_key("fallback_level", std::to_string(fallback_level_));
    if (dx12_hostile) {
        // The title faults on DX12 in-backbuffer drawing: keep drawing off
        // so the retry (and any manual hook mode) stays safe.
        save_overlay_key("dx12_render", "false");
    }
    STAR_LOG("Switching to external overlay (%s) - backoff level %d (%d sessions)",
        reason, fallback_level_, fallback_count_);
    start_external_thread();
}

void StarOverlay::ensure_hooks()
{
    if (!enabled_ || !g_overlay) return;
    // Each hook fn is now idempotent (checks orig_* / hooked flag), so safe to retry.
    if (!dx11_hooked_) hook_dx11();
    if (!dx9_hooked_) hook_dx9();
    if (!opengl_hooked_) hook_opengl();
    if (!vulkan_hooked_) hook_vulkan();
#ifdef _WIN64
    hook_dx12_ecl();
#endif
}

void StarOverlay::hook_window_for(HWND h)
{
    if (!h || !IsWindow(h)) return;
    if (h == hwnd_ && wnd_proc_orig_) return;
    // Window recreated (Unity mode switch / multi-window): unhook old, hook new.
    if (hwnd_ && wnd_proc_orig_ && hwnd_ != h) {
        if (IsWindow(hwnd_)) {
            if (IsWindowUnicode(hwnd_))
                SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
            else
                SetWindowLongPtrA(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
        }
        wnd_proc_orig_ = nullptr;
    }
    hwnd_ = h;
    hook_window();
}

void StarOverlay::hook_window()
{
    if (hwnd_ && !wnd_proc_orig_ && IsWindow(hwnd_)) {
        WNDPROC prev = nullptr;
        if (IsWindowUnicode(hwnd_)) {
            prev = (WNDPROC)SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)star_wnd_proc);
        } else {
            prev = (WNDPROC)SetWindowLongPtrA(hwnd_, GWLP_WNDPROC, (LONG_PTR)star_wnd_proc);
        }
        if (prev) {
            wnd_proc_orig_ = prev;
            STAR_LOG("WndProc hooked hwnd=%p", hwnd_);
        } else {
            DWORD err = GetLastError();
            STAR_LOG("WndProc hook FAILED hwnd=%p err=%lu", hwnd_, (unsigned long)err);
        }
    }
}

uint64_t StarOverlay::total_playtime_sec() const
{
    if (focused_sec_ < 0) focused_sec_ = 0;
    return base_playtime_sec_ + (uint64_t)focused_sec_;
}

void StarOverlay::note_present()
{
    DWORD now = GetTickCount();
    if (present_fps_tick_ == 0) present_fps_tick_ = now;
    present_frames_++;
    if (now - present_fps_tick_ >= 500) {
        present_fps_ = present_frames_ * 1000.f / (float)(now - present_fps_tick_);
        present_frames_ = 0;
        present_fps_tick_ = now;
    }
}

void StarOverlay::poll_focus()
{    DWORD now = GetTickCount();
    if (focus_last_tick_ == 0) focus_last_tick_ = now;
    // Same foreground rule as keyboard input: game window or our own overlay
    // window counts as "playing"; anything else pauses the clock.
    HWND fg = GetForegroundWindow();
    HWND ref = hwnd_ ? hwnd_ : ext_game_hwnd_;
    if (fg && (!ref || fg == ref || fg == ext_hwnd_))
        focused_sec_ += (now - focus_last_tick_) / 1000.0;
    focus_last_tick_ = now;
}

std::string StarOverlay::format_playtime(uint64_t secs)
{
    char buf[32];
    if (secs >= 3600) snprintf(buf, sizeof(buf), "%lluh %02llum",
        (unsigned long long)(secs / 3600), (unsigned long long)((secs / 60) % 60));
    else if (secs >= 60) snprintf(buf, sizeof(buf), "%llum",
        (unsigned long long)(secs / 60));
    else snprintf(buf, sizeof(buf), "%llus", (unsigned long long)secs);
    return buf;
}

void StarOverlay::shutdown()
{
    // A retry session that survived without falling back again means the
    // hostile title healed: clear the backoff so future launches use hooks.
    if (retry_session_ && !fell_back_this_session_) {
        save_overlay_key("mode", "auto");
        save_overlay_key("fallback_count", "0");
        save_overlay_key("fallback_level", "0");
        STAR_LOG("Overlay retry session clean - backoff cleared");
    }
    if (icon_decode_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_stop_ = true;
        }
        icon_decode_cv_.notify_all();
        icon_decode_thread_.join();
    }
    if (mode_ == OverlayMode::External && ext_thread_) {
        ext_stop_ = true;
        WaitForSingleObject(ext_thread_, 5000);
        CloseHandle(ext_thread_);
        ext_thread_ = nullptr;
    }
    // Persist playtime even on early shutdown paths.
    Storage::get().save_playtime(total_playtime_sec());
    bool was_enabled = enabled_;
    std::lock_guard<std::mutex> lock(render_mutex_);
    GraphicsAPI api_snapshot = active_api_;

    while (cursor_show_count_offset_ > 0) {
        if (orig_show_cursor_) orig_show_cursor_(FALSE); else ShowCursor(FALSE);
        cursor_show_count_offset_--;
    }

    if (hwnd_ && wnd_proc_orig_) {
        if (IsWindowUnicode(hwnd_)) {
            SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
        } else {
            SetWindowLongPtrA(hwnd_, GWLP_WNDPROC, (LONG_PTR)wnd_proc_orig_);
        }
        wnd_proc_orig_ = nullptr;
    }

    if (imgui_initialized_) {
        if (active_api_ == GraphicsAPI::DX11) {
            ImGui_ImplDX11_Shutdown();
#ifdef _WIN64
        } else if (active_api_ == GraphicsAPI::DX12) {
            ImGui_ImplDX12_Shutdown();
#endif
        } else if (active_api_ == GraphicsAPI::DX9) {
            ImGui_ImplDX9_Shutdown();
        } else if (active_api_ == GraphicsAPI::OpenGL) {
            ImGui_ImplOpenGL3_Shutdown();
        } else if (active_api_ == GraphicsAPI::Vulkan) {
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
        active_api_ = GraphicsAPI::None;
    }

    cleanup_rtv();
#ifdef _WIN64
    cleanup_dx12();
#endif
    cleanup_vulkan();

    if (api_snapshot == GraphicsAPI::DX11) {
        for (auto& [k, v] : icon_textures_) if (v) ((ID3D11ShaderResourceView*)v)->Release();
    } else if (api_snapshot == GraphicsAPI::DX9) {
        // MANAGED-pool IDirect3DTexture9* icons.
        for (auto& [k, v] : icon_textures_) if (v) ((IDirect3DTexture9*)v)->Release();
        dx9_device_ = nullptr;
    }
    // OpenGL icon textures belong to the game's GL context, which may be gone
    // at shutdown; the OS/driver reclaims them with the context.
    gl_icon_textures_.clear();
    icon_textures_.clear();
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_)  { device_->Release();  device_  = nullptr; }
    hooks_installed_ = false;
    // NOTE: do NOT MH_DisableHook(MH_ALL_HOOKS)/Remove/Uninitialize here.
    // MinHook is shared with integrity hooks (STAR_install_integrity_hooks).
    // Disabling ALL hooks kills CreateFile/GetFileAttributes redirects and
    // double-uninitializes MinHook (DllMain DETACH calls uninstall after this).
    // Leave gfx hooks installed; they passthrough via orig_* when disabled.
    // Keep g_overlay alive for passthrough (hooked Present needs orig pointers).
    retry_stop_ = true;
    enabled_ = false;
    open_ = false;
    if (!was_enabled) return;
}

void StarOverlay::hook_dx11()
{
    if (orig_present_ && orig_resize_) { dx11_hooked_ = true; return; }

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "STAR_Dummy";
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0,"STAR_Dummy","",WS_OVERLAPPEDWINDOW,0,0,4,4,
                                 nullptr,nullptr,wc.hInstance,nullptr);
    if (!dummy) { STAR_LOG("DX11 hook: dummy window failed"); return; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 4;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy; sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* ddev = nullptr; IDXGISwapChain* dsc = nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&sd,&dsc,&ddev,&fl,nullptr);
    if (FAILED(hr)||!dsc) {
        STAR_LOG("DX11 hook: D3D11CreateDeviceAndSwapChain failed hr=0x%08x", (unsigned)hr);
        DestroyWindow(dummy);
        return;
    }

    void** vt = *(void***)dsc;
    MH_STATUS s1 = orig_present_ ? MH_OK : MH_CreateHook(vt[8],  &hooked_Present,       (void**)&orig_present_);
    if (s1 == MH_OK) MH_EnableHook(vt[8]); else STAR_LOG("DX11 hook: Present MH=%d", (int)s1);
    MH_STATUS s2 = orig_resize_ ? MH_OK : MH_CreateHook(vt[13], &hooked_ResizeBuffers, (void**)&orig_resize_);
    if (s2 == MH_OK) MH_EnableHook(vt[13]); else STAR_LOG("DX11 hook: ResizeBuffers MH=%d", (int)s2);

    IDXGISwapChain1* dsc1 = nullptr;
    if (SUCCEEDED(dsc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&dsc1))) {
        void** vt1 = *(void***)dsc1;
        if (!orig_present1_) {
            MH_STATUS s3 = MH_CreateHook(vt1[22], &hooked_Present1, (void**)&orig_present1_);
            if (s3 == MH_OK) {
                MH_EnableHook(vt1[22]);
                STAR_LOG("DX11 Present1 hooked");
            } else {
                STAR_LOG("DX11 hook: Present1 MH=%d", (int)s3);
            }
        }
        dsc1->Release();
    }

    dsc->Release(); ddev->Release(); DestroyWindow(dummy);
    if (orig_present_) { dx11_hooked_ = true; STAR_LOG("DX11 hooked"); }

#ifdef _WIN64
    hook_dx12_ecl();
#endif
}

#ifdef _WIN64
void StarOverlay::hook_dx12_ecl()
{
    if (orig_execute_command_lists_) return;

    typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) hD3D12 = LoadLibraryA("d3d12.dll");
    if (!hD3D12) { STAR_LOG("DX12 ECL: d3d12.dll not found"); return; }

    auto pfnCreate = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pfnCreate) { STAR_LOG("DX12 ECL: D3D12CreateDevice export not found"); return; }

    ID3D12Device* dummy_dev = nullptr;
    HRESULT hr = pfnCreate(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummy_dev));
    if (FAILED(hr)) { STAR_LOG("DX12 ECL: D3D12CreateDevice failed hr=0x%08x", (unsigned)hr); return; }

    D3D12_COMMAND_QUEUE_DESC cqdesc = {};
    cqdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummy_queue = nullptr;
    if (SUCCEEDED(dummy_dev->CreateCommandQueue(&cqdesc, IID_PPV_ARGS(&dummy_queue)))) {
        void** vt12 = *(void***)dummy_queue;
        MH_STATUS mh = MH_CreateHook(vt12[10], &hooked_ExecuteCommandLists, (void**)&orig_execute_command_lists_);
        if (mh == MH_OK) {
            MH_EnableHook(vt12[10]);
            STAR_LOG("DX12 ExecuteCommandLists hooked");
        } else {
            STAR_LOG("DX12 ECL: MH_CreateHook failed status=%d", (int)mh);
        }
        dummy_queue->Release();
    } else {
        STAR_LOG("DX12 ECL: CreateCommandQueue failed");
    }
    dummy_dev->Release();
}
#endif

void StarOverlay::resolve_accent()
{
    const std::string& a = Settings::get().overlay_accent;
    if (a == "red")         { acc_r_ = 0xff; acc_g_ = 0x5a; acc_b_ = 0x5a; }
    else if (a == "green")  { acc_r_ = 0x4c; acc_g_ = 0xb8; acc_b_ = 0x4c; }
    else if (a == "purple") { acc_r_ = 0xb0; acc_g_ = 0x7f; acc_b_ = 0xff; }
    else if (a == "orange") { acc_r_ = 0xff; acc_g_ = 0xa0; acc_b_ = 0x3c; }
    else if (a == "yellow") { acc_r_ = 0xff; acc_g_ = 0xd4; acc_b_ = 0x4d; }
    else                    { acc_r_ = 0x4f; acc_g_ = 0xa3; acc_b_ = 0xff; } // blue
}

ImU32 StarOverlay::acc(float a) const
{
    return IM_COL32(acc_r_, acc_g_, acc_b_, (int)(a * 255.f + .5f));
}

ImVec4 StarOverlay::vacc(float a) const
{
    return { acc_r_ / 255.f, acc_g_ / 255.f, acc_b_ / 255.f, a };
}

void StarOverlay::setup_imgui_style_and_fonts()
{
    resolve_accent();
    ui_scale_ = Settings::get().overlay_scale;
    if (ui_scale_ < 0.75f) ui_scale_ = 0.75f;
    if (ui_scale_ > 2.0f) ui_scale_ = 2.0f;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.FontGlobalScale = ui_scale_;

    char windir[MAX_PATH]{}; GetWindowsDirectoryA(windir, MAX_PATH);
    std::string fd = std::string(windir) + "\\Fonts\\";
    const char* faces[] = { "segoeui.ttf","arial.ttf","tahoma.ttf",nullptr };

    ImFontConfig fc; fc.OversampleH = 4; fc.OversampleV = 4; fc.PixelSnapH = false;

    // Custom TTF first (overlay.star `font`), then system faces.
    std::string custom = Settings::get().overlay_font;
    while (!custom.empty() && (custom.back() == ' ' || custom.back() == '\t' ||
                               custom.back() == '\r' || custom.back() == '\n'))
        custom.pop_back();
    std::string custom_path;
    if (!custom.empty()) {
        bool absolute = (custom.size() > 1 && custom[1] == ':') || (!custom.empty() && (custom[0] == '\\' || custom[0] == '/'));
        // Relative names live in STAR/Fonts (e.g. font = poppins.ttf).
        custom_path = absolute ? custom : Settings::get().settings_dir + "\\Fonts\\" + custom;
        DWORD attr = GetFileAttributesA(custom_path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            STAR_LOG("Overlay font not found: %s", custom_path.c_str());
            custom_path.clear();
        }
    }

    auto tryFont = [&](float sz) -> ImFont* {
        if (!custom_path.empty()) {
            if (auto* f = io.Fonts->AddFontFromFileTTF(custom_path.c_str(), sz, &fc)) {
                if (sz == 16.f) STAR_LOG("Overlay font: %s", custom_path.c_str());
                return f;
            }
            STAR_LOG("Overlay font failed to load: %s", custom_path.c_str());
            custom_path.clear();
        }
        for (int i = 0; faces[i]; i++) {
            if (auto* f = io.Fonts->AddFontFromFileTTF((fd+faces[i]).c_str(), sz, &fc))
                return f;
        }
        return nullptr;
    };

    font_small_ = tryFont(14.f);
    font_title_ = tryFont(19.f);
    if (ImFont* fb = tryFont(16.f)) io.FontDefault = fb;
    else io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.ScaleAllSizes(ui_scale_);
    s.WindowRounding   = 0.f;
    s.ChildRounding    = 4.f;
    s.FrameRounding    = 4.f;
    s.ScrollbarRounding= 4.f;
    s.GrabRounding     = 4.f;
    s.WindowBorderSize = 1.f;
    s.ChildBorderSize  = 0.f;
    s.FrameBorderSize  = 0.f;
    s.WindowPadding    = { 18.f, 16.f };
    s.FramePadding     = { 10.f,  7.f };
    s.ItemSpacing      = { 10.f, 10.f };
    s.ScrollbarSize    = 8.f;

    auto* C = s.Colors;
    C[ImGuiCol_WindowBg]           = v4(P_BG0, 0.97f);
    C[ImGuiCol_ChildBg]            = v4(P_BG1, 1.f);
    C[ImGuiCol_Border]             = v4(P_SEP, 0.6f);
    C[ImGuiCol_FrameBg]            = v4(P_BG2, 1.f);
    C[ImGuiCol_FrameBgHovered]     = v4(0x30,0x30,0x30, 1.f);
    C[ImGuiCol_Button]             = v4(P_BG2, 1.f);
    C[ImGuiCol_ButtonHovered]      = v4(0x32,0x32,0x32, 1.f);
    C[ImGuiCol_ButtonActive]       = v4(0x3a,0x3a,0x3a, 1.f);
    C[ImGuiCol_Header]             = v4(P_BG2, 1.f);
    C[ImGuiCol_HeaderHovered]      = v4(0x30,0x30,0x30, 1.f);
    C[ImGuiCol_ScrollbarBg]        = v4(P_BG0, 1.f);
    C[ImGuiCol_ScrollbarGrab]      = v4(P_SEP, 1.f);
    C[ImGuiCol_ScrollbarGrabHovered]= v4(0x45,0x45,0x45, 1.f);
    C[ImGuiCol_ScrollbarGrabActive] = vacc(1.f);
    C[ImGuiCol_Separator]          = v4(P_SEP, 0.5f);
    C[ImGuiCol_Text]               = v4(P_TXT, 1.f);
    C[ImGuiCol_TextDisabled]       = v4(P_MUT, 1.f);
    C[ImGuiCol_TitleBg]            = v4(P_BG0, 1.f);
    C[ImGuiCol_TitleBgActive]      = v4(P_BG0, 1.f);
    C[ImGuiCol_PopupBg]            = v4(P_BG1, 0.98f);
    C[ImGuiCol_CheckMark]          = vacc(1.f);
    C[ImGuiCol_SliderGrab]         = vacc(1.f);
}

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

    setup_imgui_style_and_fonts();

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

ImTextureID StarOverlay::get_or_create_icon(
    const std::string& key, const std::vector<uint8_t>& rgba, int w, int h)
{
    auto it = icon_textures_.find(key);
    if (it != icon_textures_.end()) return it->second;

    ImTextureID tex_id = nullptr;
    if (!rgba.empty() && w > 0 && h > 0) {
#ifdef _WIN64
        if (active_api_ == GraphicsAPI::DX12) {
            tex_id = upload_icon_dx12(rgba, w, h);
        } else
#endif
        if (active_api_ == GraphicsAPI::Vulkan) {
            tex_id = upload_icon_vulkan(rgba, w, h);
        } else if (active_api_ == GraphicsAPI::DX9) {
            tex_id = upload_icon_dx9(rgba, w, h);
        } else if (active_api_ == GraphicsAPI::OpenGL) {
            tex_id = upload_icon_opengl(rgba, w, h);
        } else if (device_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{ rgba.data(), (UINT)(w*4), 0 };
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&td, &srd, &tex))) {
                ID3D11ShaderResourceView* srv = nullptr;
                device_->CreateShaderResourceView(tex, nullptr, &srv);
                tex->Release();
                tex_id = (ImTextureID)(void*)srv;
            }
        }
    }
    if (tex_id) icon_textures_[key] = tex_id;
    return tex_id;
}

ImTextureID StarOverlay::upload_icon_dx9(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (!dx9_device_) return nullptr;
    // MANAGED pool: survives Reset, no invalidate needed. ImGui DX9 backend
    // draws IDirect3DTexture9* directly.
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dx9_device_->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
        return nullptr;
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0))) {
        // WIC hands us RGBA bytes; D3DFMT_A8R8G8B8 stores BGRA in memory,
        // so swizzle R<->B (otherwise browns render blue/purple).
        const uint8_t* src = rgba.data();
        uint8_t* dst = (uint8_t*)locked.pBits;
        UINT row_bytes = (UINT)w * 4;
        for (int y = 0; y < h; y++) {
            const uint8_t* s = src + (size_t)y * row_bytes;
            uint8_t* d = dst + (size_t)y * locked.Pitch;
            for (int x = 0; x < w; x++) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                s += 4; d += 4;
            }
        }
        tex->UnlockRect(0);
        return (ImTextureID)(void*)tex;
    }
    tex->Release();
    return nullptr;
}

ImTextureID StarOverlay::upload_icon_opengl(const std::vector<uint8_t>& rgba, int w, int h)
{
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) return nullptr;
    typedef void(WINAPI* glGenTexturesFn)(int, unsigned int*);
    typedef void(WINAPI* glBindTextureFn)(unsigned int, unsigned int);
    typedef void(WINAPI* glTexParameteriFn)(unsigned int, unsigned int, int);
    typedef void(WINAPI* glTexImage2DFn)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
    auto glGenTextures = (glGenTexturesFn)GetProcAddress(opengl_dll, "glGenTextures");
    auto glBindTexture = (glBindTextureFn)GetProcAddress(opengl_dll, "glBindTexture");
    auto glTexParameteri = (glTexParameteriFn)GetProcAddress(opengl_dll, "glTexParameteri");
    auto glTexImage2D = (glTexImage2DFn)GetProcAddress(opengl_dll, "glTexImage2D");
    if (!glGenTextures || !glBindTexture || !glTexParameteri || !glTexImage2D) return nullptr;

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return nullptr;
    const unsigned int GL_TEXTURE_2D = 0x0DE1;
    const unsigned int GL_RGBA = 0x1908;
    const unsigned int GL_UNSIGNED_BYTE = 0x1401;
    const unsigned int GL_TEXTURE_MIN_FILTER = 0x2801;
    const unsigned int GL_TEXTURE_MAG_FILTER = 0x2800;
    const int GL_LINEAR = 0x2601;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, (int)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gl_icon_textures_.push_back(tex);
    return (ImTextureID)(void*)(uintptr_t)tex;
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

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f) dt = 0.0167f;

    float target = open_ ? 1.f : 0.f;
    panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
    panel_anim_  = clamp01(panel_anim_);

    if (panel_anim_ > 0.001f) render_panel();
    render_notifications(dt);
    render_hud();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    context_->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();

    maybe_capture_dx11(chain);
}

void StarOverlay::render_hud()
{
    bool show_fps = Settings::get().overlay_show_fps;
    bool show_time = Settings::get().overlay_show_playtime;
    if ((!show_fps && !show_time) || !imgui_initialized_) return;

    {
        static bool hud_logged = false;
        if (!hud_logged) {
            hud_logged = true;
            STAR_LOG("HUD live (fps=%d playtime=%d scale=%.2f)",
                (int)show_fps, (int)show_time, ui_scale_);
        }
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = (ImFont*)font_small_;
    if (!f) f = ImGui::GetFont();

    float pad = 10.f * ui_scale_;
    float x = pad + 4.f;
    float y = pad + 4.f;
    const float txt = 13.f * ui_scale_;
    const float pill_pad = 7.f * ui_scale_;
    const float pill_gap = 6.f * ui_scale_;

    auto pill = [&](const char* text) {
        ImVec2 sz = f->CalcTextSizeA(txt, FLT_MAX, 0.f, text);
        ImVec2 p0 = { x, y };
        ImVec2 p1 = { x + sz.x + pill_pad * 2.f, y + sz.y + pill_pad * 2.f };
        dl->AddRectFilled(p0, p1, col(P_BG0, 0.75f), 4.f);
        dl->AddText(f, txt, { p0.x + pill_pad, p0.y + pill_pad }, col(P_TXT, 0.95f), text);
        x = p1.x + pill_gap;
    };

    if (show_fps) {
        float fps = present_fps_ > 0.5f ? present_fps_ : ImGui::GetIO().Framerate;
        if (fps < 0.f) fps = 0.f;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f));
        pill(buf);
    }
    if (show_time) {
        // Total across sessions; checkpoint to disk every minute.
        DWORD now = GetTickCount();
        if (now - last_playtime_save_ > 60000) {
            last_playtime_save_ = now;
            Storage::get().save_playtime(total_playtime_sec());
        }
        // HUD pill shows the live session clock; the account block shows total.
        uint64_t secs = session_sec();
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
            (int)(secs / 3600), (int)((secs / 60) % 60), (int)(secs % 60));
        pill(buf);
    }
}

void StarOverlay::render_notifications(float dt)
{
    drain_icon_decodes();
    std::vector<AchievementNotification> notifs;
    {
        std::lock_guard<std::mutex> lock(notif_mutex_);
        for (auto& n : notifications_) { n.time_remaining -= dt; n.age += dt; }
        notifications_.erase(
            std::remove_if(notifications_.begin(), notifications_.end(),
                [](const AchievementNotification& n){ return n.time_remaining <= 0.f; }),
            notifications_.end());
        notifs = notifications_;
    }
    if (notifs.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO&    io = ImGui::GetIO();
    ImFont* fsmall = (ImFont*)font_small_;
    ImFont* ftitle = (ImFont*)font_title_;

    const float S = ui_scale_;
    const float W   = 340.f * S;
    const float BASE_H = 82.f * S;
    const float PAD = 14.f;
    const float GAP =  8.f;
    const float SLIDE_DUR = 0.3f;
    const float FADE_DUR  = 0.7f;

    // Toast corner (user-movable, persisted via notify_pos).
    const std::string& npos = Settings::get().overlay_notify_pos;
    bool ntop = (npos == "top_left" || npos == "top_right");
    bool nleft = (npos == "top_left" || npos == "bottom_left");
    float edge_x = nleft ? PAD : io.DisplaySize.x - PAD - W;
    float y = ntop ? PAD : io.DisplaySize.y - PAD;
    if (ntop && nleft && (Settings::get().overlay_show_fps || Settings::get().overlay_show_playtime))
        y += 64.f * S; // keep clear of the HUD pills

    for (size_t i = 0; i < notifs.size() && i < 4; i++) {
        auto& n = notifs[i];

        float tx_probe = (14.f + 52.f + 12.f) * S;
        float tw_probe = W - tx_probe - 12.f * S;
        std::string d1, d2;
        int desc_lines = n.description.empty() ? 0
            : wrap_two_lines(fsmall, 13.f * S, n.description.c_str(), tw_probe, d1, d2);
        float H = BASE_H + (desc_lines == 2 ? 17.f * S : 0.f);
        if (!ntop) y -= H + GAP;

        float slide = easeOut(clamp01(n.age / SLIDE_DUR));
        float fade  = (n.time_remaining < FADE_DUR) ? (n.time_remaining / FADE_DUR) : 1.f;
        float a     = clamp01(slide * fade);

        // Summary toasts go gold instead of accent.
        auto tacc = [&](float m) -> ImU32 {
            if (n.summary) return IM_COL32(255, 205, 70, (int)(a * m * 255.f + .5f));
            return acc(a * m);
        };

        float x = nleft ? edge_x - (1.f - slide) * (W + PAD)
                        : edge_x + (1.f - slide) * (W + PAD);

        dl->AddRectFilled({x, y}, {x+W, y+H}, col(P_BG0, a * 0.95f), 5.f);

        dl->AddRectFilled({x, y+3.f}, {x+3.f, y+H-3.f}, tacc(1.f), 2.f);

        float ix = x + 14.f * S, iy = y + (H - 52.f * S) * .5f, is = 52.f * S;
        ImTextureID notif_tex = get_or_create_icon(n.title, n.icon_rgba, n.icon_width, n.icon_height);
        if (notif_tex) {
            dl->AddImageRounded(notif_tex,
                {ix,iy},{ix+is,iy+is},{0,0},{1,1}, col(0xff,0xff,0xff,a), 4.f);
        } else if (n.summary) {
            dl->AddRectFilled({ix,iy},{ix+is,iy+is}, col(P_BG2, a), 4.f);
            ImVec2 sc = { ix + is * .5f, iy + is * .5f };
            draw_star(dl, sc, is * .30f, is * .30f,
                IM_COL32(255, 205, 70, (int)(a * 60.f + .5f)));
            draw_star(dl, sc, is * .22f, is * .095f, tacc(1.f));
        } else {
            dl->AddRectFilled({ix,iy},{ix+is,iy+is}, col(P_BG2, a), 4.f);
        }
        dl->AddRect({ix-1.5f,iy-1.5f},{ix+is+1.5f,iy+is+1.5f}, tacc(0.6f), 5.f, 0, 1.5f);

        float tx = ix + is + 12.f * S;
        float tw = W - (tx - x) - 12.f * S;

        float ly = y + 12.f * S;
        dl->PushClipRect({tx,ly},{tx+tw,ly+16.f*S},true);
        dl->AddText(fsmall, 12.f * S, {tx,ly}, tacc(1.f), n.header.c_str());
        dl->PopClipRect();

        float ty2 = ly + 18.f * S;
        dl->PushClipRect({tx,ty2},{tx+tw,ty2+22.f*S},true);
        dl->AddText(ftitle, 17.f * S, {tx,ty2}, col(P_TXT, a), n.title.c_str());
        dl->PopClipRect();

        if (desc_lines >= 1) {
            float dy = ty2 + 23.f * S;
            dl->PushClipRect({tx,dy},{tx+tw,dy+38.f*S},true);
            dl->AddText(fsmall, 13.f * S, {tx,dy}, col(P_MUT, a), d1.c_str());
            if (desc_lines == 2)
                dl->AddText(fsmall, 13.f * S, {tx,dy+17.f*S}, col(P_MUT, a), d2.c_str());
            dl->PopClipRect();
        }

        float prog = clamp01(n.time_remaining / 5.f);
        float bx   = x + 3.f, by = y + H - 2.f, bw = W - 6.f;
        dl->AddRectFilled({bx,by},{bx+bw,by+2.f},         col(P_SEP, a * 0.4f), 1.f);
        dl->AddRectFilled({bx,by},{bx+bw*prog, by+2.f},   tacc(0.7f), 1.f);
        if (ntop) y += H + GAP;
    }
}

void StarOverlay::render_panel()
{
    ImGuiIO& io   = ImGui::GetIO();
    float sw      = io.DisplaySize.x;
    float sh      = io.DisplaySize.y;
    ImFont* fsmall = (ImFont*)font_small_;
    ImFont* ftitle = (ImFont*)font_title_;

    float slide   = easeOut(panel_anim_);
    float alpha   = panel_anim_;

    const float PW = 420.f * ui_scale_;

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        {0,0}, {sw,sh}, col(0,0,0, 0.35f * alpha));

    float px = sw - PW * slide;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::SetNextWindowPos({px, 0.f});
    ImGui::SetNextWindowSize({PW, sh});
    ImGui::SetNextWindowBgAlpha(0.97f);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("##star_sidebar", nullptr, wf);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();

    ImGui::PushFont(ftitle);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("STAR");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 10.f);
    ImGui::PushFont(fsmall);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.f);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
    ImGui::Text("Shift+Tab");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", (int)st.wHour, (int)st.wMinute);
        float cw = ImGui::CalcTextSize(clk).x;
        ImGui::SameLine(PW - cw - 14.f);
        ImGui::PushStyleColor(ImGuiCol_Text, vacc(1.f));
        ImGui::Text("%s", clk);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto& s = Settings::get();
    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("Account:  %s", s.account_name.c_str());
    char sid[24]; snprintf(sid, sizeof(sid), "%llu", (unsigned long long)s.steam_id);
    ImGui::Text("Steam ID: %s", sid);
    char aid[12]; snprintf(aid, sizeof(aid), "%u", s.app_id);
    ImGui::Text("App ID:   %s", aid);
    const char* gfx = "detecting…";
    switch (game_api_) {
    case GraphicsAPI::DX9:    gfx = "DirectX 9"; break;
    case GraphicsAPI::DX11:   gfx = "DirectX 11"; break;
    case GraphicsAPI::DX12:   gfx = "DirectX 12"; break;
    case GraphicsAPI::OpenGL: gfx = "OpenGL"; break;
    case GraphicsAPI::Vulkan: gfx = "Vulkan"; break;
    default: break;
    }
    ImGui::Text("Graphics: %s", gfx);
    ImGui::Text("Total playtime: %s", format_playtime(total_playtime_sec()).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Screenshots live above the achievements counter: one tidy block with
    // the button, the hotkey hint, and where files go.
    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("SCREENSHOTS");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    {
        const char* lblShot = "Screenshot";
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          vacc(1.f));
        if (ImGui::Button(lblShot)) request_screenshot();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImGui::SameLine(0.f, 8.f);
        ImGui::PushFont(fsmall);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.f);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
        ImGui::Text("or press F12 in-game");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    {
        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
        ImGui::TextWrapped("Shots: %s", screenshots_dir().c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Gallery strip + viewer modal (thumbnails via the icon cache).
    {
        struct ShotEntry { std::string path; std::string name; FILETIME wt; int iw = 0, ih = 0; };
        static std::vector<ShotEntry> shots;
        static DWORD shots_refresh = 0;
        DWORD nowt = GetTickCount();
        if (shots_refresh == 0 || nowt - shots_refresh > 5000) {
            shots_refresh = nowt;
            shots.clear();
            std::string pattern = screenshots_dir() + "\\*.png";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        ShotEntry e;
                        e.name = fd.cFileName;
                        e.path = screenshots_dir() + "\\" + e.name;
                        e.wt = fd.ftLastWriteTime;
                        shots.push_back(e);
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            std::sort(shots.begin(), shots.end(), [](const ShotEntry& a, const ShotEntry& b) {
                if (a.wt.dwHighDateTime != b.wt.dwHighDateTime)
                    return a.wt.dwHighDateTime > b.wt.dwHighDateTime;
                return a.wt.dwLowDateTime > b.wt.dwLowDateTime;
            });
            if (shots.size() > 12) shots.resize(12);
            for (auto& e : shots) {
                // Dimensions every rescan (cheap metadata read); pixels only
                // upload on cache miss. (Upload-only dims used to zero out on
                // rescan, blanking the viewer a few seconds after opening.)
                uint32 uw = 0, uh = 0;
                if (StarSteamUtils::get().GetImageFileSize(e.path, &uw, &uh)) {
                    e.iw = (int)uw; e.ih = (int)uh;
                }
                if (e.iw <= 0 || e.ih <= 0) continue;
                if (icon_textures_.find("shot_" + e.name) != icon_textures_.end()) continue;
                enqueue_icon_decode(e.path, "shot_" + e.name);
            }
        }

        float avail = ImGui::GetContentRegionAvail().x;
        const float tsz = 56.f, tgap = 6.f;
        // ImageButton adds FramePadding around the image; zero it so the
        // math below holds and thumbs never spill past the panel edge.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        int per_row = (std::max)(1, (int)((avail + tgap - 1.f) / (tsz + tgap)));
        for (size_t i = 0; i < shots.size(); i++) {
            if (i > 0 && (i % (size_t)per_row) != 0) ImGui::SameLine(0.f, tgap);
            ImGui::PushID((int)i);
            auto it = icon_textures_.find("shot_" + shots[i].name);
            ImTextureID tex = (it != icon_textures_.end()) ? it->second : nullptr;
            if (tex && ImGui::ImageButton("##shot", tex, {tsz, tsz})) {
                viewer_file_ = shots[i].path;
                viewer_pending_ = true;
            }
            if (!tex) {
                ImGui::PushStyleColor(ImGuiCol_Button, v4(P_BG2, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
                ImGui::Button("##shotx", {tsz, tsz});
                ImGui::PopStyleColor(2);
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        if (!shots.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT,    1.f));
            if (ImGui::Button("Open folder")) {
                ShellExecuteA(nullptr, "open", screenshots_dir().c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
        }

        if (viewer_pending_) {
            ImGui::OpenPopup("##shotview");
            viewer_pending_ = false;
        }
        ImGui::SetNextWindowPos({sw * 0.5f, sh * 0.42f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, v4(P_BG1, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(P_SEP, 0.8f));
        if (ImGui::BeginPopupModal("##shotview", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            std::string vname = viewer_file_;
            size_t sl = vname.find_last_of("\\/");
            if (sl != std::string::npos) vname = vname.substr(sl + 1);
            ImGui::PushFont(fsmall);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
            ImGui::Text("%s", vname.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            auto vit = icon_textures_.find("shot_" + vname);
            if (vit != icon_textures_.end() && vit->second) {
                int iw = 0, ih = 0;
                for (auto& e : shots) {
                    if (e.name == vname && e.iw > 0 && e.ih > 0) { iw = e.iw; ih = e.ih; break; }
                }
                if (iw > 0 && ih > 0) {
                    // Fill the screen: 70% width / 72% height, aspect kept.
                    float vw = sw * 0.7f;
                    float vh = vw * (float)ih / (float)iw;
                    float maxh = sh * 0.72f;
                    if (vh > maxh) { vh = maxh; vw = vh * (float)iw / (float)ih; }
                    float maxw = sw * 0.75f;
                    if (vw > maxw) { vw = maxw; vh = vw * (float)ih / (float)iw; }
                    ImGui::Image(vit->second, {vw, vh});
                }
            }
            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f, 5.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT, 1.f));
            if (ImGui::Button("Close")) {
                viewer_file_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.f, 8.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(0x38,0x1a,0x1a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x44,0x20,0x20, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x44,0x20,0x20, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_TXT, 1.f));
            if (ImGui::Button("Delete")) {
                DeleteFileA(viewer_file_.c_str());
                auto dit = icon_textures_.find("shot_" + vname);
                if (dit != icon_textures_.end()) {
                    if (dit->second) {
                        if (active_api_ == GraphicsAPI::DX11)
                            ((ID3D11ShaderResourceView*)dit->second)->Release();
                        else if (active_api_ == GraphicsAPI::DX9)
                            ((IDirect3DTexture9*)dit->second)->Release();
                    }
                    icon_textures_.erase(dit);
                }
                shots.clear();
                shots_refresh = 0;
                viewer_file_.clear();
                STAR_LOG("Screenshot deleted: %s", vname.c_str());
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto& stats = StarSteamUserStats::get();
    int total = (int)s.achievements.size();
    int done  = 0;
    for (auto& d : s.achievements) {
        bool got = false; stats.GetAchievement(d.name.c_str(), &got);
        if (got) done++;
    }

    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("ACHIEVEMENTS");
    ImGui::SameLine(0.f, 8.f);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("%d / %d", done, total);
    ImGui::PopStyleColor();

    {
        const char* lbl = "Test notify";
        float bw = ImGui::CalcTextSize(lbl).x + 14.f;
        ImGui::SameLine(PW - bw - 14.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT,    1.f));
        if (ImGui::Button(lbl)) {
            StarSteamUserStats::get().play_unlock_sound();
            push_achievement("Test Achievement",
                             "Opened the STAR overlay.",
                             {}, 0, 0);
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }

    ImGui::PopFont();

    if (total > 0) {
        float pct = (float)done / (float)total;
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float  bw  = PW - 28.f;
        dl->AddRectFilled(cur, {cur.x+bw, cur.y+3.f}, col(P_SEP, 0.6f), 2.f);
        dl->AddRectFilled(cur, {cur.x+bw*pct, cur.y+3.f}, acc(0.9f), 2.f);
        ImGui::Dummy({bw, 5.f});

        // Bulk actions (confirmation modal guards misclicks).
        float bw2 = (bw - 4.f) / 2.f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,       1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x1a,0x2c,0x44, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x1e,0x36,0x54, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          vacc(1.f));
        if (ImGui::Button("Unlock all", {bw2, 28.f})) {
            bulk_is_unlock_ = true;
            bulk_confirm_pending_ = true;
        }
        ImGui::PopStyleColor(4);
        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x38,0x1a,0x1a, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x44,0x20,0x20, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT, 1.f));
        if (ImGui::Button("Reset all", {bw2, 28.f})) {
            bulk_is_unlock_ = false;
            bulk_confirm_pending_ = true;
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        if (bulk_confirm_pending_) {
            ImGui::OpenPopup("##bulk_confirm");
            bulk_confirm_pending_ = false;
        }
        ImGui::SetNextWindowPos({sw * 0.5f, sh * 0.42f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, v4(P_BG1, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, v4(P_SEP, 0.8f));
        if (ImGui::BeginPopupModal("##bulk_confirm", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushFont(ftitle);
            ImGui::Text(bulk_is_unlock_ ? "Unlock all %d achievements?" : "Reset all %d achievements?", total);
            ImGui::PopFont();
            ImGui::PushFont(fsmall);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
            ImGui::Text("%s", bulk_is_unlock_
                ? "One summary toast, no sound spam."
                : "This cannot be undone.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f, 5.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT, 1.f));
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.f, 8.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        bulk_is_unlock_ ? vacc(0.22f) : v4(0x38,0x1a,0x1a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bulk_is_unlock_ ? vacc(0.3f)  : v4(0x44,0x20,0x20, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  vacc(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text,          bulk_is_unlock_ ? vacc(1.f) : v4(P_TXT, 1.f));
            if (ImGui::Button("Confirm")) {
                if (bulk_is_unlock_) {
                    stats.set_bulk_silent(true);
                    for (auto& d : s.achievements) stats.SetAchievement(d.name.c_str());
                    stats.set_bulk_silent(false);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%d achievements", total);
                    StarSteamUserStats::get().play_completion_sound();
                    std::vector<uint8_t> sum_rgba; int sum_w = 0, sum_h = 0;
                    StarSteamUtils::get().LoadSummaryIcon(sum_rgba, sum_w, sum_h);
                    push_achievement("All achievements unlocked", msg,
                        sum_rgba, sum_w, sum_h, "100% COMPLETE", true);
                    STAR_LOG("Bulk unlock all (%d)", total);
                } else {
                    for (auto& d : s.achievements) stats.ClearAchievement(d.name.c_str());
                    STAR_LOG("Bulk reset all (%d)", total);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
        ImGui::Text("+%d this session", session_unlocks_);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Display settings (live + persisted to overlay.star) ----
    // Styled like the filter tabs below: muted section label, accent-tinted
    // pill toggles, no default-ImGui widgets.
    {
        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::Text("DISPLAY");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::Spacing();
        {
            ImGui::PushFont(fsmall);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
            ImGui::Text("Accent");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        struct Swatch { const char* name; uint8_t r, g, b; };
        static const Swatch swatches[] = {
            { "blue",   0x4f, 0xa3, 0xff },
            { "red",    0xff, 0x5a, 0x5a },
            { "green",  0x4c, 0xb8, 0x4c },
            { "purple", 0xb0, 0x7f, 0xff },
            { "orange", 0xff, 0xa0, 0x3c },
            { "yellow", 0xff, 0xd4, 0x4d },
        };
        for (int i = 0; i < 6; i++) {
            if (i > 0) ImGui::SameLine(0.f, 4.f);
            ImGui::PushID(i);
            ImVec4 c = { swatches[i].r / 255.f, swatches[i].g / 255.f, swatches[i].b / 255.f, 1.f };
            if (ImGui::ColorButton("##acc", c, ImGuiColorEditFlags_NoTooltip, { 30.f, 30.f })) {
                s.overlay_accent = swatches[i].name;
                resolve_accent();
                ImGuiStyle& st = ImGui::GetStyle();
                st.Colors[ImGuiCol_ScrollbarGrabActive] = vacc(1.f);
                st.Colors[ImGuiCol_CheckMark] = vacc(1.f);
                st.Colors[ImGuiCol_SliderGrab] = vacc(1.f);
                save_overlay_key("accent", s.overlay_accent);
                STAR_LOG("Overlay accent -> %s", s.overlay_accent.c_str());
            }
            if (s.overlay_accent == swatches[i].name)
                dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), acc(1.f), 4.f, 0, 2.f);
            ImGui::PopID();
        }

        ImGui::Spacing();
        {
            const char* hud_lbl[3] = { "FPS", "Playtime", "Sound" };
            bool* hud_val[3] = { &s.overlay_show_fps, &s.overlay_show_playtime, &s.overlay_play_sound };
            const char* hud_key[3] = { "show_fps", "show_playtime", "play_sound" };
            float hud_avail = ImGui::GetContentRegionAvail().x;
            float hud_w = (hud_avail - 8.f) / 3.f;
            for (int i = 0; i < 3; i++) {
                if (i > 0) ImGui::SameLine(0.f, 4.f);
                bool active = *hud_val[i];
                ImGui::PushStyleColor(ImGuiCol_Button,        active ? vacc(0.22f) : v4(P_BG2, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  vacc(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text,          active ? vacc(1.f) : v4(P_MUT, 1.f));
                if (ImGui::Button(hud_lbl[i], {hud_w, 30.f})) {
                    *hud_val[i] = !*hud_val[i];
                    save_overlay_key(hud_key[i], *hud_val[i] ? "true" : "false");
                }
                ImGui::PopStyleColor(4);
            }
        }

        ImGui::Spacing();
        {
            ImGui::PushFont(fsmall);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
            ImGui::Text("Toast corner");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0.f, 8.f);
            struct Corner { const char* label; const char* value; };
            static const Corner corners[] = {
                { "TL", "top_left" }, { "TR", "top_right" },
                { "BL", "bottom_left" }, { "BR", "bottom_right" },
            };
            for (int i = 0; i < 4; i++) {
                if (i > 0) ImGui::SameLine(0.f, 4.f);
                bool active = (s.overlay_notify_pos == corners[i].value);
                ImGui::PushStyleColor(ImGuiCol_Button,        active ? vacc(0.22f) : v4(P_BG2, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  vacc(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text,          active ? vacc(1.f) : v4(P_MUT, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
                if (ImGui::Button(corners[i].label)) {
                    s.overlay_notify_pos = corners[i].value;
                    save_overlay_key("notify_pos", s.overlay_notify_pos);
                    STAR_LOG("Overlay notify corner -> %s", s.overlay_notify_pos.c_str());
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                if (active)
                    dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), acc(0.7f), 3.f, 0, 1.5f);
            }
        }
    }

    ImGui::Spacing();

    // ---- Per-game notes (STAR/notes.txt, travels with the game copy) ----
    {
        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::Text("NOTES");
        ImGui::SameLine(0.f, 8.f);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, notes_dirty_ ? vacc(1.f) : v4(P_DIM, 1.f));
        ImGui::Text("%s", notes_dirty_ ? "(unsaved)" : "(auto-saved)");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        static char buf[8192] = {};
        static bool buf_init = false;
        if (!buf_init) {
            buf_init = true;
            strncpy_s(buf, notes_text_.c_str(), _TRUNCATE);
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(P_BG1, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextMultiline("##notes", buf, sizeof(buf), { -1.f, 110.f },
                ImGuiInputTextFlags_AllowTabInput)) {
            notes_text_ = buf;
            notes_dirty_ = true;
            notes_last_edit_ = GetTickCount();
        }
        ImGui::PopStyleColor(2);

        if (notes_dirty_ && GetTickCount() - notes_last_edit_ > 2000)
            save_notes();
    }

    ImGui::Spacing();

    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(P_BG1, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##filter", "Search achievements...",
            achievement_filter_, sizeof(achievement_filter_));
        ImGui::PopStyleColor(2);

        const char* tabs[3] = { "All", "Unlocked", "Locked" };
        float avail = ImGui::GetContentRegionAvail().x;
        float tab_w = (avail - 8.f) / 3.f;
        for (int i = 0; i < 3; i++) {
            if (i > 0) ImGui::SameLine(0.f, 4.f);
            bool active = filter_mode_ == i;
            ImGui::PushStyleColor(ImGuiCol_Button,        active ? vacc(0.22f) : v4(P_BG2, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  vacc(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text,          active ? vacc(1.f) : v4(P_MUT, 1.f));
            if (ImGui::Button(tabs[i], {tab_w, 30.f})) filter_mode_ = i;
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::BeginChild("##ach", {0, 0}, false, 0);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float actual_scroll_y = ImGui::GetScrollY();
        if (scroll_target_y_ < 0.f) {
            scroll_target_y_ = actual_scroll_y;
            scroll_current_y_ = actual_scroll_y;
        }
        float diff_scroll = actual_scroll_y - scroll_current_y_;
        if (diff_scroll < 0.f) diff_scroll = -diff_scroll;
        if (diff_scroll > 2.f) {
            scroll_target_y_ = actual_scroll_y;
            scroll_current_y_ = actual_scroll_y;
        }
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            scroll_target_y_ -= wheel * 100.f;
            float max_scroll_y = ImGui::GetScrollMaxY();
            if (scroll_target_y_ < 0.f) scroll_target_y_ = 0.f;
            if (scroll_target_y_ > max_scroll_y) scroll_target_y_ = max_scroll_y;
        }
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.f) dt = 0.0167f;
        scroll_current_y_ += (scroll_target_y_ - scroll_current_y_) * clamp01(12.f * dt);
        float diff_target = scroll_current_y_ - scroll_target_y_;
        if (diff_target < 0.f) diff_target = -diff_target;
        if (diff_target > 0.1f) {
            ImGui::SetScrollY(scroll_current_y_);
        } else {
            ImGui::SetScrollY(scroll_target_y_);
            scroll_current_y_ = scroll_target_y_;
        }

    const float S = ui_scale_;
    const float ROW_BASE = 64.f;
    const float ICON_S = 44.f;
    const float ICON_X = 12.f;

    std::string needle = achievement_filter_;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    int shown = 0;
    for (auto& def : s.achievements) {
        bool got = false;
        uint32_t unlock_t = 0;
        stats.GetAchievementAndUnlockTime(def.name.c_str(), &got, &unlock_t);

        if (filter_mode_ == 1 && !got) continue;
        if (filter_mode_ == 2 && got) continue;
        if (!needle.empty()) {
            std::string hay = (!def.display_name.empty()) ? def.display_name : def.name;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        shown++;

        ImVec2 rmin = ImGui::GetCursorScreenPos();
        float  rw   = ImGui::GetContentRegionAvail().x;

        float tx_probe = ICON_X + ICON_S + 12.f * S;
        float tw_probe = rw - tx_probe - 92.f * S;

        bool is_hidden = def.hidden && !got;
        const char* raw_name = is_hidden ? "(Hidden achievement)"
            : ((!def.display_name.empty()) ? def.display_name.c_str() : def.name.c_str());
        const char* raw_desc = (!def.description.empty()) ? def.description.c_str() : nullptr;
        std::string d1, d2;
        int desc_lines = (!is_hidden && raw_desc)
            ? wrap_two_lines(fsmall, 15.f * S, raw_desc, tw_probe, d1, d2) : 0;
        float ROW_H = (desc_lines == 2 ? 80.f : ROW_BASE) * S;

        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(("##r_"+def.name).c_str(), {rw, ROW_H});
        bool hovered = ImGui::IsItemHovered();

        if (hovered)
            dl->AddRectFilled(rmin, {rmin.x+rw, rmin.y+ROW_H}, col(P_BG2, 0.6f));

        float ix2 = rmin.x + ICON_X;
        float iy2 = rmin.y + (ROW_H - ICON_S) * .5f;

        std::string ikey = (got ? "p_" : "g_") + def.name;
        ImTextureID icon_tex = nullptr;
        auto icit = icon_textures_.find(ikey);
        if (icit != icon_textures_.end()) {
            icon_tex = icit->second;
        } else {
            // Preferred icon for this state, falling back to the other one so a
            // missing icongray doesn't grey out everything.
            std::string icon_path = got ? def.icon_path : def.icon_gray_path;
            if (icon_path.empty()) icon_path = got ? def.icon_gray_path : def.icon_path;
            if (!icon_path.empty()) {
                std::string full = Settings::get().settings_dir + "\\" + icon_path;
                for (char& c : full) if (c == '/') c = '\\';
                enqueue_icon_decode(full, ikey);
            }
        }
        if (icon_tex) {
            dl->AddImageRounded(icon_tex,
                {ix2,iy2},{ix2+ICON_S,iy2+ICON_S},{0,0},{1,1},
                IM_COL32(255,255,255, 255), 3.f);
        } else {
            dl->AddRectFilled({ix2,iy2},{ix2+ICON_S,iy2+ICON_S},
                col(P_BG2, 1.f), 3.f);
        }
        if (got)
            dl->AddRect({ix2-1.5f,iy2-1.5f},{ix2+ICON_S+1.5f,iy2+ICON_S+1.5f},
                acc(0.6f), 4.f, 0, 1.5f);

        float dot_x = ix2 + ICON_S - 6.f, dot_y = iy2;
        dl->AddCircleFilled({dot_x, dot_y}, 5.f,
            got ? col(P_GRN, 1.f) : col(P_DIM, 0.f));

        float tx  = ix2 + ICON_S + 12.f * S;
        float ty0 = rmin.y + 11.f * S;
        float tw  = rw - (tx - rmin.x) - 92.f * S;

        ImU32 name_col = got ? col(P_TXT, 1.f) : col(P_MUT, 1.f);
        dl->PushClipRect({tx,ty0},{tx+tw,ty0+22.f*S},true);
        dl->AddText(ftitle, 17.f * S, {tx,ty0}, name_col, raw_name);
        dl->PopClipRect();

        if (is_hidden) {
            // Blurred-out look: dim bars where the description would be.
            float dy = ty0 + 23.f * S;
            dl->AddRectFilled({tx,dy},{tx+tw*0.7f,dy+12.f*S}, col(P_DIM, 0.45f), 3.f);
        } else if (desc_lines >= 1) {
            float dy = ty0 + 23.f * S;
            dl->PushClipRect({tx,dy},{tx+tw,dy+40.f*S},true);
            dl->AddText(fsmall, 15.f * S, {tx,dy}, col(P_LGT, 1.f), d1.c_str());
            if (desc_lines == 2)
                dl->AddText(fsmall, 15.f * S, {tx,dy+19.f*S}, col(P_LGT, 1.f), d2.c_str());
            dl->PopClipRect();
        }

        float btn_x = rmin.x + rw - 84.f * S;
        float btn_y = rmin.y + (ROW_H - 26.f * S) * .5f;

        ImGui::SetCursorScreenPos({btn_x, btn_y});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});

        if (got) {
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x38,0x1a,0x1a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x44,0x20,0x20, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT, 1.f));
            if (ImGui::Button(("Reset##" + def.name).c_str(), {72.f * S, 26.f * S}))
                stats.ClearAchievement(def.name.c_str());
            ImGui::PopStyleColor(4);
            if (unlock_t) {
                std::string ts = fmt_unlock_time(unlock_t);
                ImVec2 tsz = fsmall->CalcTextSizeA(13.f * S, FLT_MAX, 0.f, ts.c_str());
                dl->AddText(fsmall, 13.f * S,
                    {btn_x + 72.f * S - tsz.x, btn_y + 30.f * S},
                    col(P_MUT, 1.f), ts.c_str());
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,       1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x1a,0x2c,0x44, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x1e,0x36,0x54, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          vacc(1.f));
            if (ImGui::Button(("Unlock##" + def.name).c_str(), {72.f * S, 26.f * S}))
                stats.SetAchievement(def.name.c_str());
            ImGui::PopStyleColor(4);
        }
        ImGui::PopStyleVar();

        ImGui::SetCursorScreenPos({rmin.x, rmin.y + ROW_H});
        ImVec2 sep_p = ImGui::GetCursorScreenPos();
        dl->AddLine({sep_p.x, sep_p.y}, {sep_p.x+rw, sep_p.y}, col(P_SEP, 0.3f));
    }

    if (s.achievements.empty() || shown == 0) {
        const char* msg = s.achievements.empty()
            ? "No achievements in STAR/achievements.json"
            : "No achievements match your search";
        ImGui::Dummy({0.f, 16.f});
        ImGui::PushFont(fsmall);
        float msg_w = ImGui::CalcTextSize(msg).x;
        float avail_w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - msg_w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar();
}

void StarOverlay::push_achievement(const std::string& name, const std::string& desc,
                                   const std::vector<uint8_t>& rgba, int iw, int ih,
                                   const std::string& header, bool summary)
{
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(notif_mutex_);
    AchievementNotification n;
    n.header = header; n.summary = summary; n.title = name; n.description = desc;
    n.icon_rgba = rgba; n.icon_width = iw; n.icon_height = ih;
    n.time_remaining = 5.f; n.age = 0.f;
    notifications_.push_back(std::move(n));
}

void StarOverlay::request_screenshot()
{
    if (!enabled_) return;
    screenshot_requested_ = true;
    STAR_LOG("Screenshot requested");
}

std::string StarOverlay::screenshots_dir()
{
    // Per-game subfolder so shots never mix across titles.
    // Falls back to the game-local STAR folder when Documents is unavailable.
    std::string leaf = "\\STAR\\screenshots\\" + std::to_string(Settings::get().app_id);
    char docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)) && docs[0])
        return std::string(docs) + leaf;
    return Settings::get().settings_dir + leaf;
}

std::string StarOverlay::next_screenshot_path()
{
    std::string dir = screenshots_dir();
    Storage::ensure_dir(dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char base[128];
    snprintf(base, sizeof(base), "STAR_%u_%04d%02d%02d_%02d%02d%02d",
        Settings::get().app_id, (int)st.wYear, (int)st.wMonth, (int)st.wDay,
        (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    for (int i = 0; i < 100; i++) {
        char full[MAX_PATH];
        if (i == 0) snprintf(full, sizeof(full), "%s\\%s.png", dir.c_str(), base);
        else snprintf(full, sizeof(full), "%s\\%s_%d.png", dir.c_str(), base, i);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) return full;
    }
    return "";
}

void StarOverlay::notify_screenshot(const std::string& file, bool dark)
{
    std::string name = file;
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // Toast shows immediately; the thumbnail arrives when the background
    // decode finishes.
    push_achievement(name,
        dark ? "All black? Try Borderless mode."
             : "Saved to " + screenshots_dir(),
        {}, 0, 0, "SCREENSHOT SAVED");
    enqueue_icon_decode(file, name, name);
    // Pop the viewer so the user gets an instant preview on next panel open.
    viewer_file_ = file;
    viewer_pending_ = true;
    STAR_LOG("Screenshot saved: %s", file.c_str());
}

void StarOverlay::enqueue_icon_decode(const std::string& path, const std::string& key,
                                      const std::string& toast_title)
{
    if (path.empty() || key.empty()) return;
    {
        std::lock_guard<std::mutex> lock(icon_decode_mutex_);
        if (!icon_decode_pending_.insert(key).second) return;
        icon_decode_queue_.push_back({path, key, toast_title});
    }
    icon_decode_cv_.notify_one();
}

void StarOverlay::icon_decode_worker()
{
    for (;;) {
        IconDecodeRequest req;
        {
            std::unique_lock<std::mutex> lock(icon_decode_mutex_);
            icon_decode_cv_.wait(lock, [&] {
                return icon_decode_stop_ || !icon_decode_queue_.empty();
            });
            if (icon_decode_stop_ && icon_decode_queue_.empty()) break;
            req = std::move(icon_decode_queue_.front());
            icon_decode_queue_.pop_front();
        }
        IconDecodeResult res;
        res.key = req.key;
        res.toast_title = req.toast_title;
        StarSteamUtils::get().LoadIconFile(req.path, res.rgba, res.w, res.h);
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_ready_.push_back(std::move(res));
        }
    }
}

void StarOverlay::drain_icon_decodes()
{
    std::vector<IconDecodeResult> ready;
    {
        std::lock_guard<std::mutex> lock(icon_decode_mutex_);
        ready.swap(icon_decode_ready_);
    }
    for (auto& r : ready) {
        if (r.w > 0 && r.h > 0 && !r.rgba.empty())
            get_or_create_icon(r.key, r.rgba, r.w, r.h);
        if (!r.toast_title.empty()) {
            std::lock_guard<std::mutex> lock(notif_mutex_);
            for (auto& n : notifications_) {
                if (n.title == r.toast_title) {
                    n.icon_rgba = std::move(r.rgba);
                    n.icon_width = r.w;
                    n.icon_height = r.h;
                    break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(icon_decode_mutex_);
            icon_decode_pending_.erase(r.key);
        }
    }
}

bool StarOverlay::save_rgba_png(const std::string& path, const uint8_t* rgba, int w, int h)
{
    if (path.empty() || !rgba || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    // Screenshots must be opaque: backbuffers often carry garbage/zero in
    // alpha (X8 formats, GL default framebuffer), which viewers show as
    // black or checkered "strange colors".
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        px[i * 4 + 0] = rgba[i * 4 + 0];
        px[i * 4 + 1] = rgba[i * 4 + 1];
        px[i * 4 + 2] = rgba[i * 4 + 2];
        px[i * 4 + 3] = 255;
    }
    const uint8_t* data = px.data();
    bool com_here = false;
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (cohr == S_OK) com_here = true;
    else if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory, (void**)&factory)) && factory) {
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath((size_t)(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
        if (SUCCEEDED(factory->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && encoder &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && frame &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize((UINT)w, (UINT)h))) {
            GUID fmt = GUID_WICPixelFormat32bppRGBA;
            // NOTE: the PNG encoder may coerce this (typically to BGRA);
            // the buffer must match whatever comes back, or R/B swap.
            if (SUCCEEDED(frame->SetPixelFormat(&fmt))) {
                const uint8_t* src = data;
                std::vector<uint8_t> bgra;
                bool fmt_ok = true;
                if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
                    bgra.resize((size_t)w * h * 4);
                    for (size_t i = 0; i < (size_t)w * h; i++) {
                        bgra[i * 4 + 0] = data[i * 4 + 2];
                        bgra[i * 4 + 1] = data[i * 4 + 1];
                        bgra[i * 4 + 2] = data[i * 4 + 0];
                        bgra[i * 4 + 3] = data[i * 4 + 3];
                    }
                    src = bgra.data();
                } else if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
                    STAR_LOG("Screenshot: unexpected pixel format, aborting %s", path.c_str());
                    fmt_ok = false;
                }
                if (fmt_ok &&
                    SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)(w * h * 4), (BYTE*)src)) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
                    ok = true;
                }
            }
        }
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        factory->Release();
    }
    if (com_here) CoUninitialize();
    if (!ok) STAR_LOG("Screenshot encode failed: %s", path.c_str());
    return ok;
}

// Captures run AFTER the overlay draws, so screenshots show what the user
// sees (panel, toasts, HUD included).

void StarOverlay::maybe_capture_dx11(IDXGISwapChain* chain)
{
    if (!screenshot_requested_.exchange(false)) return;
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
        const uint8_t* srow = (const uint8_t*)map.pData;
        for (UINT y = 0; y < desc.Height; y++) {
            uint8_t* d = rgba.data() + (size_t)y * desc.Width * 4;
            if (bgra) {
                const uint8_t* s = srow + (size_t)y * map.RowPitch;
                for (UINT x = 0; x < desc.Width; x++) {
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    s += 4; d += 4;
                }
            } else {
                memcpy(d, srow + (size_t)y * map.RowPitch, (size_t)desc.Width * 4);
            }
        }
        context_->Unmap(staging, 0);
        path = next_screenshot_path();
        if (!path.empty() && save_rgba_png(path, rgba.data(), (int)desc.Width, (int)desc.Height))
            notify_screenshot(path);
    }
    staging->Release();
    if (resolved) resolved->Release();
    bb->Release();
}

void StarOverlay::maybe_capture_dx9(IDirect3DDevice9* device)
{
    if (!screenshot_requested_.exchange(false)) return;
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
        for (UINT y = 0; y < desc.Height; y++) {
            const uint8_t* s = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
            uint8_t* d = rgba.data() + (size_t)y * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; x++) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                s += 4; d += 4;
            }
        }
        sys->UnlockRect();
        std::string path = next_screenshot_path();
        if (!path.empty() && save_rgba_png(path, rgba.data(), (int)desc.Width, (int)desc.Height))
            notify_screenshot(path);
    }
    sys->Release();
}

void StarOverlay::maybe_capture_opengl()
{
    if (!screenshot_requested_.exchange(false)) return;
    if (!enabled_) return;
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) return;
    typedef void(WINAPI* glGetIntegervFn)(unsigned int, int*);
    typedef void(WINAPI* glReadPixelsFn)(int, int, int, int, unsigned int, unsigned int, void*);
    auto glGetIntegerv = (glGetIntegervFn)GetProcAddress(opengl_dll, "glGetIntegerv");
    auto glReadPixels = (glReadPixelsFn)GetProcAddress(opengl_dll, "glReadPixels");
    if (!glGetIntegerv || !glReadPixels) return;
    const unsigned int GL_VIEWPORT = 0x0BA2;
    const unsigned int GL_RGBA = 0x1908;
    const unsigned int GL_UNSIGNED_BYTE = 0x1401;
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
    std::string path = next_screenshot_path();
    if (!path.empty() && save_rgba_png(path, rgba.data(), vp[2], vp[3]))
        notify_screenshot(path);
}

void StarOverlay::capture_desktop_duplication()
{
    // Zero game-state interaction: own D3D11 device + DXGI desktop duplication.
    // For titles whose buffers fault on any foreign access (ACEVO), and any
    // exclusive-fullscreen game. Captures the primary output.
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
    IDXGIOutput1* out1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    output->Release();
    adapter->Release();
    factory->Release();
    if (FAILED(hr) || !out1) {
        if (ctx) ctx->Release();
        if (ddev) ddev->Release();
        return;
    }
    IDXGIOutputDuplication* dup = nullptr;
    hr = out1->DuplicateOutput(ddev, &dup);
    out1->Release();
    if (FAILED(hr) || !dup) {
        static bool logged_dup = false;
        if (!logged_dup) {
            logged_dup = true;
            STAR_LOG("Screenshot: desktop duplication unavailable hr=0x%08x", (unsigned)hr);
        }
        if (ctx) ctx->Release();
        if (ddev) ddev->Release();
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
        if (ctx) ctx->Release();
        if (ddev) ddev->Release();
        return;
    }
    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr) || !tex) {
        dup->ReleaseFrame();
        dup->Release();
        if (ctx) ctx->Release();
        if (ddev) ddev->Release();
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
        if (ctx) ctx->Release();
        if (ddev) ddev->Release();
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
            std::vector<uint8_t> rgba((size_t)desc.Width * desc.Height * 4);
            uint64_t bright = 0;
            for (UINT y = 0; y < desc.Height; y++) {
                const uint8_t* s = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
                uint8_t* d = rgba.data() + (size_t)y * desc.Width * 4;
                if (bgra) {
                    for (UINT x = 0; x < desc.Width; x++) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        bright += (uint64_t)d[0] + d[1] + d[2];
                        s += 4; d += 4;
                    }
                } else {
                    memcpy(d, s, (size_t)desc.Width * 4);
                    for (size_t i = 0; i < (size_t)desc.Width * 4; i += 4)
                        bright += (uint64_t)d[i] + d[i + 1] + d[i + 2];
                }
            }
            ctx->Unmap(staging, 0);
            std::string path = next_screenshot_path();
            // All-black desktop frame = exclusive/independent-flip fullscreen
            // bypassing DWM; tell the user how to fix it instead of silence.
            double mean = (double)bright / ((double)desc.Width * desc.Height * 3.0);
            bool dark = mean < 4.0;
            if (dark) STAR_LOG("Screenshot looks black (exclusive fullscreen?)");
            if (!path.empty() && save_rgba_png(path, rgba.data(), (int)desc.Width, (int)desc.Height))
                notify_screenshot(path, dark);
        }
        staging->Release();
    }
    tex->Release();
    dup->ReleaseFrame();
    dup->Release();
    if (ctx) ctx->Release();
    if (ddev) ddev->Release();
}

#ifdef _WIN64
void StarOverlay::maybe_capture_dx12(IDXGISwapChain* chain)
{
    if (!screenshot_requested_.exchange(false)) return;
    if (!enabled_) return;
    // Render is off (hostile titles): read the composed desktop instead of
    // touching the game's buffers at all.
    if (!Settings::get().overlay_dx12_render) {
        capture_desktop_duplication();
        return;
    }
    if (!imgui_initialized_ || active_api_ != GraphicsAPI::DX12) return;
    auto* dev = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    if (!dev || !queue) return;
    IDXGISwapChain3* chain3 = nullptr;
    UINT bi = 0;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&chain3))) {
        bi = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    } else {
        return;
    }
    if (bi >= dx12_resources_.size()) return;
    auto* resource = (ID3D12Resource*)dx12_resources_[bi];
    D3D12_RESOURCE_DESC rd = resource->GetDesc();
    bool bgra = (rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    if (rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && !bgra) {
        STAR_LOG("Screenshot: unsupported DX12 format %u", (unsigned)rd.Format);
        return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowsize = 0, total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowsize, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) || !readback)
        return;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT ok = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(ok)) ok = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    if (FAILED(ok) || !list) {
        if (alloc) alloc->Release();
        readback->Release();
        return;
    }
    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = resource;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b1 = b0;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b1);
    list->Close();
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence) {
        if (SUCCEEDED(queue->Signal(fence, 1)) && fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                fence->SetEventOnCompletion(1, ev);
                WaitForSingleObject(ev, 3000);
                CloseHandle(ev);
            }
        }
        fence->Release();
    }
    void* mapped = nullptr;
    D3D12_RANGE range{};
    range.Begin = 0; range.End = (SIZE_T)total;
    if (SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped) {
        UINT w = (UINT)rd.Width, h = rd.Height;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++) {
            const uint8_t* s = (const uint8_t*)mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
            uint8_t* d = rgba.data() + (size_t)y * w * 4;
            if (bgra) {
                for (UINT x = 0; x < w; x++) {
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    s += 4; d += 4;
                }
            } else {
                memcpy(d, s, (size_t)w * 4);
            }
        }
        D3D12_RANGE empty{};
        empty.Begin = 0; empty.End = 0;
        readback->Unmap(0, &empty);
        std::string path = next_screenshot_path();
        if (!path.empty() && save_rgba_png(path, rgba.data(), (int)w, (int)h))
            notify_screenshot(path);
    }
    list->Release();
    alloc->Release();
    readback->Release();
}
#endif

#ifdef _WIN64
void STDMETHODCALLTYPE StarOverlay::hooked_ExecuteCommandLists(void* queue, UINT count, void* const* lists)
{
    // Only the DIRECT (graphics) queue can do our render-target work. Games
    // routinely execute copy/compute queues first (uploads during loading);
    // capturing one of those and issuing graphics barriers on it faults.
    if (!g_dx12_captured_queue_) {
        auto* q = (ID3D12CommandQueue*)queue;
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_dx12_captured_queue_ = queue;
            STAR_LOG("DX12 command queue captured");
        }
    }
    if (g_overlay && g_overlay->orig_execute_command_lists_)
        g_overlay->orig_execute_command_lists_(queue, count, lists);
}
#endif

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present(IDXGISwapChain* sc, UINT si, UINT fl)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
        g_overlay->on_present(sc, si, fl);
    }
    return g_overlay && g_overlay->orig_present_ ? g_overlay->orig_present_(sc, si, fl) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_Present1(
    IDXGISwapChain1* sc, UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (g_overlay && !(fl & DXGI_PRESENT_TEST)) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
        g_overlay->on_present(sc, si, fl);
    }
    return g_overlay && g_overlay->orig_present1_ ? g_overlay->orig_present1_(sc, si, fl, pp) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_ResizeBuffers(
    IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    if (g_overlay) g_overlay->on_resize_buffers(sc, bc, w, h, fmt, fl);
    return g_overlay ? g_overlay->orig_resize_(sc, bc, w, h, fmt, fl) : S_OK;
}

void StarOverlay::on_present(IDXGISwapChain* chain, UINT si, UINT fl)
{
    STAR_UNREFERENCED(si); STAR_UNREFERENCED(fl);
    if (!enabled_) return;
    // Window recreated (mode switch / multi-window): re-hook so input keeps working.
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(chain->GetDesc(&sd))) hook_window_for(sd.OutputWindow);
    }

    // Game API detection (once): DXGI present + D3D11 device = DX11, else DX12.
    if (game_api_ == GraphicsAPI::None) {
        ID3D11Device* probe = nullptr;
        if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device), (void**)&probe))) {
            probe->Release();
            game_api_ = GraphicsAPI::DX11;
            STAR_LOG("Game graphics API: DirectX 11");
        } else {
            game_api_ = GraphicsAPI::DX12;
            STAR_LOG("Game graphics API: DirectX 12");
        }
    }
    // External mode only sniffs (for the label + input); all drawing lives
    // in the external window. Hook rendering stays off entirely.
    if (mode_ == OverlayMode::External) return;

    // Vulkan games also present via DXGI (flip model) but render via Vulkan.
    // The DX12 path would draw into the same backbuffer Unity presents via
    // Vulkan with no shared sync -> GPU hang / game freeze. The Vulkan
    // present hook owns rendering for these games.
    if (game_api_ == GraphicsAPI::Vulkan) return;

    if (!imgui_initialized_) {
        static bool logged_attempt = false;
        if (!logged_attempt) { logged_attempt = true; STAR_LOG("on_present: attempting imgui init"); }
        ID3D11Device* d3d11_device = nullptr;
        if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11_device))) {
            d3d11_device->Release();
            init_imgui(chain);
        } else {
            static bool logged_dx11_fail = false;
            if (!logged_dx11_fail) { logged_dx11_fail = true; STAR_LOG("on_present: DX11 device not found, using DX12 path"); }
#ifdef _WIN64
            if (!orig_execute_command_lists_) {
                hook_dx12_ecl();
                return;
            }
            if (g_dx12_captured_queue_) {
                ID3D12Device* d3d12_device = nullptr;
                auto* queue = (ID3D12CommandQueue*)g_dx12_captured_queue_;
                if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12_device))) {
                    init_imgui_dx12(chain, d3d12_device, g_dx12_captured_queue_);
                    d3d12_device->Release();
                } else {
                    static bool logged_queue_fail = false;
                    if (!logged_queue_fail) { logged_queue_fail = true; STAR_LOG("on_present: DX12 queue->GetDevice failed"); }
                }
            }
#endif
        }
    }

    if (imgui_initialized_) {
        if (active_api_ == GraphicsAPI::DX11) {
            render_frame(chain);
#ifdef _WIN64
        } else if (active_api_ == GraphicsAPI::DX12) {
            // Capture first: with dx12_render=false this is the ONLY consumer
            // (render_frame_dx12 early-returns), routing to desktop duplication.
            maybe_capture_dx12(chain);
            render_frame_dx12(chain);
#endif
        }
    }
}

void StarOverlay::on_resize_buffers(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl)
{
    STAR_UNREFERENCED(sc); STAR_UNREFERENCED(bc); STAR_UNREFERENCED(w);
    STAR_UNREFERENCED(h);  STAR_UNREFERENCED(fmt); STAR_UNREFERENCED(fl);
    // External mode owns its own RTV; a game-chain resize must never touch it.
    if (mode_ == OverlayMode::External) return;
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (active_api_ == GraphicsAPI::DX11) {
        cleanup_rtv();
#ifdef _WIN64
    } else if (active_api_ == GraphicsAPI::DX12) {
        cleanup_dx12();
        imgui_initialized_ = false;
#endif
    }
}

#ifdef _WIN64
void StarOverlay::init_imgui_dx12(IDXGISwapChain* chain, void* device, void* command_queue)
{
    auto* dev = (ID3D12Device*)device;
    auto* queue = (ID3D12CommandQueue*)command_queue;

    DXGI_SWAP_CHAIN_DESC sd{};
    chain->GetDesc(&sd);
    hook_window_for(sd.OutputWindow);
    STAR_LOG("init_imgui_dx12: buffers=%u fmt=%u hwnd=%p", sd.BufferCount, sd.BufferDesc.Format, hwnd_);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = sd.BufferCount;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ID3D12DescriptorHeap* rtv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) { STAR_LOG("init_imgui_dx12: rtv_heap failed"); return; }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 257;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ID3D12DescriptorHeap* srv_heap = nullptr;
    if (FAILED(dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)))) {
        STAR_LOG("init_imgui_dx12: srv_heap failed");
        rtv_heap->Release();
        return;
    }

    std::vector<ID3D12Resource*> resources(sd.BufferCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    UINT rtv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < sd.BufferCount; i++) {
        if (SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&resources[i])))) {
            dev->CreateRenderTargetView(resources[i], nullptr, rtv_handle);
            rtv_handle.ptr += rtv_descriptor_size;
        }
    }

    std::vector<ID3D12CommandAllocator*> allocators(sd.BufferCount);
    for (UINT i = 0; i < sd.BufferCount; i++) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])))) { STAR_LOG("init_imgui_dx12: allocator[%u] failed", i); return; }
    }

    ID3D12GraphicsCommandList* cmd_list = nullptr;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&cmd_list)))) { STAR_LOG("init_imgui_dx12: cmd_list failed"); return; }
    cmd_list->Close();

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd_);
    hook_window();

    IMGUI_CHECKVERSION();
    STAR_LOG("init_imgui_dx12: calling ImGui_ImplDX12_Init");
    if (ImGui_ImplDX12_Init(dev, sd.BufferCount, sd.BufferDesc.Format, srv_heap,
                            srv_heap->GetCPUDescriptorHandleForHeapStart(),
                            srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        setup_imgui_style_and_fonts();
        dev->AddRef();
        queue->AddRef();
        dx12_device_ = dev;
        dx12_command_queue_ = queue;
        dx12_rtv_heap_ = rtv_heap;
        dx12_srv_heap_ = srv_heap;
        dx12_command_list_ = cmd_list;
        dx12_buffer_count_ = sd.BufferCount;

        dx12_command_allocators_.resize(sd.BufferCount);
        for (UINT i = 0; i < sd.BufferCount; i++) dx12_command_allocators_[i] = allocators[i];

        dx12_resources_.resize(sd.BufferCount);
        for (UINT i = 0; i < sd.BufferCount; i++) dx12_resources_[i] = resources[i];

        ID3D12Fence* frame_fence = nullptr;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence))) || !frame_fence) {
            STAR_LOG("init_imgui_dx12: frame fence failed, cleaning up");
            cleanup_dx12();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            STAR_LOG("init_imgui_dx12: fence event failed, cleaning up");
            frame_fence->Release();
            cleanup_dx12();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        dx12_fence_ = frame_fence;
        dx12_fence_value_ = 0;
        dx12_frame_fence_.assign(sd.BufferCount, 0);
        dx12_fence_event_ = fence_event;

        dx12_srv_next_slot_ = 1;
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::DX12;
        STAR_LOG("ImGui ready (DX12)");
    } else {
        STAR_LOG("init_imgui_dx12: ImGui_ImplDX12_Init FAILED - BackendRendererUserData=%p",
                 ImGui::GetIO().BackendRendererUserData);
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

ImTextureID StarOverlay::upload_icon_dx12(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* dev   = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* heap  = (ID3D12DescriptorHeap*)dx12_srv_heap_;
    if (!dev || !queue || !heap || dx12_srv_next_slot_ >= 257) return nullptr;

    UINT row_pitch     = (UINT)(w * 4);
    UINT aligned_pitch = (row_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 upload_size = (UINT64)aligned_pitch * h;

    D3D12_HEAP_PROPERTIES upload_props = {};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension  = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width      = upload_size;
    buf_desc.Height     = buf_desc.DepthOrArraySize = buf_desc.MipLevels = 1;
    buf_desc.Format     = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout     = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload_buf = nullptr;
    if (FAILED(dev->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE,
                                            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&upload_buf))))
        return nullptr;

    void* mapped = nullptr;
    if (FAILED(upload_buf->Map(0, nullptr, &mapped))) { upload_buf->Release(); return nullptr; }
    for (int row = 0; row < h; row++)
        memcpy((uint8_t*)mapped + (size_t)row * aligned_pitch, rgba.data() + (size_t)row * row_pitch, row_pitch);
    upload_buf->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES default_props = {};
    default_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = (UINT64)w;
    tex_desc.Height             = (UINT)h;
    tex_desc.DepthOrArraySize   = tex_desc.MipLevels = 1;
    tex_desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* texture = nullptr;
    if (FAILED(dev->CreateCommittedResource(&default_props, D3D12_HEAP_FLAG_NONE,
                                            &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&texture)))) {
        upload_buf->Release(); return nullptr;
    }

    ID3D12CommandAllocator*    tmp_alloc = nullptr;
    ID3D12GraphicsCommandList* tmp_list  = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmp_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmp_alloc, nullptr, IID_PPV_ARGS(&tmp_list)))) {
        if (tmp_alloc) tmp_alloc->Release();
        texture->Release(); upload_buf->Release(); return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource        = texture;
    dst_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource                            = upload_buf;
    src_loc.Type                                 = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Footprint.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width      = (UINT)w;
    src_loc.PlacedFootprint.Footprint.Height     = (UINT)h;
    src_loc.PlacedFootprint.Footprint.Depth      = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch   = aligned_pitch;

    tmp_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tmp_list->ResourceBarrier(1, &barrier);
    tmp_list->Close();

    ID3D12CommandList* lists[] = { tmp_list };
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        tmp_list->Release(); tmp_alloc->Release(); upload_buf->Release();
        texture->Release(); return nullptr;
    }
    if (FAILED(queue->Signal(fence, 1))) {
        fence->Release();
        tmp_list->Release(); tmp_alloc->Release(); upload_buf->Release();
        texture->Release(); return nullptr;
    }
    if (fence->GetCompletedValue() < 1) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
    fence->Release();
    tmp_list->Release();
    tmp_alloc->Release();
    upload_buf->Release();

    UINT desc_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (UINT64)dx12_srv_next_slot_ * desc_inc;
    gpu.ptr += (UINT64)dx12_srv_next_slot_ * desc_inc;
    dx12_srv_next_slot_++;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels       = 1;
    dev->CreateShaderResourceView(texture, &srv_desc, cpu);

    dx12_icon_resources_.push_back(texture);
    return (ImTextureID)(void*)(UINT64)gpu.ptr;
}

void StarOverlay::render_frame_dx12(IDXGISwapChain* chain)
{
    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!imgui_initialized_) return;
    // Escape hatch: API emulation without any DX12 drawing.    if (!Settings::get().overlay_dx12_render) return;

    IDXGISwapChain3* chain3 = nullptr;
    UINT backbuffer_index = 0;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&chain3))) {
        backbuffer_index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    } else {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f) dt = 0.0167f;

    float target = open_ ? 1.f : 0.f;
    panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
    panel_anim_  = clamp01(panel_anim_);

    if (panel_anim_ > 0.001f) render_panel();
    render_notifications(dt);
    render_hud();

    ImGui::Render();

    auto* dev = (ID3D12Device*)dx12_device_;
    auto* queue = (ID3D12CommandQueue*)dx12_command_queue_;
    auto* cmd_list = (ID3D12GraphicsCommandList*)dx12_command_list_;
    auto* fence = (ID3D12Fence*)dx12_fence_;
    auto fence_event = (HANDLE)dx12_fence_event_;
    if (backbuffer_index >= dx12_command_allocators_.size() ||
        backbuffer_index >= dx12_resources_.size() ||
        backbuffer_index >= dx12_frame_fence_.size() ||
        !fence || !fence_event) {
        return;
    }
    auto* allocator = (ID3D12CommandAllocator*)dx12_command_allocators_[backbuffer_index];
    auto* resource = (ID3D12Resource*)dx12_resources_[backbuffer_index];
    auto* rtv_heap = (ID3D12DescriptorHeap*)dx12_rtv_heap_;
    auto* srv_heap = (ID3D12DescriptorHeap*)dx12_srv_heap_;

    // Wait until the GPU finished the previous frame recorded with this
    // buffer's allocator. Timeout+skip (instead of hang) on device loss.
    uint64_t wait_value = dx12_frame_fence_[backbuffer_index];
    if (wait_value != 0 && fence->GetCompletedValue() < wait_value) {
        fence->SetEventOnCompletion(wait_value, fence_event);
        if (WaitForSingleObject(fence_event, 1000) != WAIT_OBJECT_0) {
            static bool logged_timeout = false;
            if (!logged_timeout) {
                logged_timeout = true;
                STAR_LOG("DX12 frame fence timeout, skipping frames until GPU recovers");
            }
            if (++dx12_timeout_streak_ >= 30) {
                dx12_timeout_streak_ = 0;
                switch_to_external("DX12 frame fence never completes", true);
                return;
            }
            return;
        }
    }

    // NOTE: on engines with unexpected backbuffer state (e.g. ACEVO), ANY
    // transition of their buffer faults the GPU, so there is a dx12_render
    // escape hatch in overlay.star. When enabled we assume stock flip-model
    // PRESENT state here, like the stock ImGui DX12 example does.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    {
        // First-frames diagnostics: pinpoints which D3D call dies, if any.
        static int dx12_dbg = 0;
        bool dbg = dx12_dbg < 5;
        HRESULT r1 = allocator->Reset();
        if (FAILED(r1)) {
            STAR_LOG("DX12 allocator Reset failed hr=0x%08x removed=0x%08x",
                (unsigned)r1, (unsigned)dev->GetDeviceRemovedReason());
            return;
        }
        HRESULT r2 = cmd_list->Reset(allocator, nullptr);
        if (FAILED(r2)) {
            STAR_LOG("DX12 list Reset failed hr=0x%08x removed=0x%08x",
                (unsigned)r2, (unsigned)dev->GetDeviceRemovedReason());
            return;
        }
        if (dbg) STAR_LOG("DX12 frame %d: reset ok (bi=%u)", dx12_dbg, backbuffer_index);
    }
    cmd_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    UINT rtv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_handle.ptr += backbuffer_index * rtv_descriptor_size;
    cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    cmd_list->SetDescriptorHeaps(1, &srv_heap);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_list->ResourceBarrier(1, &barrier);

    HRESULT close_hr = cmd_list->Close();
    if (FAILED(close_hr)) {
        STAR_LOG("DX12 list Close failed hr=0x%08x removed=0x%08x",
            (unsigned)close_hr, (unsigned)dev->GetDeviceRemovedReason());
        return;
    }

    ID3D12CommandList* lists[] = { cmd_list };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++dx12_fence_value_);
    dx12_frame_fence_[backbuffer_index] = dx12_fence_value_;
    dx12_timeout_streak_ = 0;
    {
        static bool logged_first = false;
        if (!logged_first) {
            logged_first = true;
            HRESULT removed = dev->GetDeviceRemovedReason();
            STAR_LOG("DX12 first frame submitted (removed=0x%08x)", (unsigned)removed);
            if (FAILED(removed))
                switch_to_external("DX12 device removed after first submit", true);
        }
    }
}

void StarOverlay::cleanup_dx12()
{
    for (auto* res : dx12_icon_resources_) if (res) ((ID3D12Resource*)res)->Release();
    dx12_icon_resources_.clear();
    dx12_srv_next_slot_ = 1;
    dx12_frame_fence_.clear();
    dx12_fence_value_ = 0;
    if (dx12_fence_event_) { CloseHandle((HANDLE)dx12_fence_event_); dx12_fence_event_ = nullptr; }
    if (dx12_fence_) { ((ID3D12Fence*)dx12_fence_)->Release(); dx12_fence_ = nullptr; }

    for (auto* res : dx12_resources_) if (res) ((ID3D12Resource*)res)->Release();
    dx12_resources_.clear();
    for (auto* alloc : dx12_command_allocators_) if (alloc) ((ID3D12CommandAllocator*)alloc)->Release();
    dx12_command_allocators_.clear();
    if (dx12_command_list_) { ((ID3D12GraphicsCommandList*)dx12_command_list_)->Release(); dx12_command_list_ = nullptr; }
    if (dx12_rtv_heap_) { ((ID3D12DescriptorHeap*)dx12_rtv_heap_)->Release(); dx12_rtv_heap_ = nullptr; }
    if (dx12_srv_heap_) { ((ID3D12DescriptorHeap*)dx12_srv_heap_)->Release(); dx12_srv_heap_ = nullptr; }
    if (dx12_command_queue_) { ((ID3D12CommandQueue*)dx12_command_queue_)->Release(); dx12_command_queue_ = nullptr; }
    if (dx12_device_) { ((ID3D12Device*)dx12_device_)->Release(); dx12_device_ = nullptr; }
}
#endif

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX9Present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* rgn)
{
    if (g_overlay) g_overlay->on_present_dx9(device);
    return g_overlay ? g_overlay->orig_dx9_present_(device, src, dst, window, rgn) : S_OK;
}

HRESULT STDMETHODCALLTYPE StarOverlay::hooked_DX9Reset(IDirect3DDevice9* device, void* params)
{
    if (g_overlay) g_overlay->on_reset_dx9();
    typedef HRESULT(STDMETHODCALLTYPE* DX9ResetFn)(IDirect3DDevice9*, void*);
    auto orig = (DX9ResetFn)g_overlay->orig_dx9_reset_;
    HRESULT hr = orig(device, params);
    if (SUCCEEDED(hr) && g_overlay && g_overlay->active_api_ == GraphicsAPI::DX9) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return hr;
}

void StarOverlay::hook_dx9()
{
    if (orig_dx9_present_ && orig_dx9_reset_) { dx9_hooked_ = true; return; }
    HMODULE d3d9_dll = GetModuleHandleA("d3d9.dll");
    if (!d3d9_dll) d3d9_dll = LoadLibraryA("d3d9.dll");
    if (!d3d9_dll) { STAR_LOG("DX9 hook: d3d9.dll not available"); return; }

    typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
    auto pDirect3DCreate9 = (Direct3DCreate9Fn)GetProcAddress(d3d9_dll, "Direct3DCreate9");
    if (!pDirect3DCreate9) { STAR_LOG("DX9 hook: Direct3DCreate9 export missing"); return; }

    IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { STAR_LOG("DX9 hook: Direct3DCreate9 failed"); return; }

    HWND dummy = CreateWindowExA(0, "STAR_Dummy", "", WS_OVERLAPPEDWINDOW, 0, 0, 4, 4, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dummy) { STAR_LOG("DX9 hook: dummy window failed"); d3d->Release(); return; }

    D3DPRESENT_PARAMETERS d3dpp{};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = dummy;

    // We only need the vtable, so try several device flavors (HAL/SW first,
    // NULLREF fallback). XNA/D3D9Ex games share the same vtable layout.
    struct Dx9Attempt { int devtype; DWORD behavior; const char* name; };
    const Dx9Attempt attempts[] = {
        { D3DDEVTYPE_HAL,    D3DCREATE_SOFTWARE_VERTEXPROCESSING, "HAL/SW" },
        { D3DDEVTYPE_HAL,    D3DCREATE_HARDWARE_VERTEXPROCESSING, "HAL/HW" },
        { D3DDEVTYPE_HAL,    D3DCREATE_MIXED_VERTEXPROCESSING,    "HAL/MIXED" },
        { D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "NULLREF/SW" },
        { D3DDEVTYPE_REF,    D3DCREATE_SOFTWARE_VERTEXPROCESSING, "REF/SW" },
    };
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = E_FAIL;
    for (auto& a : attempts) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, (D3DDEVTYPE)a.devtype, dummy, a.behavior, &d3dpp, &device);
        if (SUCCEEDED(hr) && device) break;
        STAR_LOG("DX9 hook: CreateDevice %s failed hr=0x%08x", a.name, (unsigned)hr);
    }
    if (FAILED(hr) || !device) {
        STAR_LOG("DX9 hook: all CreateDevice attempts failed, last hr=0x%08x", (unsigned)hr);
        DestroyWindow(dummy);
        d3d->Release();
        return;
    }

    void** vt = *(void***)device;
    if (!orig_dx9_present_) {
        MH_STATUS s1 = MH_CreateHook(vt[17], &hooked_DX9Present, (void**)&orig_dx9_present_);
        if (s1 == MH_OK) MH_EnableHook(vt[17]); else STAR_LOG("DX9 hook: Present MH=%d", (int)s1);
    }
    if (!orig_dx9_reset_) {
        MH_STATUS s2 = MH_CreateHook(vt[16], &hooked_DX9Reset,   (void**)&orig_dx9_reset_);
        if (s2 == MH_OK) MH_EnableHook(vt[16]); else STAR_LOG("DX9 hook: Reset MH=%d", (int)s2);
    }

    device->Release();
    DestroyWindow(dummy);
    d3d->Release();
    if (orig_dx9_present_) { dx9_hooked_ = true; STAR_LOG("DX9 hooked"); }
}

void StarOverlay::on_present_dx9(IDirect3DDevice9* device)
{
    if (!enabled_ || !device) return;
    poll_hotkey();
    note_present();
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::DX9;
        STAR_LOG("Game graphics API: DirectX 9");
    }
    if (mode_ == OverlayMode::External) return;
    dx9_device_ = device;
    {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (SUCCEEDED(device->GetCreationParameters(&cp))) hook_window_for(cp.hFocusWindow);
    }

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    if (!imgui_initialized_) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        device->GetCreationParameters(&cp);
        HWND new_hwnd = cp.hFocusWindow;
        if (!new_hwnd) new_hwnd = GetActiveWindow();
        hook_window_for(new_hwnd);

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(hwnd_);
        // hook_window already done via hook_window_for

        if (ImGui_ImplDX9_Init(device)) {
            setup_imgui_style_and_fonts();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::DX9;
            STAR_LOG("ImGui ready (DX9) hwnd=%p", hwnd_);
        } else {
            STAR_LOG("ImGui DX9 init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::DX9) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.f) dt = 0.0167f;

        float target = open_ ? 1.f : 0.f;
        panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
        panel_anim_  = clamp01(panel_anim_);

        if (panel_anim_ > 0.001f) render_panel();
        render_notifications(dt);
        render_hud();
    render_hud();

        ImGui::Render();

        IDirect3DStateBlock9* state_block = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state_block))) {
            state_block->Capture();
        }

        HRESULT scene_hr = device->BeginScene();
        if (SUCCEEDED(scene_hr)) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }

        if (state_block) {
            state_block->Apply();
            state_block->Release();
        }

        maybe_capture_dx9(device);
    }
}

void StarOverlay::on_reset_dx9()
{
    if (mode_ == OverlayMode::External) return;
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (active_api_ == GraphicsAPI::DX9 && imgui_initialized_) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

BOOL WINAPI StarOverlay::hooked_wglSwapBuffers(HDC hdc)
{
    if (g_overlay) g_overlay->on_present_opengl(hdc);
    return g_overlay ? g_overlay->orig_wglSwapBuffers_(hdc) : FALSE;
}

void StarOverlay::hook_opengl()
{
    if (orig_wglSwapBuffers_) { opengl_hooked_ = true; return; }
    HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
    if (!opengl_dll) opengl_dll = LoadLibraryA("opengl32.dll");
    if (!opengl_dll) return;

    void* pSwapBuffers = (void*)GetProcAddress(opengl_dll, "wglSwapBuffers");
    if (pSwapBuffers) {
        MH_STATUS s = MH_CreateHook(pSwapBuffers, &hooked_wglSwapBuffers, (void**)&orig_wglSwapBuffers_);
        if (s == MH_OK) {
            MH_EnableHook(pSwapBuffers);
            opengl_hooked_ = true;
            STAR_LOG("OpenGL hooked");
        } else {
            STAR_LOG("OpenGL hook: MH=%d", (int)s);
        }
    }
}

void StarOverlay::on_present_opengl(HDC hdc)
{
    if (!enabled_) return;
    poll_hotkey();
    note_present();
    if (game_api_ == GraphicsAPI::None) {
        game_api_ = GraphicsAPI::OpenGL;
        STAR_LOG("Game graphics API: OpenGL");
    }
    if (mode_ == OverlayMode::External) return;
    hook_window_for(WindowFromDC(hdc));

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    if (!imgui_initialized_) {
        HWND new_hwnd = WindowFromDC(hdc);
        if (!new_hwnd) new_hwnd = GetActiveWindow();
        hook_window_for(new_hwnd);

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(hwnd_);

        if (ImGui_ImplOpenGL3_Init()) {
            setup_imgui_style_and_fonts();
            imgui_initialized_ = true;
            active_api_ = GraphicsAPI::OpenGL;
            STAR_LOG("ImGui ready (OpenGL) hwnd=%p", hwnd_);
        } else {
            STAR_LOG("ImGui OpenGL init FAILED");
        }
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::OpenGL) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        apply_cursor_mode();

        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.f) dt = 0.0167f;

        float target = open_ ? 1.f : 0.f;
        panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
        panel_anim_  = clamp01(panel_anim_);

        if (panel_anim_ > 0.001f) render_panel();
        render_notifications(dt);
        render_hud();
    render_hud();

        ImGui::Render();

        #ifndef GL_ALL_ATTRIB_BITS
        #define GL_ALL_ATTRIB_BITS 0x000fffff
        #define GL_CLIENT_ALL_ATTRIB_BITS 0xffffffff
        #endif
        typedef void(WINAPI* glPushAttribFn)(uint32_t);
        typedef void(WINAPI* glPopAttribFn)();
        typedef void(WINAPI* glPushClientAttribFn)(uint32_t);
        typedef void(WINAPI* glPopClientAttribFn)();

        HMODULE opengl_dll = GetModuleHandleA("opengl32.dll");
        auto glPushAttrib = (glPushAttribFn)GetProcAddress(opengl_dll, "glPushAttrib");
        auto glPopAttrib = (glPopAttribFn)GetProcAddress(opengl_dll, "glPopAttrib");
        auto glPushClientAttrib = (glPushClientAttribFn)GetProcAddress(opengl_dll, "glPushClientAttrib");
        auto glPopClientAttrib = (glPopClientAttribFn)GetProcAddress(opengl_dll, "glPopClientAttrib");

        if (glPushAttrib) glPushAttrib(GL_ALL_ATTRIB_BITS);
        if (glPushClientAttrib) glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (glPopClientAttrib) glPopClientAttrib();
        if (glPopAttrib) glPopAttrib();

        maybe_capture_opengl();
    }
}

#define VULKAN_FUNCS \
    VK_FUNC(vkGetInstanceProcAddr) \
    VK_FUNC(vkGetDeviceProcAddr) \
    VK_FUNC(vkCreateInstance) \
    VK_FUNC(vkDestroyInstance) \
    VK_FUNC(vkCreateDevice) \
    VK_FUNC(vkDestroyDevice) \
    VK_FUNC(vkEnumeratePhysicalDevices) \
    VK_FUNC(vkGetPhysicalDeviceProperties) \
    VK_FUNC(vkGetPhysicalDeviceQueueFamilyProperties) \
    VK_FUNC(vkGetPhysicalDeviceMemoryProperties) \
    VK_FUNC(vkGetDeviceQueue) \
    VK_FUNC(vkCreateSwapchainKHR) \
    VK_FUNC(vkDestroySwapchainKHR) \
    VK_FUNC(vkGetSwapchainImagesKHR) \
    VK_FUNC(vkCreateRenderPass) \
    VK_FUNC(vkDestroyRenderPass) \
    VK_FUNC(vkCreateDescriptorPool) \
    VK_FUNC(vkDestroyDescriptorPool) \
    VK_FUNC(vkCreateCommandPool) \
    VK_FUNC(vkDestroyCommandPool) \
    VK_FUNC(vkAllocateCommandBuffers) \
    VK_FUNC(vkFreeCommandBuffers) \
    VK_FUNC(vkBeginCommandBuffer) \
    VK_FUNC(vkEndCommandBuffer) \
    VK_FUNC(vkCmdBeginRenderPass) \
    VK_FUNC(vkCmdEndRenderPass) \
    VK_FUNC(vkCmdPipelineBarrier) \
    VK_FUNC(vkCmdCopyBufferToImage) \
    VK_FUNC(vkCmdCopyImageToBuffer) \
    VK_FUNC(vkCreateFramebuffer) \
    VK_FUNC(vkDestroyFramebuffer) \
    VK_FUNC(vkCreateImage) \
    VK_FUNC(vkDestroyImage) \
    VK_FUNC(vkCreateImageView) \
    VK_FUNC(vkDestroyImageView) \
    VK_FUNC(vkCreateBuffer) \
    VK_FUNC(vkDestroyBuffer) \
    VK_FUNC(vkAllocateMemory) \
    VK_FUNC(vkFreeMemory) \
    VK_FUNC(vkBindBufferMemory) \
    VK_FUNC(vkBindImageMemory) \
    VK_FUNC(vkMapMemory) \
    VK_FUNC(vkUnmapMemory) \
    VK_FUNC(vkGetBufferMemoryRequirements) \
    VK_FUNC(vkGetImageMemoryRequirements) \
    VK_FUNC(vkCreateSampler) \
    VK_FUNC(vkDestroySampler) \
    VK_FUNC(vkCreateFence) \
    VK_FUNC(vkWaitForFences) \
    VK_FUNC(vkDestroyFence) \
    VK_FUNC(vkQueuePresentKHR) \
    VK_FUNC(vkDeviceWaitIdle) \
    VK_FUNC(vkResetCommandBuffer) \
    VK_FUNC(vkQueueSubmit)

#define VK_FUNC(name) static PFN_##name name = nullptr;
VULKAN_FUNCS
#undef VK_FUNC

static bool resolve_vulkan_funcs(HMODULE vulkan) {
    #define VK_FUNC(name) \
        name = (PFN_##name)GetProcAddress(vulkan, #name); \
        if (!name) return false;
    VULKAN_FUNCS
    #undef VK_FUNC
    return true;
}

static VkInstance g_vk_instance = VK_NULL_HANDLE;

static PFN_vkVoidFunction ImGuiVulkanLoader(const char* function_name, void* user_data) {
    HMODULE vulkan = (HMODULE)user_data;
    if (vkGetInstanceProcAddr && g_vk_instance) {
        auto addr = vkGetInstanceProcAddr(g_vk_instance, function_name);
        if (addr) return addr;
    }
    return (PFN_vkVoidFunction)GetProcAddress(vulkan, function_name);
}

static uint32_t vk_find_memory_type(VkPhysicalDevice pdev, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(pdev, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

struct VulkanOverlayData {
    HMODULE vulkan_dll = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> image_views;
    std::vector<VkImage> images;
    uint32_t image_count = 0;
    VkSampler icon_sampler = VK_NULL_HANDLE;
    std::vector<VkImage>        icon_images;
    std::vector<VkDeviceMemory> icon_memories;
    std::vector<VkImageView>    icon_views;

    void cleanup() {
        if (device) {
            if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
            for (auto v : icon_views)    if (v && vkDestroyImageView) vkDestroyImageView(device, v, nullptr);
            icon_views.clear();
            for (auto m : icon_memories) if (m && vkFreeMemory)       vkFreeMemory(device, m, nullptr);
            icon_memories.clear();
            for (auto i : icon_images)   if (i && vkDestroyImage)     vkDestroyImage(device, i, nullptr);
            icon_images.clear();
            if (icon_sampler && vkDestroySampler) vkDestroySampler(device, icon_sampler, nullptr);
            icon_sampler = VK_NULL_HANDLE;
            for (auto fb : framebuffers) if (fb && vkDestroyFramebuffer) vkDestroyFramebuffer(device, fb, nullptr);
            framebuffers.clear();
            for (auto iv : image_views) if (iv && vkDestroyImageView) vkDestroyImageView(device, iv, nullptr);
            image_views.clear();
            if (vkFreeCommandBuffers && command_pool) {
                vkFreeCommandBuffers(device, command_pool, (uint32_t)command_buffers.size(), command_buffers.data());
            }
            command_buffers.clear();
            if (vkDestroyCommandPool && command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
            if (vkDestroyDescriptorPool && descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
            if (vkDestroyRenderPass && render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
            render_pass = VK_NULL_HANDLE;
        }
        device = VK_NULL_HANDLE;
        physical_device = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
    }
};

int StarOverlay::hooked_vkCreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateInstance)(const void*, const void*, void**);
    auto orig = (PFN_vkCreateInstance)g_overlay->orig_vkCreateInstance_;
    VkResult res = orig(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && g_overlay) {
        g_overlay->vk_instance_ = *pInstance;
        g_vk_instance = (VkInstance)*pInstance;
    }
    return res;
}

int StarOverlay::hooked_vkCreateDevice(void* physicalDevice, const void* pCreateInfo, const void* pAllocator, void** pDevice)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateDevice)(void*, const void*, const void*, void**);
    auto orig = (PFN_vkCreateDevice)g_overlay->orig_vkCreateDevice_;
    VkResult res = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS && pDevice && g_overlay) {
        g_overlay->vk_physical_device_ = physicalDevice;
        g_overlay->vk_device_ = *pDevice;
        g_overlay->vk_queue_family_ = 0;

        struct FakeQueueCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            uint32_t queueFamilyIndex;
        };
        struct FakeCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            uint32_t queueCreateInfoCount;
            const FakeQueueCreateInfo* pQueueCreateInfos;
        };
        auto* ci = (const FakeCreateInfo*)pCreateInfo;
        if (ci && ci->queueCreateInfoCount > 0 && ci->pQueueCreateInfos) {
            g_overlay->vk_queue_family_ = ci->pQueueCreateInfos[0].queueFamilyIndex;
        }

        auto gdpa = (PFN_vkGetDeviceProcAddr)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetDeviceProcAddr");
        if (gdpa) {
            void* pCreateSwapchain = (void*)gdpa((VkDevice)*pDevice, "vkCreateSwapchainKHR");
            if (pCreateSwapchain && !g_overlay->orig_vkCreateSwapchainKHR_) {
                MH_CreateHook(pCreateSwapchain, &hooked_vkCreateSwapchainKHR, (void**)&g_overlay->orig_vkCreateSwapchainKHR_);
                MH_EnableHook(pCreateSwapchain);
            }
        }
    }
    return res;
}

int StarOverlay::hooked_vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pSwapchain)
{
    typedef VkResult(VKAPI_PTR* PFN_vkCreateSwapchainKHR)(void*, const void*, const void*, uint64_t*);
    auto orig = (PFN_vkCreateSwapchainKHR)g_overlay->orig_vkCreateSwapchainKHR_;
    VkResult res = orig ? orig(device, pCreateInfo, pAllocator, pSwapchain)
                        : ((PFN_vkCreateSwapchainKHR)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkCreateSwapchainKHR"))(device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS && pSwapchain && g_overlay) {
        // Recovery: if vkCreateDevice was missed (game init before SteamAPI_Init),
        // this still gives us the VkDevice for on_present_vulkan.
        if (device) g_overlay->vk_device_ = device;
        struct FakeSwapchainCreateInfo {
            int sType;
            const void* pNext;
            int flags;
            void* surface;
            uint32_t minImageCount;
            int imageFormat;
        };
        auto* ci = (const FakeSwapchainCreateInfo*)pCreateInfo;
        if (ci) {
            g_overlay->vk_swapchain_format_ = ci->imageFormat;
            g_overlay->vk_min_image_count_ = ci->minImageCount;
        }
        g_overlay->vk_swapchain_recreated_ = true;
        STAR_LOG("Vulkan swapchain created fmt=%d count=%u device=%p", g_overlay->vk_swapchain_format_, (unsigned)g_overlay->vk_min_image_count_, device);
    }
    return res;
}

int StarOverlay::hooked_vkQueuePresentKHR(void* queue, const void* pPresentInfo)
{
    if (g_overlay) {
        g_overlay->poll_hotkey();
        g_overlay->note_present();
        g_overlay->on_present_vulkan(queue, pPresentInfo);
    }
    typedef VkResult(VKAPI_PTR* PFN_vkQueuePresentKHR)(void*, const void*);
    auto orig = (PFN_vkQueuePresentKHR)g_overlay->orig_vkQueuePresentKHR_;
    return orig(queue, pPresentInfo);
}

int StarOverlay::hooked_vkAcquireNextImageKHR(void* device, uint64_t swapchain, uint64_t timeout, void* semaphore, void* fence, uint32_t* pImageIndex)
{
    if (g_overlay) {
        // Called every frame with the game's device + swapchain. This is the
        // recovery path for games that init Vulkan before SteamAPI_Init: the
        // create hooks never fire, but this still gives us the VkDevice.
        if (!g_overlay->vk_device_) g_overlay->vk_device_ = device;
        g_overlay->vk_swapchain_ = (void*)swapchain;
    }
    typedef VkResult(VKAPI_PTR* PFN_vkAcquireNextImageKHR)(void*, uint64_t, uint64_t, void*, void*, uint32_t*);
    auto orig = (PFN_vkAcquireNextImageKHR)g_overlay->orig_vkAcquireNextImageKHR_;
    return orig(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

void StarOverlay::hook_vulkan()
{
    if (orig_vkQueuePresentKHR_ && orig_vkCreateDevice_ && orig_vkCreateInstance_) { vulkan_hooked_ = true; return; }
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) return;

    void* pCreateInstance = (void*)GetProcAddress(vulkan, "vkCreateInstance");
    void* pCreateDevice = (void*)GetProcAddress(vulkan, "vkCreateDevice");
    void* pQueuePresent = (void*)GetProcAddress(vulkan, "vkQueuePresentKHR");
    void* pCreateSwapchain = (void*)GetProcAddress(vulkan, "vkCreateSwapchainKHR");
    void* pAcquireNextImage = (void*)GetProcAddress(vulkan, "vkAcquireNextImageKHR");

    if (pCreateInstance && !orig_vkCreateInstance_) {
        if (MH_CreateHook(pCreateInstance, &hooked_vkCreateInstance, (void**)&orig_vkCreateInstance_) == MH_OK)
            MH_EnableHook(pCreateInstance);
    }
    if (pCreateDevice && !orig_vkCreateDevice_) {
        if (MH_CreateHook(pCreateDevice, &hooked_vkCreateDevice, (void**)&orig_vkCreateDevice_) == MH_OK)
            MH_EnableHook(pCreateDevice);
    }
    if (pQueuePresent && !orig_vkQueuePresentKHR_) {
        if (MH_CreateHook(pQueuePresent, &hooked_vkQueuePresentKHR, (void**)&orig_vkQueuePresentKHR_) == MH_OK) {
            MH_EnableHook(pQueuePresent);
            STAR_LOG("Vulkan QueuePresent hooked");
        }
    }
    // Loader export: catches swapchain recreates even when vkCreateDevice was missed.
    if (pCreateSwapchain && !orig_vkCreateSwapchainKHR_) {
        if (MH_CreateHook(pCreateSwapchain, &hooked_vkCreateSwapchainKHR, (void**)&orig_vkCreateSwapchainKHR_) == MH_OK) {
            MH_EnableHook(pCreateSwapchain);
            STAR_LOG("Vulkan CreateSwapchain hooked (loader)");
        }
    }
    // Loader export: fires every frame with the game's device + swapchain.
    // Recovery for games that init Vulkan before SteamAPI_Init (the create
    // hooks are missed); on_present_vulkan uses the captured device to
    // rebuild the instance/physical device at present time.
    if (pAcquireNextImage && !orig_vkAcquireNextImageKHR_) {
        if (MH_CreateHook(pAcquireNextImage, &hooked_vkAcquireNextImageKHR, (void**)&orig_vkAcquireNextImageKHR_) == MH_OK) {
            MH_EnableHook(pAcquireNextImage);
            STAR_LOG("Vulkan AcquireNextImage hooked (loader)");
        }
    }
    if (orig_vkQueuePresentKHR_) { vulkan_hooked_ = true; STAR_LOG("Vulkan hooked"); }
}

void StarOverlay::on_present_vulkan(void* queue, const void* pPresentInfo)
{
    if (!enabled_) return;
    // Claim the game unconditionally: a Vulkan present means the game renders
    // via Vulkan, even if a DXGI present fired first and mis-detected DX12.
    // on_present() gates the DX12 path on this so it never fights the Vulkan
    // present on the same backbuffer (GPU hang / game freeze).
    if (game_api_ != GraphicsAPI::Vulkan) {
        game_api_ = GraphicsAPI::Vulkan;
        STAR_LOG("Game graphics API: Vulkan");
        // In-backbuffer Vulkan drawing submits without the game's present
        // semaphores (same-queue ordering covers that) and now transitions
        // the image explicitly instead of lying about initialLayout, so the
        // old GPU-hang vector is gone. If a title still faults, the auto
        // fallback (switch_to_external) takes over.
    }
    if (mode_ == OverlayMode::External) return;
    if (!vk_device_ || !vk_instance_) {
        // Late-init recovery: the game created its Vulkan instance/device
        // before SteamAPI_Init, so the create hooks were missed. The
        // vkCreateSwapchainKHR / vkAcquireNextImageKHR hooks captured the
        // device; rebuild the instance + physical device here so the overlay
        // can init.
        if (!vk_instance_ && vk_device_) {
            recover_vulkan_late();
            // The recovery's format/count are unknown until the swapchain
            // hook fires; init on a later present so the real values are used.
            vk_swapchain_recreated_ = true;
            return;
        }
        if (!vk_device_ || !vk_instance_) {
            static bool logged = false;
            if (!logged) { logged = true; STAR_LOG("Vulkan present: waiting for device/instance (device=%p instance=%p)", vk_device_, vk_instance_); }
            return;
        }
    }

    std::unique_lock<std::mutex> lock(render_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    if (!pi || pi->swapchainCount == 0 || !pi->pSwapchains || !pi->pImageIndices) return;

    uint32_t image_index = pi->pImageIndices[0];
    vk_queue_ = queue;

    if ((vk_swapchain_recreated_ || !imgui_initialized_) && vk_swapchain_format_ != 0) {
        cleanup_vulkan();
        init_imgui_vulkan(queue, pPresentInfo);
        vk_swapchain_recreated_ = false;
    }

    if (imgui_initialized_ && active_api_ == GraphicsAPI::Vulkan) {
        render_frame_vulkan(queue, pPresentInfo);
    }
}

void StarOverlay::recover_vulkan_late()
{
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) return;
    if (!resolve_vulkan_funcs(vulkan)) return;

    // The game's instance was created before we hooked, so we can't capture
    // it. Create a throwaway instance to enumerate the physical device and
    // satisfy ImGui's backend (which asserts Instance + PhysicalDevice).
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "STAR";
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo inst_info = {};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;

    VkInstance dummy = VK_NULL_HANDLE;
    if (vkCreateInstance(&inst_info, nullptr, &dummy) != VK_SUCCESS) return;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(dummy, &count, nullptr);
    if (count == 0) { vkDestroyInstance(dummy, nullptr); return; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(dummy, &count, devices.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (auto pd : devices) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { chosen = pd; break; }
    }
    if (!chosen && !devices.empty()) chosen = devices[0];

    vk_instance_ = dummy;
    vk_physical_device_ = chosen;
    g_vk_instance = dummy;
    STAR_LOG("Vulkan late recovery: device=%p instance=%p physical=%p fmt=%d count=%u",
        vk_device_, vk_instance_, vk_physical_device_, vk_swapchain_format_, (unsigned)vk_min_image_count_);
}

// The game's swapchain is often sRGB (e.g. R8G8B8A8_SRGB). ImGui's shader
// outputs colors as-is, so an sRGB render pass would re-encode them and wash
// the overlay out. Render through a UNORM view of the same image instead: the
// formats are view-compatible, the game's sRGB content is preserved by
// LOAD_OP_LOAD, and ImGui's colors hit the screen unmodified.
static VkFormat unorm_view_format(VkFormat fmt)
{
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8_SRGB:    return VK_FORMAT_R8G8B8_UNORM;
        case VK_FORMAT_B8G8R8_SRGB:    return VK_FORMAT_B8G8R8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return fmt;
    }
}

void StarOverlay::init_imgui_vulkan(void* queue, const void* pPresentInfo)
{
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) return;

    if (!resolve_vulkan_funcs(vulkan)) return;

    VkAttachmentDescription attachment = {};
    attachment.format = unorm_view_format((VkFormat)vk_swapchain_format_);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment = {};
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass((VkDevice)vk_device_, &rp_info, nullptr, &rp) != VK_SUCCESS) return;

    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 257 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 257;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = pool_sizes;

    VkDescriptorPool dp = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool((VkDevice)vk_device_, &pool_info, nullptr, &dp) != VK_SUCCESS) {
        vkDestroyRenderPass((VkDevice)vk_device_, rp, nullptr);
        return;
    }

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    uint64_t swapchain = pi->pSwapchains[0];

    uint32_t count = 0;
    vkGetSwapchainImagesKHR((VkDevice)vk_device_, (VkSwapchainKHR)swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR((VkDevice)vk_device_, (VkSwapchainKHR)swapchain, &count, images.data());

    std::vector<VkImageView> views(count);
    std::vector<VkFramebuffer> fbs(count);

    if (!hwnd_) hwnd_ = GetActiveWindow();
    RECT rect{};
    if (hwnd_) GetClientRect(hwnd_, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;
    if (w == 0) w = 1280;
    if (h == 0) h = 720;

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = unorm_view_format((VkFormat)vk_swapchain_format_);
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView((VkDevice)vk_device_, &view_info, nullptr, &views[i]) != VK_SUCCESS) return;

        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = rp;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &views[i];
        fb_info.width = w;
        fb_info.height = h;
        fb_info.layers = 1;

        if (vkCreateFramebuffer((VkDevice)vk_device_, &fb_info, nullptr, &fbs[i]) != VK_SUCCESS) return;
    }

    VkCommandPoolCreateInfo cp_info = {};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp_info.queueFamilyIndex = vk_queue_family_;

    VkCommandPool cp = VK_NULL_HANDLE;
    if (vkCreateCommandPool((VkDevice)vk_device_, &cp_info, nullptr, &cp) != VK_SUCCESS) return;

    std::vector<VkCommandBuffer> cbs(count);
    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = cp;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = count;
    if (vkAllocateCommandBuffers((VkDevice)vk_device_, &cb_info, cbs.data()) != VK_SUCCESS) return;

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd_);
    hook_window();

    ImGui_ImplVulkan_LoadFunctions(&ImGuiVulkanLoader, vulkan);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = (VkInstance)vk_instance_;
    init_info.PhysicalDevice = (VkPhysicalDevice)vk_physical_device_;
    init_info.Device = (VkDevice)vk_device_;
    init_info.QueueFamily = vk_queue_family_;
    init_info.Queue = (VkQueue)queue;
    init_info.DescriptorPool = dp;
    init_info.RenderPass = rp;
    init_info.MinImageCount = vk_min_image_count_;
    init_info.ImageCount = count;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (ImGui_ImplVulkan_Init(&init_info)) {
        setup_imgui_style_and_fonts();
        imgui_initialized_ = true;
        active_api_ = GraphicsAPI::Vulkan;

        auto* data = new VulkanOverlayData();
        data->vulkan_dll = vulkan;
        data->instance = (VkInstance)vk_instance_;
        data->physical_device = (VkPhysicalDevice)vk_physical_device_;
        data->device = (VkDevice)vk_device_;
        data->queue = (VkQueue)queue;
        data->queue_family = vk_queue_family_;
        data->render_pass = rp;
        data->descriptor_pool = dp;
        data->command_pool = cp;
        data->command_buffers = cbs;
        data->framebuffers = fbs;
        data->image_views = views;
        data->images = images;
        data->image_count = count;

        VkSamplerCreateInfo samp_info = {};
        samp_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samp_info.magFilter    = VK_FILTER_LINEAR;
        samp_info.minFilter    = VK_FILTER_LINEAR;
        samp_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_info.maxLod       = 1.f;
        if (vkCreateSampler((VkDevice)vk_device_, &samp_info, nullptr, &data->icon_sampler) != VK_SUCCESS)
            data->icon_sampler = VK_NULL_HANDLE;

        context_ = (ID3D11DeviceContext*)data;
    }
}

void StarOverlay::maybe_capture_vulkan(void* queue, const void* pPresentInfo)
{
    if (!screenshot_requested_.exchange(false)) return;
    if (!enabled_ || !imgui_initialized_ || active_api_ != GraphicsAPI::Vulkan) return;
    if (!vk_device_ || !vk_physical_device_) return;
    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    if (!pi || pi->swapchainCount == 0 || !pi->pSwapchains || !pi->pImageIndices) return;
    VkDevice dev = (VkDevice)vk_device_;
    VkSwapchainKHR swap = (VkSwapchainKHR)pi->pSwapchains[0];
    uint32_t idx = pi->pImageIndices[0];
    uint32_t count = 0;
    if (!vkGetSwapchainImagesKHR || vkGetSwapchainImagesKHR(dev, swap, &count, nullptr) != VK_SUCCESS || idx >= count)
        return;
    std::vector<VkImage> images(count);
    if (vkGetSwapchainImagesKHR(dev, swap, &count, images.data()) != VK_SUCCESS) return;
    VkImage image = images[idx];

    RECT rect{};
    uint32_t w = 1280, h = 720;
    if (hwnd_ && GetClientRect(hwnd_, &rect)) {
        if (rect.right - rect.left > 0) w = (uint32_t)(rect.right - rect.left);
        if (rect.bottom - rect.top > 0) h = (uint32_t)(rect.bottom - rect.top);
    }
    VkFormat fmt = (VkFormat)vk_swapchain_format_;
    bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    if (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB && !bgra) {
        STAR_LOG("Screenshot: unsupported Vulkan format %d", (int)fmt);
        return;
    }
    auto* data = (VulkanOverlayData*)context_;
    if (!data || !data->command_pool) return;

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = (VkDeviceSize)w * h * 4;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &buf_info, nullptr, &staging) != VK_SUCCESS) return;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, staging, &req);
    uint32_t mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (mtype == UINT32_MAX) { vkDestroyBuffer(dev, staging, nullptr); return; }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = mtype;
    if (vkAllocateMemory(dev, &alloc_info, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        return;
    }
    vkBindBufferMemory(dev, staging, mem, 0);

    VkCommandBufferAllocateInfo cb_alloc{};
    cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_alloc.commandPool = data->command_pool;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool shot_ok = false;
    std::string shot_path;
    if (vkAllocateCommandBuffers(dev, &cb_alloc, &cb) == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                           VkAccessFlags sa, VkAccessFlags da) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = 0; b.subresourceRange.levelCount = 1;
            b.subresourceRange.baseArrayLayer = 0; b.subresourceRange.layerCount = 1;
            b.srcAccessMask = sa; b.dstAccessMask = da;
            vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = w; copy.imageExtent.height = h; copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, 0);
        vkEndCommandBuffer(cb);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fence_info, nullptr, &fence) == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            if (vkQueueSubmit((VkQueue)queue, 1, &submit, fence) == VK_SUCCESS)
                vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ULL);
            void* mapped = nullptr;
            if (vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped) {
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                if (bgra) {
                    const uint8_t* s = (const uint8_t*)mapped;
                    uint8_t* d = rgba.data();
                    for (size_t p = 0; p < (size_t)w * h; p++) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                        s += 4; d += 4;
                    }
                } else {
                    memcpy(rgba.data(), mapped, rgba.size());
                }
                vkUnmapMemory(dev, mem);
                shot_path = next_screenshot_path();
                if (!shot_path.empty() && save_rgba_png(shot_path, rgba.data(), (int)w, (int)h))
                    shot_ok = true;
            }
            vkDestroyFence(dev, fence, nullptr);
        }
        vkFreeCommandBuffers(dev, data->command_pool, 1, &cb);
    }
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    if (shot_ok) notify_screenshot(shot_path);
}

void StarOverlay::render_frame_vulkan(void* queue, const void* pPresentInfo)
{
    auto* data = (VulkanOverlayData*)context_;
    if (!data) return;

    struct FakePresentInfo {
        int sType;
        const void* pNext;
        uint32_t waitSemaphoreCount;
        const uint64_t* pWaitSemaphores;
        uint32_t swapchainCount;
        const uint64_t* pSwapchains;
        const uint32_t* pImageIndices;
    };
    auto* pi = (const FakePresentInfo*)pPresentInfo;
    uint32_t image_index = pi->pImageIndices[0];
    if (image_index >= data->image_count) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    apply_cursor_mode();

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f) dt = 0.0167f;

    float target = open_ ? 1.f : 0.f;
    panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
    panel_anim_  = clamp01(panel_anim_);

    if (panel_anim_ > 0.001f) render_panel();
    render_notifications(dt);
    render_hud();

    ImGui::Render();

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer cb = data->command_buffers[image_index];
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &begin_info);

    // The game presents this image, so it sits in PRESENT_SRC_KHR. The render
    // pass expects COLOR_ATTACHMENT_OPTIMAL (its finalLayout transitions back
    // to PRESENT_SRC_KHR on end). Transition explicitly instead of lying about
    // initialLayout - a lying initialLayout is a GPU-hang vector on strict
    // drivers. Same-queue ordering (no semaphores) keeps this safe: our submit
    // runs after the game's present work on the same queue.
    if (image_index < data->images.size() && data->images[image_index]) {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = data->images[image_index];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    RECT rect{};
    if (hwnd_) GetClientRect(hwnd_, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;
    if (w == 0) w = 1280;
    if (h == 0) h = 720;

    VkRenderPassBeginInfo rp_begin = {};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = data->render_pass;
    rp_begin.framebuffer = data->framebuffers[image_index];
    rp_begin.renderArea.extent.width = w;
    rp_begin.renderArea.extent.height = h;

    vkCmdBeginRenderPass(cb, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    if (vkQueueSubmit) {
        vkQueueSubmit((VkQueue)queue, 1, &submit, VK_NULL_HANDLE);
    }

    maybe_capture_vulkan(queue, pPresentInfo);
}

ImTextureID StarOverlay::upload_icon_vulkan(const std::vector<uint8_t>& rgba, int w, int h)
{
    auto* data = (VulkanOverlayData*)context_;
    if (!data || !data->device || !data->icon_sampler || !data->command_pool) return nullptr;

    VkDevice dev  = data->device;
    VkQueue  que  = data->queue;
    VkCommandPool pool = data->command_pool;

    VkBufferCreateInfo buf_info = {};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size  = (VkDeviceSize)w * h * 4;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &buf_info, nullptr, &staging) != VK_SUCCESS) return nullptr;

    VkMemoryRequirements buf_req{};
    vkGetBufferMemoryRequirements(dev, staging, &buf_req);
    uint32_t buf_mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_mtype == UINT32_MAX) { vkDestroyBuffer(dev, staging, nullptr); return nullptr; }

    VkMemoryAllocateInfo buf_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    buf_alloc.allocationSize  = buf_req.size;
    buf_alloc.memoryTypeIndex = buf_mtype;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &buf_alloc, nullptr, &staging_mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }
    vkBindBufferMemory(dev, staging, staging_mem, 0);

    void* mapped = nullptr;
    vkMapMemory(dev, staging_mem, 0, buf_req.size, 0, &mapped);
    memcpy(mapped, rgba.data(), (size_t)w * h * 4);
    vkUnmapMemory(dev, staging_mem);

    VkImageCreateInfo img_info = {};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = { (uint32_t)w, (uint32_t)h, 1 };
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &img_info, nullptr, &image) != VK_SUCCESS) {
        vkFreeMemory(dev, staging_mem, nullptr); vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }

    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(dev, image, &img_req);
    uint32_t img_mtype = vk_find_memory_type((VkPhysicalDevice)vk_physical_device_, img_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mtype == UINT32_MAX) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr); vkDestroyBuffer(dev, staging, nullptr); return nullptr;
    }
    VkMemoryAllocateInfo img_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    img_alloc.allocationSize  = img_req.size;
    img_alloc.memoryTypeIndex = img_mtype;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &img_alloc, nullptr, &img_mem) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }
    vkBindImageMemory(dev, image, img_mem, 0);

    VkCommandBufferAllocateInfo cb_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cb_alloc.commandPool        = pool;
    cb_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cb_alloc, &cb) != VK_SUCCESS) {
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    auto transition = [&](VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                          VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout           = from;
        barrier.newLayout           = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask       = src_access;
        barrier.dstAccessMask       = dst_access;
        vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent      = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    if (vkQueueSubmit(que, 1, &submit, fence) != VK_SUCCESS) {
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, pool, 1, &cb);
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return nullptr;
    }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    vkFreeMemory(dev, staging_mem, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);

    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view_info.image            = image;
    view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &view_info, nullptr, &view) != VK_SUCCESS) {
        vkFreeMemory(dev, img_mem, nullptr);
        vkDestroyImage(dev, image, nullptr);
        return nullptr;
    }

    data->icon_images.push_back(image);
    data->icon_memories.push_back(img_mem);
    data->icon_views.push_back(view);

    return reinterpret_cast<ImTextureID>(ImGui_ImplVulkan_AddTexture(data->icon_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void StarOverlay::cleanup_vulkan()
{
    if (active_api_ == GraphicsAPI::Vulkan && context_) {
        auto* data = (VulkanOverlayData*)context_;
        data->cleanup();
        delete data;
        context_ = nullptr;
        // data->cleanup() destroys the descriptor pool and icon views, which
        // invalidates every cached ImTextureID. Drop the cache so icons are
        // re-uploaded after a swapchain recreation (e.g. fullscreen toggle).
        gl_icon_textures_.clear();
        icon_textures_.clear();
    }
}

int WINAPI StarOverlay::hooked_ShowCursor(BOOL bShow)
{
    if (g_overlay && g_overlay->orig_show_cursor_) {
        // External mode: the virtual cursor carries the panel, so the OS
        // cursor is left exactly as the game wants it (usually hidden).
        if (g_overlay->open_ && g_overlay->mode_ != OverlayMode::External) {
            int current = g_overlay->orig_show_cursor_(TRUE);
            g_overlay->orig_show_cursor_(FALSE);
            current--;
            // Hold the floor at 1 while open: the game may re-hide every frame.
            if (!bShow) {
                if (current <= 1) {
                    return 0;
                }
            }
        }
        return g_overlay->orig_show_cursor_(bShow);
    }
    return bShow ? 0 : -1;
}

BOOL WINAPI StarOverlay::hooked_ClipCursor(const RECT* lpRect)
{
    if (g_overlay && g_overlay->orig_clip_cursor_) {
        if (g_overlay->open_) {
            return g_overlay->orig_clip_cursor_(nullptr);
        }
        return g_overlay->orig_clip_cursor_(lpRect);
    }
    return TRUE;
}

HCURSOR WINAPI StarOverlay::hooked_SetCursor(HCURSOR hCursor)
{
    if (g_overlay && g_overlay->orig_set_cursor_) {
        if (g_overlay->open_ && g_overlay->mode_ != OverlayMode::External) {
            HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
            return g_overlay->orig_set_cursor_(arrow);
        }
        return g_overlay->orig_set_cursor_(hCursor);
    }
    return nullptr;
}

SHORT StarOverlay::real_GetAsyncKeyState(int vk)
{
    return orig_get_async_key_ ? orig_get_async_key_(vk) : ::GetAsyncKeyState(vk);
}

BOOL StarOverlay::real_GetKeyboardState(PBYTE keys)
{
    return orig_get_keyboard_state_ ? orig_get_keyboard_state_(keys) : ::GetKeyboardState(keys);
}

SHORT StarOverlay::real_GetKeyState(int vk)
{
    return orig_get_key_ ? orig_get_key_(vk) : ::GetKeyState(vk);
}

BOOL StarOverlay::caller_in_self_module()
{
    static HMODULE self_base = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((void*)&hooked_GetCursorPos, &mbi, sizeof(mbi)))
            self_base = (HMODULE)mbi.AllocationBase;
    }
    void* caller = _ReturnAddress();
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(caller, &mbi, sizeof(mbi))) return false;
    return self_base && mbi.AllocationBase == self_base;
}

SHORT WINAPI StarOverlay::hooked_GetAsyncKeyState(int vkey)
{
    auto* o = g_overlay;
    if (o && o->open_) return 0; // game sees nothing while panel is open
    return (o && o->orig_get_async_key_) ? o->orig_get_async_key_(vkey) : ::GetAsyncKeyState(vkey);
}

BOOL WINAPI StarOverlay::hooked_GetKeyboardState(PBYTE keys)
{
    auto* o = g_overlay;
    if (o && o->open_) {
        if (keys) memset(keys, 0, 256);
        return TRUE;
    }
    return (o && o->orig_get_keyboard_state_) ? o->orig_get_keyboard_state_(keys) : ::GetKeyboardState(keys);
}

SHORT WINAPI StarOverlay::hooked_GetKeyState(int vkey)
{
    auto* o = g_overlay;
    if (o && o->open_) return 0;
    return (o && o->orig_get_key_) ? o->orig_get_key_(vkey) : ::GetKeyState(vkey);
}

BOOL WINAPI StarOverlay::hooked_GetCursorPos(LPPOINT pt)
{
    auto* o = g_overlay;
    if (!o || !o->orig_get_cursor_pos_) {
        return o ? (o->orig_get_cursor_pos_ ? o->orig_get_cursor_pos_(pt) : FALSE)
                 : ::GetCursorPos(pt);
    }
    // ImGui's own backend polls through here too; give our own code the truth.
    if (caller_in_self_module()) return o->orig_get_cursor_pos_(pt);
    if (o->open_) {
        if (pt) *pt = o->frozen_cursor_;
        return TRUE;
    }
    return o->orig_get_cursor_pos_(pt);
}

BOOL WINAPI StarOverlay::hooked_SetCursorPos(int x, int y)
{
    auto* o = g_overlay;
    if (o && o->open_ && !caller_in_self_module()) {
        // Game re-locking the cursor (Unity Locked mode). Swallow it; the
        // real cursor keeps following the user's mouse for the panel.
        STAR_UNREFERENCED(x); STAR_UNREFERENCED(y);
        return TRUE;
    }
    return (o && o->orig_set_cursor_pos_) ? o->orig_set_cursor_pos_(x, y) : ::SetCursorPos(x, y);
}

constexpr DWORD kMouseMoveBits = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
constexpr DWORD kMouseButtonBits = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTDOWN |
    MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP |
    MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP | MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL;

static bool strip_mouse_motion(MOUSEINPUT& mi)
{
    // Returns true if any motion was stripped. Buttons/wheel survive.
    if ((mi.dwFlags & kMouseMoveBits) == 0)
        return false;
    mi.dwFlags &= ~kMouseMoveBits;
    mi.dx = 0;
    mi.dy = 0;
    return true;
}

static bool is_injected_mouse_move(const MOUSEINPUT& mi)
{
    // Pure motion (no buttons/wheel in the same packet) is a cursor warp.
    MOUSEINPUT c = mi;
    return strip_mouse_motion(c) && (c.dwFlags & kMouseButtonBits) == 0;
}

UINT WINAPI StarOverlay::hooked_SendInput(UINT nInputs, LPINPUT pInputs, int cbSize)
{
    auto* o = g_overlay;
    if (o && o->open_ && pInputs && cbSize == sizeof(INPUT) && !caller_in_self_module()) {
        // Drop motion everywhere (pure packets vanish, mixed packets keep
        // their buttons). Physical mouse never travels this path.
        std::vector<INPUT> fwd;
        fwd.reserve(nInputs);
        for (UINT i = 0; i < nInputs; i++) {
            if (pInputs[i].type != INPUT_MOUSE) { fwd.push_back(pInputs[i]); continue; }
            MOUSEINPUT mi = pInputs[i].mi;
            if (!strip_mouse_motion(mi)) { fwd.push_back(pInputs[i]); continue; }
            if (mi.dwFlags & kMouseButtonBits) {
                INPUT c = pInputs[i];
                c.mi = mi;
                fwd.push_back(c);
            }
        }
        if (fwd.size() < nInputs) {
            if (fwd.empty()) return nInputs; // swallowed: pretend it landed
            return (o->orig_send_input_) ? o->orig_send_input_((UINT)fwd.size(), fwd.data(), cbSize)
                                         : ::SendInput((UINT)fwd.size(), fwd.data(), cbSize);
        }
    }
    return (o && o->orig_send_input_) ? o->orig_send_input_(nInputs, pInputs, cbSize)
                                      : ::SendInput(nInputs, pInputs, cbSize);
}

void WINAPI StarOverlay::hooked_mouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo)
{
    auto* o = g_overlay;
    if (o && o->open_ && !caller_in_self_module()) {
        MOUSEINPUT mi{};
        mi.dwFlags = dwFlags;
        mi.dx = (LONG)dx;
        mi.dy = (LONG)dy;
        if (is_injected_mouse_move(mi)) return; // pure warp: drop it
        if (strip_mouse_motion(mi)) {
            // Mixed packet: forward buttons/wheel, drop the motion.
            if (o->orig_mouse_event_) o->orig_mouse_event_(mi.dwFlags, 0, 0, dwData, dwExtraInfo);
            else ::mouse_event(mi.dwFlags, 0, 0, dwData, dwExtraInfo);
            return;
        }
    }
    if (o && o->orig_mouse_event_) o->orig_mouse_event_(dwFlags, dx, dy, dwData, dwExtraInfo);
    else ::mouse_event(dwFlags, dx, dy, dwData, dwExtraInfo);
}
