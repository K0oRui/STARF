#include "overlay/overlay_internal.h"
#include "core/callbacks.h"
#include "imgui.h"
#include "imgui_impl_win32.h"

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
        notes_.save(); // flush unsaved notes text
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
        if (((shift & 0x8000) != 0) && ((oem3 & 0x8000) != 0))
            down = true;
    }
    const bool was_down = hotkey_prev_down_.exchange(down);
    if (down && !was_down) {
        toggle_overlay();
    }
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

    if (msg == WM_KEYDOWN && wp == VK_TAB && (g_overlay->real_GetKeyState(VK_SHIFT) & 0x8000)) {
        // Shared edge with poll_hotkey(): skip if polling already claimed it.
        if (!g_overlay->hotkey_prev_down_.exchange(true)) {
            g_overlay->toggle_overlay();
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

void StarOverlay::hook_window_for(HWND h)
{
    if (!h || !IsWindow(h)) return;
    if (h == hwnd_ && wnd_proc_orig_) return;
    // Window recreated (Unity mode switch / multi-window): unhook old, hook new.
    if (hwnd_ && wnd_proc_orig_ && hwnd_ != h) {
        if (IsWindow(hwnd_)) swap_wndproc(hwnd_, wnd_proc_orig_);
        wnd_proc_orig_ = nullptr;
    }
    hwnd_ = h;
    hook_window();
}

WNDPROC StarOverlay::swap_wndproc(HWND h, WNDPROC p)
{
    if (IsWindowUnicode(h)) return (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)p);
    return (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)p);
}

void StarOverlay::hook_window()
{
    if (hwnd_ && !wnd_proc_orig_ && IsWindow(hwnd_)) {
        WNDPROC prev = swap_wndproc(hwnd_, star_wnd_proc);
        if (prev) {
            wnd_proc_orig_ = prev;
            STAR_LOG("WndProc hooked hwnd=%p", hwnd_);
        } else {
            DWORD err = GetLastError();
            STAR_LOG("WndProc hook FAILED hwnd=%p err=%lu", hwnd_, (unsigned long)err);
        }
    }
}

