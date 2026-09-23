#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include <cctype>
#include <ctime>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <algorithm>
#include <unordered_map>

bool StarOverlay::styled_button(const char* label, const ImVec2& size, ImVec2 pad,
                                ImVec4 bg, ImVec4 hov, ImVec4 act, ImVec4 text)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, pad);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, act);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return clicked;
}

bool StarOverlay::pill_button(const char* label, const ImVec2& size, bool active)
{
    return styled_button(label, size, {8.f * style_.scale(), 4.f * style_.scale()},
        active ? style_.vacc(A_ACC_BG) : v4(P_BG2, 1.f),
        active ? style_.vacc(A_ACC_HOV) : v4(P_HOV, 1.f),
        style_.vacc(A_ACC_ACT),
        active ? style_.vacc(1.f) : v4(P_MUT, 1.f));
}

bool StarOverlay::muted_button(const char* label, const ImVec2& size, ImVec2 pad, ImVec4 text)
{
    return styled_button(label, size, pad, v4(P_BG2, 1.f), v4(P_HOV, 1.f), v4(P_ACT, 1.f), text);
}

namespace {
void push_popup_style(ImVec2 pad)
{
    float ps = std::clamp(Settings::get().overlay_scale, 0.75f, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, v4(P_BG1, A_POPUP));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(P_SEP, A_BORDER));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f * ps);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
}
inline void section_sep()
{
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
}
} // namespace

void StarOverlay::render_panel()
{
    ImGuiIO& io   = ImGui::GetIO();
    float sw      = io.DisplaySize.x;
    float sh      = io.DisplaySize.y;
    ImFont* fsmall = (ImFont*)style_.small_font();
    ImFont* ftitle = (ImFont*)style_.title_font();

    const float PW = 420.f * style_.scale();

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        {0,0}, {sw,sh}, col(P_BLK, 0.35f * panel_anim_));

    float px = sw - PW * panel_anim_;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panel_anim_);
    ImGui::SetNextWindowPos({px, 0.f});
    ImGui::SetNextWindowSize({PW, sh});
    ImGui::SetNextWindowBgAlpha(A_BG);

    ImGui::Begin("##star_sidebar", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoBringToFrontOnFocus);

    panel_header(fsmall, ftitle);
    panel_screenshots(fsmall, sw, sh);
    panel_display(fsmall);
    panel_notes(fsmall);
    panel_achievements(fsmall, ftitle, sw, sh);
    panel_achievement_list(fsmall, ftitle);

    ImGui::End();
    ImGui::PopStyleVar();
}

void StarOverlay::panel_header(ImFont* fsmall, ImFont* ftitle)
{
    ImGui::PushFont(ftitle);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("STAR");
    ImGui::PopStyleColor();
    ImVec2 title_min = ImGui::GetItemRectMin();
    ImVec2 title_max = ImGui::GetItemRectMax();
    float title_ascent = ftitle->Ascent;
    ImGui::PopFont();

    // Draw hint + clock via AddText so CurrLineTextBaseOffset cannot shift them.
    // Baseline-aligned: small text baseline sits on the same y as STAR's baseline.
    ImGui::PushFont(fsmall);
    float small_ascent = fsmall->Ascent;
    float small_top = title_min.y + title_ascent - small_ascent;
    ImGui::GetWindowDrawList()->AddText(fsmall, ImGui::GetFontSize(),
        { title_max.x + 8.f * style_.scale(), small_top }, col(P_DIM, 1.f), "Shift+Tab");
    time_t now = time(nullptr);
    char clk[16] = "";
    struct tm tm_now{};
    if (localtime_s(&tm_now, &now) == 0)
        strftime(clk, sizeof(clk), "%I:%M %p", &tm_now);
    float clock_x = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(clk).x;
    ImGui::GetWindowDrawList()->AddText(fsmall, ImGui::GetFontSize(),
        { clock_x, small_top }, style_.acc(1.f), clk);
    ImGui::PopFont();

    section_sep();

    auto& s = Settings::get();
    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("Account:  %s", s.account_name.c_str());
    char sid[24]; snprintf(sid, sizeof(sid), "%llu", (unsigned long long)s.steam_id);
    ImGui::Text("Steam ID: %s", sid);
    char aid[12]; snprintf(aid, sizeof(aid), "%u", s.app_id);
    ImGui::Text("App ID:   %s", aid);
    // Order matches GraphicsAPI (None first); the atomic only stores valid enumerators.
    static const char* gfx_names[] = { "detecting…", "DirectX 7", "DirectX 8", "DirectX 9", "DirectX 10", "DirectX 11", "DirectX 12", "OpenGL", "Vulkan", "GDI" };
    const char* gfx = gfx_names[(int)(GraphicsAPI)game_api_];
    ImGui::Text("Graphics: %s", gfx);
    ImGui::Text("Total playtime: %s", format_playtime(total_playtime_sec()).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void StarOverlay::panel_screenshots(ImFont* fsmall, float sw, float sh)
{
    const float S = style_.scale();
    const std::string shots_dir = ScreenshotService::dir();
    section_sep();

    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("SCREENSHOTS");
    ImGui::PopStyleColor();

    if (muted_button("Screenshot", {0.f, 0.f}, {8.f * S, 4.f * S}, style_.vacc(1.f))) request_screenshot();

    ImGui::SameLine(0.f, 8.f * S);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
    ImGui::Text("or press F12 in-game");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
    ImGui::TextWrapped("Shots: %s", shots_dir.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    struct ShotDims { FILETIME wt{}; int iw = 0, ih = 0; };
    struct ShotEntry : ShotDims { std::string path; std::string name; };
    static std::vector<ShotEntry> shots;
    static std::unordered_map<std::string, ShotDims> shot_dims;
    static size_t total_shots = 0;
    static DWORD shots_refresh = 0;
    auto resolve_dims = [&](const std::string& name, const std::string& path, FILETIME wt) -> ShotDims {
        auto it = shot_dims.find(name);
        if (it != shot_dims.end() && CompareFileTime(&it->second.wt, &wt) == 0)
            return it->second;
        ShotDims d{};
        d.wt = wt;
        uint32 uw = 0, uh = 0;
        if (StarSteamUtils::get().GetImageFileSize(path, &uw, &uh)) {
            d.iw = (int)uw; d.ih = (int)uh;
        }
        shot_dims[name] = d;
        return d;
    };
    DWORD nowt = GetTickCount();
    if (shots_refresh == 0 || nowt - shots_refresh > 5000) {
        shots_refresh = nowt;
        shots.clear();
        total_shots = 0;
        std::string pattern = shots_dir + "\\*.png";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    total_shots++;
                    ShotEntry e;
                    e.name = fd.cFileName;
                    e.path = shots_dir + "\\" + e.name;
                    e.wt = fd.ftLastWriteTime;
                    shots.push_back(e);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(shots.begin(), shots.end(), [](const ShotEntry& a, const ShotEntry& b) {
            return CompareFileTime(&a.wt, &b.wt) > 0;
        });
        if (shots.size() > 12) shots.resize(12);
        // GetImageFileSize opens a WIC decoder per file on the render thread;
        // re-querying all 12 shots every 5s hitches the panel. Dimensions are
        // immutable for an existing file, so cache by name+write-time and only
        // decode newcomers/changed files.
        for (auto& e : shots) {
            static_cast<ShotDims&>(e) = resolve_dims(e.name, e.path, e.wt);
            if (e.iw <= 0 || e.ih <= 0) continue;
            if (icons_.contains("shot_" + e.name)) continue;
            enqueue_icon_decode(e.path, "shot_" + e.name);
        }
    }

    float avail = ImGui::GetContentRegionAvail().x;
    const float th = 48.f * S, tgap = 8.f * S;
    auto thumb_w = [&](size_t i) {
        return shots[i].ih > 0 ? th * (float)shots[i].iw / (float)shots[i].ih : th;
    };

    size_t shown = 0;
    float used = 0.f;
    for (size_t i = 0; i < shots.size(); i++) {
        float need = (shown == 0 ? 0.f : tgap) + thumb_w(i);
        if (used + need > avail) break;
        used += need;
        shown++;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
    for (size_t i = 0; i < shown; i++) {
        float tw = thumb_w(i);
        if (i > 0) ImGui::SameLine(0.f, tgap);
        ImGui::PushID((int)i);
        bool is_last = (i + 1 == shown) && shown < total_shots;
        ImTextureID tex = icons_.find("shot_" + shots[i].name);
        bool clicked;
        if (tex) {
            // ImageButton draws square images; round manually so loaded thumbs
            // match the rounded placeholder buttons (FrameRounding) and scrim.
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            clicked = ImGui::InvisibleButton("##shot", {tw, th});
            ImVec2 p1 = {p0.x + tw, p0.y + th};
            ImDrawList* tdl = ImGui::GetWindowDrawList();
            tdl->AddImageRounded(tex, p0, p1,
                {0,0}, {1,1}, col(P_WHT, 1.f), R_ICON * S);
            if (ImGui::IsItemHovered())
                tdl->AddRectFilled(p0, p1, col(P_WHT, 0.18f), R_ICON * S);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, v4(P_BG2, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_DIM, 1.f));
            clicked = ImGui::Button("##shotx", {tw, th});
            ImGui::PopStyleColor(2);
        }
        bool rclicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        if ((clicked && is_last) || rclicked) {
            ShellExecuteA(nullptr, "open", shots_dir.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (clicked && !is_last) {
            viewer_file_ = shots[i].path;
        }
        if (is_last) {
            ImVec2 rmin = ImGui::GetItemRectMin();
            ImVec2 rmax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(rmin, rmax, col(P_BLK, (150.f / 255.f)), R_ICON * S);
            std::string plus = "+" + std::to_string(total_shots - shown);
            float cx = (rmin.x + rmax.x - ImGui::CalcTextSize(plus.c_str()).x) * 0.5f;
            float cy = (rmin.y + rmax.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f;
            dl->AddText({cx, cy}, col(P_WHT, 1.f), plus.c_str());
        }
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            col(P_SEP, 1.f), R_ICON * S, 0, 1.f * S);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    if (!viewer_file_.empty() && !ImGui::IsPopupOpen("##shotview"))
        ImGui::OpenPopup("##shotview");
    ImGui::SetNextWindowPos({sw * 0.5f, sh * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    push_popup_style({16.f * S, 16.f * S});
    if (ImGui::BeginPopupModal("##shotview", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoTitleBar)) {
        std::string vname = file_name(viewer_file_);
        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::Text("%s", vname.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImTextureID vtex = icons_.find("viewer_" + vname);
        if (!vtex) {
            enqueue_icon_decode(viewer_file_, "viewer_" + vname);
            vtex = icons_.find("shot_" + vname);
        }
        if (vtex) {
            int iw = 0, ih = 0;
            for (auto& e : shots) {
                if (e.name == vname && e.iw > 0 && e.ih > 0) { iw = e.iw; ih = e.ih; break; }
            }
            // Fresh screenshots set viewer_file_ before the 5s directory
            // refresh picks them up: without this the popup stays blank.
            if (iw <= 0 || ih <= 0) {
                ShotDims d = resolve_dims(vname, viewer_file_, FILETIME{});
                iw = d.iw; ih = d.ih;
            }
            if (iw > 0 && ih > 0) {
                float k = sw * 0.7f / (float)iw;
                if (sh * 0.72f / (float)ih < k) k = sh * 0.72f / (float)ih;
                float vw = (float)iw * k, vh = (float)ih * k;
                ImVec2 img_min = ImGui::GetCursorScreenPos();
                ImVec2 img_max = {img_min.x + vw, img_min.y + vh};
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddImageRounded(vtex, img_min, img_max, {0,0}, {1,1},
                    col(P_WHT, 1.f), 6.f * S);
                dl->AddRect(img_min, img_max, col(P_SEP, 1.f), 6.f * S, 0, 1.f * S);
                ImGui::Dummy({vw, vh});
            }
        }
        ImGui::Spacing();
        if (muted_button("Close", {64.f * S, 0.f}, {12.f * S, 6.f * S}, v4(P_MUT, 1.f))) {
            viewer_file_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.f, 8.f * S);
        if (styled_button("Delete", {64.f * S, 0.f}, {12.f * S, 6.f * S},
                v4(P_DNG, 1.f), v4(P_DNG_HOV, 1.f), v4(P_DNG_ACT, 1.f), v4(P_TXT, 1.f))) {
            DeleteFileA(viewer_file_.c_str());
            shots.clear();
            shots_refresh = 0;
            total_shots = 0;
            viewer_file_.clear();
            STAR_LOG("Screenshot deleted: %s", vname.c_str());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void StarOverlay::panel_achievements(ImFont* fsmall, ImFont* ftitle, float sw, float sh)
{
    const float S = style_.scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    section_sep();

    auto& s = Settings::get();
    auto& stats = StarSteamUserStats::get();
    int total = (int)s.achievements.size();
    int done  = 0;
    auto snapshot = stats.achievement_snapshot();
    for (auto& d : s.achievements) {
        auto it = snapshot.find(d.name);
        if (it != snapshot.end() && it->second.achieved) done++;
    }

    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("ACHIEVEMENTS");
    ImGui::SameLine(0.f, 8.f * S);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("%d / %d", done, total);
    ImGui::PopStyleColor();
    if (session_unlocks_ > 0) {
        ImGui::SameLine(0.f, 6.f * S);
        ImGui::PushStyleColor(ImGuiCol_Text, style_.vacc(1.f));
        ImGui::Text("(+%d)", session_unlocks_);
        ImGui::PopStyleColor();
    }

    {
        const char* lbl = "Test notify";
        float bw = ImGui::CalcTextSize(lbl).x + 14.f * S;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f * S);
        if (muted_button(lbl, {0.f, 0.f}, {8.f * S, 4.f * S}, v4(P_MUT, 1.f))) {
            StarSteamUserStats::get().play_unlock_sound();
            push_achievement("Test Achievement",
                             "Opened the STAR overlay.",
                             {}, 0, 0);
        }
    }

    ImGui::PopFont();

    if (total > 0) {
        float pct = (float)done / (float)total;
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float  bw  = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(cur, {cur.x+bw, cur.y+6.f * S}, col(P_SEP, A_TRACK), R_SMALL * S);
        if (pct > 0.f)
            dl->AddRectFilled(cur, {cur.x+bw*pct, cur.y+6.f * S}, style_.acc(0.9f), R_SMALL * S);
        ImGui::Dummy({bw, 8.f * S});

        float bw2 = (bw - 4.f * S) / 2.f;
        if (styled_button("Unlock all", {bw2, 0.f}, {8.f * S, 4.f * S},
                v4(P_BG2, 1.f), style_.vacc(A_ACC_HOV), style_.vacc(A_ACC_ACT), style_.vacc(1.f))) {
            bulk_is_unlock_ = true;
            ImGui::OpenPopup("##bulk_confirm");
        }
        ImGui::SameLine(0.f, 4.f * S);
        if (styled_button("Reset all", {bw2, 0.f}, {8.f * S, 4.f * S},
                v4(P_BG2, 1.f), v4(P_DNG_HOV, 1.f), v4(P_DNG_ACT, 1.f), v4(P_MUT, 1.f))) {
            bulk_is_unlock_ = false;
            ImGui::OpenPopup("##bulk_confirm");
        }

        ImGui::SetNextWindowPos({sw * 0.5f, sh * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        push_popup_style({20.f * S, 20.f * S});
        if (ImGui::BeginPopupModal("##bulk_confirm", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoTitleBar)) {
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
            if (muted_button("Cancel", {72.f * S, 0.f}, {12.f * S, 6.f * S}, v4(P_MUT, 1.f))) ImGui::CloseCurrentPopup();
            ImGui::SameLine(0.f, 8.f * S);
            if (styled_button("Confirm", {72.f * S, 0.f}, {12.f * S, 6.f * S},
                    bulk_is_unlock_ ? style_.vacc(A_ACC_BG) : v4(P_DNG, 1.f),
                    bulk_is_unlock_ ? style_.vacc(A_ACC_HOV) : v4(P_DNG_HOV, 1.f),
                    style_.vacc(A_ACC_ACT),
                    bulk_is_unlock_ ? style_.vacc(1.f) : v4(P_TXT, 1.f))) {
                if (bulk_is_unlock_) {
                    stats.set_all_achievements(true);
                    std::string msg = std::to_string(total) + " achievements";
                    StarSteamUserStats::get().play_completion_sound();
                    std::vector<uint8_t> sum_rgba; int sum_w = 0, sum_h = 0;
                    StarSteamUtils::get().LoadSummaryIcon(sum_rgba, sum_w, sum_h);
                    push_achievement("All achievements unlocked", msg,
                        sum_rgba, sum_w, sum_h, "100% COMPLETE", true);
                    STAR_LOG("Bulk unlock all (%d)", total);
                } else {
                    stats.set_all_achievements(false);
                    STAR_LOG("Bulk reset all (%d)", total);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
}

void StarOverlay::panel_display(ImFont* fsmall)
{
    const float S = style_.scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    section_sep();

    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("DISPLAY");
    ImGui::Spacing();
    ImGui::Text("Accent");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    auto& s = Settings::get();
    for (size_t i = 0; i < std::size(kOverlayAccents); i++) {
        const auto& sw = kOverlayAccents[i];
        if (i > 0) ImGui::SameLine(0.f, 4.f * S);
        ImGui::PushID((int)i);
        if (ImGui::ColorButton("##acc", { sw.r / 255.f, sw.g / 255.f, sw.b / 255.f, 1.f }, ImGuiColorEditFlags_NoTooltip, { 30.f * S, 30.f * S })) {
            s.overlay_accent = sw.name;
            style_.resolve_accent();
            ImGuiStyle& st = ImGui::GetStyle();
            st.Colors[ImGuiCol_ScrollbarGrabActive] = style_.vacc(1.f);
            st.Colors[ImGuiCol_CheckMark] = style_.vacc(1.f);
            st.Colors[ImGuiCol_SliderGrab] = style_.vacc(1.f);
            save_overlay_key("accent", s.overlay_accent);
            STAR_LOG("Overlay accent -> %s", s.overlay_accent.c_str());
        }
        if (s.overlay_accent == sw.name)
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style_.acc(1.f), R_ICON * S, 0, T_LINE * S);
        ImGui::PopID();
    }

    ImGui::Spacing();
    struct HudToggle { const char* label; bool* value; const char* key; };
    HudToggle hud[] = {
        { "FPS", &s.overlay_show_fps, "show_fps" },
        { "Playtime", &s.overlay_show_playtime, "show_playtime" },
        { "Sound", &s.overlay_play_sound, "play_sound" },
    };
    float hud_w = (ImGui::GetContentRegionAvail().x - 8.f * S) / 3.f;
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine(0.f, 4.f * S);
        if (pill_button(hud[i].label, {hud_w, 0.f}, *hud[i].value)) {
            *hud[i].value = !*hud[i].value;
            save_overlay_key(hud[i].key, *hud[i].value ? "true" : "false");
        }
    }

    ImGui::Spacing();
    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("Toast corner");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0.f, 8.f * S);

    struct Corner { const char* label; const char* value; };
    static const Corner corners[] = {
        { "TL", "top_left" }, { "TR", "top_right" },
        { "BL", "bottom_left" }, { "BR", "bottom_right" },
    };
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine(0.f, 4.f * S);
        bool active = (s.overlay_notify_pos == corners[i].value);
        if (pill_button(corners[i].label, {0.f, 0.f}, active)) {
            s.overlay_notify_pos = corners[i].value;
            save_overlay_key("notify_pos", s.overlay_notify_pos);
            STAR_LOG("Overlay notify corner -> %s", s.overlay_notify_pos.c_str());
        }
        if (active)
            dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style_.acc(A_ACC_EMPH), R_SMALL * S, 0, T_LINE * S);
    }
}

void StarOverlay::panel_notes(ImFont* fsmall)
{
    section_sep();

    const float S = style_.scale();
    ImGui::PushFont(fsmall);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
    ImGui::Text("NOTES");
    ImGui::SameLine(0.f, 8.f * S);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, notes_.dirty() ? style_.vacc(1.f) : v4(P_MUT, 1.f));
    ImGui::Text("%s", notes_.dirty() ? "(unsaved)" : "(auto-saved)");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    static char buf[NotesStore::kMaxBytes + 1] = {};
    static bool buf_init = false;
    if (!buf_init) {
        buf_init = true;
        strncpy_s(buf, notes_.text().c_str(), _TRUNCATE);
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(P_BG1, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextMultiline("##notes", buf, sizeof(buf), { -1.f, 110.f * S },
            ImGuiInputTextFlags_AllowTabInput)) {
        notes_.set_text(buf);
    }
    ImGui::PopStyleColor(2);

    if (notes_.autosave_due(GetTickCount()))
        notes_.save();
}

void StarOverlay::panel_achievement_list(ImFont* fsmall, ImFont* ftitle)
{
    const float S = style_.scale();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(P_BG1, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##filter", "Search achievements...",
        achievement_filter_, sizeof(achievement_filter_));
    ImGui::PopStyleColor(2);

    const char* tabs[3] = { "All", "Unlocked", "Locked" };
    float tab_w = (ImGui::GetContentRegionAvail().x - 8.f * S) / 3.f;
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine(0.f, 4.f * S);
        if (pill_button(tabs[i], {tab_w, 0.f}, filter_mode_ == i)) filter_mode_ = i;
    }

    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    float ach_max_h = 400.f * style_.scale();
    ImGui::BeginChild("##ach", {0, ach_max_h}, false, 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto& s = Settings::get();
    auto& stats = StarSteamUserStats::get();
    const float ICON_S = 44.f * S;
    const float ICON_X = 12.f * S;

    std::string needle = achievement_filter_;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    auto snapshot = stats.achievement_snapshot();
    std::vector<size_t> filtered;
    for (size_t i = 0; i < s.achievements.size(); ++i) {
        const auto& def = s.achievements[i];
        auto it = snapshot.find(def.name);
        bool got = it != snapshot.end() && it->second.achieved;
        if (filter_mode_ == 1 && !got) continue;
        if (filter_mode_ == 2 && got) continue;
        if (!needle.empty()) {
            std::string hay = def.display_name.empty() ? def.name : def.display_name;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        filtered.push_back(i);
    }
    int shown = (int)filtered.size();
    // Fixed-height rows allow ImGui to skip all layout/decode work off screen.
    const float ROW_H = 80.f * S;
    ImGuiListClipper clipper;
    clipper.Begin(shown, ROW_H);
    while (clipper.Step())
    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
        auto& def = s.achievements[filtered[index]];
        auto it = snapshot.find(def.name);
        bool got = it != snapshot.end() && it->second.achieved;
        uint32_t unlock_t = it != snapshot.end() ? it->second.unlock_time : 0;

        ImVec2 rmin = ImGui::GetCursorScreenPos();
        float  rw   = ImGui::GetContentRegionAvail().x;

        float tx = rmin.x + ICON_X + ICON_S + 12.f * S;
        float tw = rw - (tx - rmin.x) - 92.f * S;

        bool is_hidden = def.hidden && !got;
        const char* raw_name = is_hidden ? "(Hidden achievement)"
            : ((!def.display_name.empty()) ? def.display_name.c_str() : def.name.c_str());
        const char* raw_desc = (!def.description.empty()) ? def.description.c_str() : nullptr;
        std::string d1, d2;
        int desc_lines = (!is_hidden && raw_desc)
            ? wrap_two_lines(fsmall, 14.f * S, raw_desc, tw, d1, d2) : 0;

        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(("##r_"+def.name).c_str(), {rw, ROW_H});
        bool hovered = ImGui::IsItemHovered();

        if (hovered)
            dl->AddRectFilled(rmin, {rmin.x+rw, rmin.y+ROW_H}, col(P_BG2, 0.6f));

        float ix2 = rmin.x + ICON_X;
        float iy2 = rmin.y + (ROW_H - ICON_S) * .5f;

        std::string ikey = (got ? "p_" : "g_") + def.name;
        ImTextureID icon_tex = icons_.find(ikey);
        if (!icon_tex) {
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
                col(P_WHT, 1.f), R_SMALL * S);
        } else {
            dl->AddRectFilled({ix2,iy2},{ix2+ICON_S,iy2+ICON_S},
                col(P_BG2, 1.f), R_SMALL * S);
        }
        if (got)
            dl->AddRect({ix2-1.5f * S,iy2-1.5f * S},{ix2+ICON_S+1.5f * S,iy2+ICON_S+1.5f * S},
                style_.acc(A_ACC_LINE), R_ICON * S, 0, T_LINE * S);

        if (got)
            dl->AddCircleFilled({ix2 + ICON_S - 6.f * S, iy2}, 5.f * S, col(0x4c,0xb8,0x4c, 1.f));

        float ty0 = rmin.y + 11.f * S;

        clip_text(dl, ftitle, 17.f * S, {tx, ty0}, {tx+tw, ty0+22.f*S}, got ? col(P_TXT, 1.f) : col(P_MUT, 1.f), raw_name);

        if (is_hidden) {
            float dy = ty0 + 23.f * S;
            dl->AddRectFilled({tx,dy},{tx+tw*0.7f,dy+12.f*S}, col(P_DIM, 0.45f), R_SMALL * S);
        } else if (desc_lines >= 1) {
            float dy = ty0 + 23.f * S;
            clip_text(dl, fsmall, 14.f * S, {tx, dy}, {tx+tw, dy+40.f*S}, col(0x9e,0x9e,0x9e, 1.f),
                d1.c_str(), desc_lines == 2 ? d2.c_str() : nullptr, 19.f*S);
        }

        float btn_x = rmin.x + rw - 84.f * S;
        float btn_y = rmin.y + (ROW_H - 26.f * S) * .5f;

        ImGui::SetCursorScreenPos({btn_x, btn_y});

        if (got) {
            if (styled_button(("Reset##" + def.name).c_str(), {72.f * S, 26.f * S}, {8.f * S, 4.f * S},
                    v4(P_BG2, 1.f), v4(P_DNG_HOV, 1.f), v4(P_DNG_ACT, 1.f), v4(P_MUT, 1.f)))
                stats.ClearAchievement(def.name.c_str());
            if (unlock_t) {
                std::string ts = fmt_unlock_time(unlock_t);
                ImVec2 tsz = fsmall->CalcTextSizeA(14.f * S, FLT_MAX, 0.f, ts.c_str());
                dl->AddText(fsmall, 14.f * S,
                    {btn_x + 72.f * S - tsz.x, btn_y + 30.f * S},
                    col(P_MUT, 1.f), ts.c_str());
            }
        } else {
            if (styled_button(("Unlock##" + def.name).c_str(), {72.f * S, 26.f * S}, {8.f * S, 4.f * S},
                    v4(P_BG2, 1.f), style_.vacc(A_ACC_HOV), style_.vacc(A_ACC_ACT), style_.vacc(1.f)))
                stats.SetAchievement(def.name.c_str());
        }

        ImGui::SetCursorScreenPos({rmin.x, rmin.y + ROW_H});
        dl->AddLine({rmin.x, rmin.y + ROW_H}, {rmin.x + rw, rmin.y + ROW_H}, col(P_SEP, 0.3f));
    }

    if (s.achievements.empty() || shown == 0) {
        const char* msg = s.achievements.empty()
            ? "No achievements in STAR/achievements.json"
            : "No achievements match your search";
        ImGui::Dummy({0.f, 16.f * S});
        ImGui::PushFont(fsmall);
        float msg_w = ImGui::CalcTextSize(msg).x;
        float avail_w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - msg_w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void StarOverlay::push_achievement(const std::string& name, const std::string& desc,
                                   const std::vector<uint8_t>& rgba, int iw, int ih,
                                   const std::string& header, bool summary)
{
    if (!enabled_) return;
    AchievementNotification n;
    n.header = header; n.summary = summary; n.title = name; n.description = desc;
    if (!rgba.empty()) n.icon_rgba = std::make_shared<const std::vector<uint8_t>>(rgba);
    n.icon_width = iw; n.icon_height = ih;
    n.time_remaining = 5.f; n.age = 0.f;
    notifications_.push(std::move(n));
}
