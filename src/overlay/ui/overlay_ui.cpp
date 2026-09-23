#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "core/storage.h"
#include "imgui.h"

void StarOverlay::build_frame_ui()
{
    for (auto& shot : screenshots_.take_completed()) notify_screenshot(shot.path, shot.dark);
    drain_icon_decodes();
    // Real-time clock (not ImGui's present-rate clock): the animation must
    // keep advancing even when the game stalls its presents during loading.
    // Double precision + clamped step: float time loses ms resolution after
    // ~1h uptime, so uncapped DX7/DX8 titles computed dt==0 every frame and
    // fell back to 16.7ms, expiring 5s toasts in well under a second.
    // A stall (loading, alt-tab) must not age toasts by the whole gap either.
    double now = now_seconds();
    float dt;
    if (last_frame_time_ <= 0.0) {
        dt = 0.0167f;
    } else {
        double raw = now - last_frame_time_;
        if (raw <= 0.0) raw = 0.0;
        if (raw > 0.1) raw = 0.1;
        dt = (float)raw;
    }
    last_frame_time_ = now;

    float target = open_ ? 1.f : 0.f;
    if (target != panel_target_) {
        panel_target_ = target;
        panel_anim_t0_ = now;
    }
    const float PANEL_ANIM_DUR = 0.35f;
    float t = clamp01((float)((now - panel_anim_t0_) / PANEL_ANIM_DUR));
    panel_anim_ = open_ ? easeOut(t) : 1.f - easeIn(t);

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

    STAR_LOG_ONCE("HUD live (fps=%d playtime=%d scale=%.2f)",
        (int)show_fps, (int)show_time, style_.scale());

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = (ImFont*)style_.small_font();
    if (!f) f = ImGui::GetFont();

    const float S = style_.scale();
    float pad = 10.f * S;
    float x = pad + 4.f * S;
    float y = pad + 4.f * S;
    const float txt = 13.f * S;
    const float pill_pad = 7.f * S;
    const float pill_gap = 6.f * S;

    auto pill = [&](const std::string& text) {
        ImVec2 sz = f->CalcTextSizeA(txt, FLT_MAX, 0.f, text.c_str());
        ImVec2 p0 = { x, y };
        ImVec2 p1 = { x + sz.x + pill_pad * 2.f, y + sz.y + pill_pad * 2.f };
        dl->AddRectFilled(p0, p1, col(P_BG0, 0.75f), R_ICON * S);
        dl->AddText(f, txt, { p0.x + pill_pad, p0.y + pill_pad }, col(P_TXT, 0.95f), text.c_str());
        x = p1.x + pill_gap;
    };

    if (show_fps) {
        static float fps = 0;
        static DWORD sampled = 0;
        DWORD now = GetTickCount();
        if (!sampled || now - sampled >= 250) {
            fps = present_fps_ > 0.5f ? present_fps_ : ImGui::GetIO().Framerate;
            sampled = now;
        }
        if (fps < 0.f) fps = 0.f;
        pill(std::to_string((int)(fps + 0.5f)) + " FPS");
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
    std::vector<AchievementNotification> notifs = notifications_.advance_and_snapshot(dt);
    if (notifs.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO&    io = ImGui::GetIO();
    ImFont* fsmall = (ImFont*)style_.small_font();
    ImFont* ftitle = (ImFont*)style_.title_font();

    const float S = style_.scale();
    const float W   = 340.f * S;
    const float BASE_H = 82.f * S;
    const float PAD = 14.f * S;
    const float GAP =  8.f * S;
    const float RC = R_CARD * S;
    const float RI = R_ICON * S;
    const float TL = T_LINE * S;
    const float BAR_H = 2.f * S;
    const float EDGE_W = 3.f * S;
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

        float tw = W - 90.f * S;
        std::string d1, d2;
        int desc_lines = n.description.empty() ? 0
            : wrap_two_lines(fsmall, 14.f * S, n.description.c_str(), tw, d1, d2);
        float H = BASE_H + (desc_lines == 2 ? 17.f * S : 0.f);
        if (!ntop) y -= H + GAP;

        float slide = easeOut(clamp01(n.age / SLIDE_DUR));
        float fade  = (n.time_remaining < FADE_DUR) ? (n.time_remaining / FADE_DUR) : 1.f;
        float a     = clamp01(slide * fade);

        // Summary toasts go gold instead of accent.
        auto tacc = [&](float m) -> ImU32 {
            if (n.summary) return col(P_GLD, a * m);
            return style_.acc(a * m);
        };

        float x = nleft ? edge_x - (1.f - slide) * (W + PAD)
                        : edge_x + (1.f - slide) * (W + PAD);

        dl->AddRectFilled({x, y}, {x+W, y+H}, col(P_BG0, a * 0.95f), RC);

        // Edge-to-edge but confined: ends follow the card's own corner arcs.
        dl->AddRectFilled({x, y + RC}, {x + EDGE_W, y + H - RC}, tacc(1.f), 0.f);
        float dang = asinf((RC - EDGE_W) / RC);
        dl->PathClear();
        dl->PathArcTo({x + RC, y + RC}, RC, kPi, 1.5f * kPi - dang);
        dl->PathLineTo({x + EDGE_W, y + RC});
        dl->PathFillConvex(tacc(1.f));
        dl->PathArcTo({x + RC, y + H - RC}, RC, 0.5f * kPi + dang, kPi);
        dl->PathLineTo({x + EDGE_W, y + H - RC});
        dl->PathFillConvex(tacc(1.f));

        float ix = x + 14.f * S, iy = y + (H - 52.f * S) * .5f, is = 52.f * S;
        const auto& icon_key = n.icon_key.empty() ? n.title : n.icon_key;
        ImTextureID notif_tex = n.icon_rgba ? get_or_create_icon(icon_key, *n.icon_rgba, n.icon_width, n.icon_height) : icons_.find(icon_key);
        if (notif_tex) {
            dl->AddImageRounded(notif_tex,
                {ix,iy},{ix+is,iy+is},{0,0},{1,1}, col(P_WHT,a), RI);
        } else {
            dl->AddRectFilled({ix,iy},{ix+is,iy+is}, col(P_BG2, a), RI);
            if (n.summary) {
                ImVec2 sc = { ix + is * .5f, iy + is * .5f };
                draw_star(dl, sc, is * .30f, is * .30f,
                    col(P_GLD, a * (60.f / 255.f)));
                draw_star(dl, sc, is * .22f, is * .095f, tacc(1.f));
            }
        }
        dl->AddRect({ix-TL,iy-TL},{ix+is+TL,iy+is+TL}, tacc(A_ACC_LINE), RC, 0, TL);

        float tx = ix + is + 12.f * S;

        float ly = y + 12.f * S;
        clip_text(dl, fsmall, 12.f * S, {tx, ly}, {tx+tw, ly+16.f*S}, tacc(1.f), n.header.c_str());

        float ty2 = ly + 18.f * S;
        clip_text(dl, ftitle, 17.f * S, {tx, ty2}, {tx+tw, ty2+22.f*S}, col(P_TXT, a), n.title.c_str());

        if (desc_lines >= 1) {
            float dy = ty2 + 23.f * S;
            clip_text(dl, fsmall, 14.f * S, {tx, dy}, {tx+tw, dy+38.f*S}, col(P_MUT, a),
                d1.c_str(), desc_lines == 2 ? d2.c_str() : nullptr, 17.f*S);
        }

        float prog = clamp01(n.time_remaining / 5.f);
        dl->AddRectFilled({x + RC, y + H - BAR_H}, {x + W - RC, y + H}, col(P_SEP, a * A_TRACK), 0.f);
        card_bottom_wedge(dl, {x, y}, {x + W, y + H}, RC, BAR_H, col(P_SEP, a * A_TRACK), false);
        card_bottom_wedge(dl, {x, y}, {x + W, y + H}, RC, BAR_H, col(P_SEP, a * A_TRACK), true);
        // Fill shares the track's confined ends; its free (right) end keeps
        // the small cap while wide and goes square once narrow.
        float fw = W * prog;
        if (fw >= 1.f) {
            ImU32 fcol = tacc(A_ACC_EMPH);
            bool lo = fw >= RC;
            bool hi = fw >= W - RC;
            float mx0 = lo ? x + RC : x;
            float mx1 = hi ? x + W - RC : x + fw;
            if (lo) card_bottom_wedge(dl, {x, y}, {x + W, y + H}, RC, BAR_H, fcol, false);
            if (hi) card_bottom_wedge(dl, {x, y}, {x + W, y + H}, RC, BAR_H, fcol, true);
            if (mx1 - mx0 >= 1.f)
                dl->AddRectFilled({mx0, y + H - BAR_H}, {mx1, y + H}, fcol,
                    (!hi && lo && mx1 - mx0 >= BAR_H) ? 1.f * S : 0.f);
        }
        if (ntop) y += H + GAP;
    }
}

