#include "overlay/overlay_internal.h"
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

void StarOverlay::build_frame_ui()
{
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.f) dt = 0.0167f;

    float target = open_ ? 1.f : 0.f;
    panel_anim_ += (target - panel_anim_) * clamp01(12.f * dt);
    panel_anim_  = clamp01(panel_anim_);

    if (panel_anim_ > 0.001f) render_panel();
    render_notifications(dt);
    render_hud();

    ImGui::Render();
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
                (int)show_fps, (int)show_time, style_.scale());
        }
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = (ImFont*)style_.small_font();
    if (!f) f = ImGui::GetFont();

    float pad = 10.f * style_.scale();
    float x = pad + 4.f;
    float y = pad + 4.f;
    const float txt = 13.f * style_.scale();
    const float pill_pad = 7.f * style_.scale();
    const float pill_gap = 6.f * style_.scale();

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
    std::vector<AchievementNotification> notifs = notifications_.advance_and_snapshot(dt);
    if (notifs.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO&    io = ImGui::GetIO();
    ImFont* fsmall = (ImFont*)style_.small_font();
    ImFont* ftitle = (ImFont*)style_.title_font();

    const float S = style_.scale();
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
            return style_.acc(a * m);
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

