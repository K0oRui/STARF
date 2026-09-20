#include "overlay/overlay_internal.h"
#include "core/settings.h"
#include "steam/steam_user_stats.h"
#include "steam/steam_utils.h"
#include "imgui.h"
#include <d3d9.h>
#include <d3d10.h>
#include <cctype>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <algorithm>

void StarOverlay::render_panel()
{
    ImGuiIO& io   = ImGui::GetIO();
    float sw      = io.DisplaySize.x;
    float sh      = io.DisplaySize.y;
    ImFont* fsmall = (ImFont*)style_.small_font();
    ImFont* ftitle = (ImFont*)style_.title_font();

    // panel_anim_ is already eased (easeOut on the raw progress); slide and
    // alpha share the same curve so the fade stays in sync with the motion.
    float slide   = panel_anim_;
    float alpha   = panel_anim_;

    const float PW = 420.f * style_.scale();

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        {0,0}, {sw,sh}, col(0,0,0, 0.35f * alpha));

    float px = sw - PW * slide;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::SetNextWindowPos({px, 0.f});
    ImGui::SetNextWindowSize({PW, sh});
    ImGui::SetNextWindowBgAlpha(0.97f);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##star_sidebar", nullptr, wf);

    panel_header(fsmall, ftitle, PW);
    panel_screenshots(fsmall, sw, sh);
    panel_display(fsmall);
    panel_notes(fsmall);
    panel_achievements(fsmall, ftitle, PW, sw, sh);
    panel_achievement_list(fsmall, ftitle);

    ImGui::End();
    ImGui::PopStyleVar();
}

void StarOverlay::panel_header(ImFont* fsmall, ImFont* ftitle, float pw)
{
    ImGui::PushFont(ftitle);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("STAR");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.f, 8.f);
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
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cw);
        ImGui::PushStyleColor(ImGuiCol_Text, style_.vacc(1.f));
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
    case GraphicsAPI::DX7:    gfx = "DirectX 7"; break;
    case GraphicsAPI::DX8:    gfx = "DirectX 8"; break;
    case GraphicsAPI::DX9:    gfx = "DirectX 9"; break;
    case GraphicsAPI::DX10:   gfx = "DirectX 10"; break;
    case GraphicsAPI::DX11:   gfx = "DirectX 11"; break;
    case GraphicsAPI::DX12:   gfx = "DirectX 12"; break;
    case GraphicsAPI::OpenGL: gfx = "OpenGL"; break;
    case GraphicsAPI::Vulkan: gfx = "Vulkan"; break;
case GraphicsAPI::GDI:    gfx = "GDI"; break;
    default: break;
    }
    ImGui::Text("Graphics: %s", gfx);
    ImGui::Text("Total playtime: %s", format_playtime(total_playtime_sec()).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void StarOverlay::panel_screenshots(ImFont* fsmall, float sw, float sh)
{
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
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          style_.vacc(1.f));
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
        ImGui::TextWrapped("Shots: %s", ScreenshotService::dir().c_str());
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
            std::string pattern = ScreenshotService::dir() + "\\*.png";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        ShotEntry e;
                        e.name = fd.cFileName;
                        e.path = ScreenshotService::dir() + "\\" + e.name;
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
                if (icons_.contains("shot_" + e.name)) continue;
                enqueue_icon_decode(e.path, "shot_" + e.name);
            }
        }

        float avail = ImGui::GetContentRegionAvail().x;
        const float tsz = 56.f, tgap = 8.f;
        // ImageButton adds FramePadding around the image; zero it so the
        // math below holds and thumbs never spill past the panel edge.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.f, 0.f});
        int per_row = (std::max)(1, (int)((avail + tgap - 1.f) / (tsz + tgap)));
        for (size_t i = 0; i < shots.size(); i++) {
            if (i > 0 && (i % (size_t)per_row) != 0) ImGui::SameLine(0.f, tgap);
            ImGui::PushID((int)i);
            ImTextureID tex = icons_.find("shot_" + shots[i].name);
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
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT,    1.f));
            if (ImGui::Button("Open folder")) {
                ShellExecuteA(nullptr, "open", ScreenshotService::dir().c_str(),
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
                if (iw > 0 && ih > 0) {
                    // Fill the screen: 70% width / 72% height, aspect kept.
                    float vw = sw * 0.7f;
                    float vh = vw * (float)ih / (float)iw;
                    float maxh = sh * 0.72f;
                    if (vh > maxh) { vh = maxh; vw = vh * (float)iw / (float)ih; }
                    float maxw = sw * 0.75f;
                    if (vw > maxw) { vw = maxw; vh = vw * (float)ih / (float)iw; }
                    ImGui::Image(vtex, {vw, vh});
                }
            }
            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
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
                // Textures may already be referenced by this frame's draw list;
                // bounded cache eviction releases them on a later frame.
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
}

void StarOverlay::panel_achievements(ImFont* fsmall, ImFont* ftitle, float pw, float sw, float sh)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

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
    ImGui::SameLine(0.f, 8.f);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
    ImGui::Text("%d / %d", done, total);
    ImGui::PopStyleColor();

    {
        const char* lbl = "Test notify";
        float bw = ImGui::CalcTextSize(lbl).x + 14.f;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
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
        float  bw  = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(cur, {cur.x+bw, cur.y+3.f}, col(P_SEP, 0.6f), 2.f);
        dl->AddRectFilled(cur, {cur.x+bw*pct, cur.y+3.f}, style_.acc(0.9f), 2.f);
        ImGui::Dummy({bw, 5.f});

        // Bulk actions (confirmation modal guards misclicks).
        float bw2 = (bw - 4.f) / 2.f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,       1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x1a,0x2c,0x44, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x1e,0x36,0x54, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          style_.vacc(1.f));
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
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,    1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x3a,0x3a,0x3a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          v4(P_MUT, 1.f));
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0.f, 8.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        bulk_is_unlock_ ? style_.vacc(0.22f) : v4(0x38,0x1a,0x1a, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bulk_is_unlock_ ? style_.vacc(0.3f)  : v4(0x44,0x20,0x20, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  style_.vacc(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text,          bulk_is_unlock_ ? style_.vacc(1.f) : v4(P_TXT, 1.f));
            if (ImGui::Button("Confirm")) {
                if (bulk_is_unlock_) {
                    stats.set_all_achievements(true);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%d achievements", total);
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
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::Text("+%d this session", session_unlocks_);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
}

void StarOverlay::panel_display(ImFont* fsmall)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

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
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
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
        auto& s = Settings::get();
        for (int i = 0; i < 6; i++) {
            if (i > 0) ImGui::SameLine(0.f, 4.f);
            ImGui::PushID(i);
            ImVec4 c = { swatches[i].r / 255.f, swatches[i].g / 255.f, swatches[i].b / 255.f, 1.f };
            if (ImGui::ColorButton("##acc", c, ImGuiColorEditFlags_NoTooltip, { 30.f, 30.f })) {
                s.overlay_accent = swatches[i].name;
                style_.resolve_accent();
                ImGuiStyle& st = ImGui::GetStyle();
                st.Colors[ImGuiCol_ScrollbarGrabActive] = style_.vacc(1.f);
                st.Colors[ImGuiCol_CheckMark] = style_.vacc(1.f);
                st.Colors[ImGuiCol_SliderGrab] = style_.vacc(1.f);
                save_overlay_key("accent", s.overlay_accent);
                STAR_LOG("Overlay accent -> %s", s.overlay_accent.c_str());
            }
            if (s.overlay_accent == swatches[i].name)
                dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style_.acc(1.f), 4.f, 0, 2.f);
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
                ImGui::PushStyleColor(ImGuiCol_Button,        active ? style_.vacc(0.22f) : v4(P_BG2, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? style_.vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  style_.vacc(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text,          active ? style_.vacc(1.f) : v4(P_MUT, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
                if (ImGui::Button(hud_lbl[i], {hud_w, 30.f})) {
                    *hud_val[i] = !*hud_val[i];
                    save_overlay_key(hud_key[i], *hud_val[i] ? "true" : "false");
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
            }
        }

        ImGui::Spacing();
        {
            ImGui::PushFont(fsmall);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
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
                ImGui::PushStyleColor(ImGuiCol_Button,        active ? style_.vacc(0.22f) : v4(P_BG2, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? style_.vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  style_.vacc(0.35f));
                ImGui::PushStyleColor(ImGuiCol_Text,          active ? style_.vacc(1.f) : v4(P_MUT, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
                if (ImGui::Button(corners[i].label)) {
                    s.overlay_notify_pos = corners[i].value;
                    save_overlay_key("notify_pos", s.overlay_notify_pos);
                    STAR_LOG("Overlay notify corner -> %s", s.overlay_notify_pos.c_str());
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                if (active)
                    dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style_.acc(0.7f), 3.f, 0, 1.5f);
            }
        }
    }
}

void StarOverlay::panel_notes(ImFont* fsmall)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Per-game notes (STAR/notes.txt, travels with the game copy) ----
    {
        ImGui::PushFont(fsmall);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::Text("NOTES");
        ImGui::SameLine(0.f, 8.f);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, notes_.dirty() ? style_.vacc(1.f) : v4(P_MUT, 1.f));
        ImGui::Text("%s", notes_.dirty() ? "(unsaved)" : "(auto-saved)");
        ImGui::PopStyleColor();
        ImGui::PopFont();

        static char buf[8192] = {};
        static bool buf_init = false;
        if (!buf_init) {
            buf_init = true;
            strncpy_s(buf, notes_.text().c_str(), _TRUNCATE);
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(P_BG1, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_TXT, 1.f));
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextMultiline("##notes", buf, sizeof(buf), { -1.f, 110.f },
                ImGuiInputTextFlags_AllowTabInput)) {
            notes_.set_text(buf);
        }
        ImGui::PopStyleColor(2);

        if (notes_.autosave_due(GetTickCount()))
            notes_.save();
    }
}

void StarOverlay::panel_achievement_list(ImFont* fsmall, ImFont* ftitle)
{
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
            ImGui::PushStyleColor(ImGuiCol_Button,        active ? style_.vacc(0.22f) : v4(P_BG2, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? style_.vacc(0.3f)  : v4(0x30,0x30,0x30, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  style_.vacc(0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text,          active ? style_.vacc(1.f) : v4(P_MUT, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f, 4.f});
            if (ImGui::Button(tabs[i], {tab_w, 30.f})) filter_mode_ = i;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    float ach_max_h = 400.f * style_.scale();
    ImGui::BeginChild("##ach", {0, ach_max_h}, false, 0);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

    auto& s = Settings::get();
    auto& stats = StarSteamUserStats::get();
    const float S = style_.scale();
    const float ICON_S = 44.f;
    const float ICON_X = 12.f;

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

        float tx_probe = ICON_X + ICON_S + 12.f * S;
        float tw_probe = rw - tx_probe - 92.f * S;

        bool is_hidden = def.hidden && !got;
        const char* raw_name = is_hidden ? "(Hidden achievement)"
            : ((!def.display_name.empty()) ? def.display_name.c_str() : def.name.c_str());
        const char* raw_desc = (!def.description.empty()) ? def.description.c_str() : nullptr;
        std::string d1, d2;
        int desc_lines = (!is_hidden && raw_desc)
            ? wrap_two_lines(fsmall, 15.f * S, raw_desc, tw_probe, d1, d2) : 0;

        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(("##r_"+def.name).c_str(), {rw, ROW_H});
        bool hovered = ImGui::IsItemHovered();

        if (hovered)
            dl->AddRectFilled(rmin, {rmin.x+rw, rmin.y+ROW_H}, col(P_BG2, 0.6f));

        float ix2 = rmin.x + ICON_X;
        float iy2 = rmin.y + (ROW_H - ICON_S) * .5f;

        std::string ikey = (got ? "p_" : "g_") + def.name;
        ImTextureID icon_tex = nullptr;
        if (icons_.contains(ikey)) {
            icon_tex = icons_.find(ikey);
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
                style_.acc(0.6f), 4.f, 0, 1.5f);

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
            dl->AddText(fsmall, 14.f * S, {tx,dy}, col(P_LGT, 1.f), d1.c_str());
            if (desc_lines == 2)
                dl->AddText(fsmall, 14.f * S, {tx,dy+19.f*S}, col(P_LGT, 1.f), d2.c_str());
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
                ImVec2 tsz = fsmall->CalcTextSizeA(14.f * S, FLT_MAX, 0.f, ts.c_str());
                dl->AddText(fsmall, 14.f * S,
                    {btn_x + 72.f * S - tsz.x, btn_y + 30.f * S},
                    col(P_MUT, 1.f), ts.c_str());
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        v4(P_BG2,       1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(0x1a,0x2c,0x44, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  v4(0x1e,0x36,0x54, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          style_.vacc(1.f));
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
        ImGui::PushStyleColor(ImGuiCol_Text, v4(P_MUT, 1.f));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

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
