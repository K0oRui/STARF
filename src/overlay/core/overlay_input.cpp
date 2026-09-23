#include "overlay/overlay_internal.h"

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
                    // Games commonly loop until ShowCursor(FALSE) < 0.
                    // Report hidden to the game while retaining the overlay's
                    // visible cursor; returning zero traps that loop forever.
                    return -1;
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

BOOL StarOverlay::caller_in_self_module(void* caller)
{
    static HMODULE self_base = [] {
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery((void*)&hooked_GetCursorPos, &mbi, sizeof(mbi))
            ? (HMODULE)mbi.AllocationBase : nullptr;
    }();
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
    if (!o) return ::GetCursorPos(pt);
    if (!o->orig_get_cursor_pos_) return FALSE;
    // ImGui's own backend polls through here too; give our own code the truth.
    if (caller_in_self_module(_ReturnAddress())) return o->orig_get_cursor_pos_(pt);
    if (o->open_) {
        if (pt) *pt = o->frozen_cursor_;
        return TRUE;
    }
    return o->orig_get_cursor_pos_(pt);
}

BOOL WINAPI StarOverlay::hooked_SetCursorPos(int x, int y)
{
    auto* o = g_overlay;
    if (o && o->open_ && !caller_in_self_module(_ReturnAddress())) {
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

UINT WINAPI StarOverlay::hooked_SendInput(UINT nInputs, LPINPUT pInputs, int cbSize)
{
    auto* o = g_overlay;
    if (o && o->open_ && pInputs && cbSize == sizeof(INPUT) && !caller_in_self_module(_ReturnAddress())) {
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
    if (o && o->open_ && !caller_in_self_module(_ReturnAddress())) {
        MOUSEINPUT mi{};
        mi.dwFlags = dwFlags;
        mi.dx = (LONG)dx;
        mi.dy = (LONG)dy;
        MOUSEINPUT c = mi;
        if (strip_mouse_motion(c) && (c.dwFlags & kMouseButtonBits) == 0) return; // pure warp: drop it
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
