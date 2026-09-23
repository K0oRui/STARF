#include "overlay/ui/overlay_style.h"
#include "overlay/core/overlay_util.h"
#include "core/settings.h"
#include "core/star_common.h"
#include <windows.h>
#include <algorithm>
#include <string>

void OverlayStyle::resolve_accent()
{
    const std::string& a = Settings::get().overlay_accent;
    for (const auto& e : kOverlayAccents) {
        if (a == e.name) { r_ = e.r; g_ = e.g; b_ = e.b; return; }
    }
    r_ = kOverlayAccents[0].r; g_ = kOverlayAccents[0].g; b_ = kOverlayAccents[0].b; // blue
}

void OverlayStyle::setup()
{
    resolve_accent();
    scale_ = std::clamp(Settings::get().overlay_scale, 0.75f, 2.0f);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Bake glyphs at the final scaled size so AddText(size*scale) and
    // PushFont+Text both hit the raster 1:1 instead of bitmap-scaling.
    io.FontGlobalScale = 1.0f;

    char windir[MAX_PATH]{}; GetWindowsDirectoryA(windir, MAX_PATH);
    std::string fd = std::string(windir) + "\\Fonts\\";
    const char* faces[] = { "segoeui.ttf","arial.ttf","tahoma.ttf",nullptr };

    ImFontConfig fc; fc.OversampleH = 4; fc.OversampleV = 4; fc.PixelSnapH = false;
    // Segoe UI sits ~0.8px low at 16px; -1px corrects it (scaled with UI).
    fc.GlyphOffset.y = -1.0f * scale_;

    // Custom TTF first (overlay.star `font`), then system faces.
    std::string custom = Settings::get().overlay_font;
    custom.erase(custom.find_last_not_of(" \t\r\n") + 1); // npos+1 wraps to 0: empty/all-blank clears
    std::string custom_path;
    if (!custom.empty()) {
        bool absolute = (custom.size() > 1 && custom[1] == ':') || custom[0] == '\\' || custom[0] == '/';
        // Relative names live in STAR/Fonts (e.g. font = poppins.ttf).
        custom_path = absolute ? custom : Settings::get().settings_dir + "\\Fonts\\" + custom;
        DWORD attr = GetFileAttributesA(custom_path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            STAR_LOG_WARN("Overlay font not found: %s", custom_path.c_str());
            custom_path.clear();
        }
    }

    const float base_default = 16.f * scale_;
    auto tryFont = [&](float sz) -> ImFont* {
        if (!custom_path.empty()) {
            if (auto* f = io.Fonts->AddFontFromFileTTF(custom_path.c_str(), sz, &fc)) {
                if (sz == base_default) STAR_LOG("Overlay font: %s", custom_path.c_str());
                return f;
            }
            STAR_LOG_WARN("Overlay font failed to load: %s", custom_path.c_str());
            custom_path.clear();
        }
        for (int i = 0; faces[i]; i++) {
            if (auto* f = io.Fonts->AddFontFromFileTTF((fd+faces[i]).c_str(), sz, &fc))
                return f;
        }
        return nullptr;
    };

    small_ = tryFont(14.f * scale_);
    title_ = tryFont(19.f * scale_);
    if (ImFont* fb = tryFont(16.f * scale_)) io.FontDefault = fb;
    else io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.ScaleAllSizes(scale_);
    s.WindowRounding   = 0.f;
    s.ChildRounding    = R_ICON * scale_;
    s.FrameRounding    = R_ICON * scale_;
    s.ScrollbarRounding= R_SMALL * scale_;
    s.GrabRounding     = R_SMALL * scale_;
    s.WindowBorderSize = 1.f;
    s.ChildBorderSize  = 0.f;
    s.FrameBorderSize  = 0.f;
    s.WindowPadding    = { 18.f * scale_, 16.f * scale_ };
    s.FramePadding     = { 10.f * scale_,  7.f * scale_ };
    s.ItemSpacing      = { 10.f * scale_, 10.f * scale_ };
    s.ScrollbarSize    = 10.f * scale_;
    s.ButtonTextAlign  = { 0.5f, 0.5f };

    auto* C = s.Colors;
    C[ImGuiCol_WindowBg]           = v4(P_BG0, A_BG);
    C[ImGuiCol_ChildBg]            = v4(P_BG1, 1.f);
    C[ImGuiCol_Border]             = v4(P_SEP, A_BORDER);
    C[ImGuiCol_FrameBg]            = v4(P_BG2, 1.f);
    C[ImGuiCol_FrameBgHovered]     = v4(P_HOV, 1.f);
    C[ImGuiCol_Button]             = v4(P_BG2, 1.f);
    C[ImGuiCol_ButtonHovered]      = v4(P_HOV, 1.f);
    C[ImGuiCol_ButtonActive]       = v4(P_ACT, 1.f);
    C[ImGuiCol_Header]             = v4(P_BG2, 1.f);
    C[ImGuiCol_HeaderHovered]      = v4(P_HOV, 1.f);
    C[ImGuiCol_ScrollbarBg]        = v4(P_BG0, 1.f);
    C[ImGuiCol_ScrollbarGrab]      = v4(P_DIM, 1.f);
    C[ImGuiCol_ScrollbarGrabHovered]= v4(P_MUT, 1.f);
    C[ImGuiCol_ScrollbarGrabActive] = vacc(1.f);
    C[ImGuiCol_Separator]          = v4(P_SEP, 0.5f);
    C[ImGuiCol_Text]               = v4(P_TXT, 1.f);
    C[ImGuiCol_TextDisabled]       = v4(P_MUT, 1.f);
    C[ImGuiCol_TitleBg]            = v4(P_BG0, 1.f);
    C[ImGuiCol_TitleBgActive]      = v4(P_BG0, 1.f);
    C[ImGuiCol_PopupBg]            = v4(P_BG1, A_POPUP);
    C[ImGuiCol_CheckMark]          = vacc(1.f);
    C[ImGuiCol_SliderGrab]         = vacc(1.f);
}
