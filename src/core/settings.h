#pragma once
#include "core/star_common.h"

struct DlcEntry {
    uint32_t app_id;
    std::string name;
};

struct AchievementDef {
    std::string name;
    std::string display_name;
    std::string description;
    std::string icon_path;
    std::string icon_gray_path;
    bool hidden = false;
};

class Settings {
public:
    static Settings& get();

    void load(const std::string& settings_dir);

    uint32_t app_id = 0;
    std::string account_name = "STAR Player";
    uint64_t steam_id = 76561198000000001ULL;
    std::string language = "english";
    std::vector<std::string> supported_languages;
    std::string supported_languages_str;
    bool unlock_all_dlc = false;
    bool is_beta_branch = false;
    std::string branch_name = "public";
    std::vector<DlcEntry> dlc_list;
    std::vector<AchievementDef> achievements;
    bool overlay_enabled = true;
    float overlay_scale = 1.25f;         // UI scale multiplier (0.75 - 2.0)
    std::string overlay_accent = "blue"; // blue | red | green | purple | orange | yellow
    bool overlay_show_fps = false;       // FPS counter HUD (top-left)
    bool overlay_show_playtime = false;  // session playtime HUD (top-left)
    bool overlay_play_sound = true;      // achievement unlock sound
    std::string overlay_notify_pos = "bottom_right"; // top_left|top_right|bottom_left|bottom_right
    std::string overlay_mode = "auto";   // auto | hook | external
    int overlay_fallback_count = 0;      // auto-fallback: sessions left to skip before retrying hooks
    int overlay_fallback_level = 0;      // auto-fallback: backoff level (0-6, skip window = 2^level - 1)
    std::string overlay_font;            // custom TTF (abs path or STAR-relative), empty = system font

    std::string settings_dir;

private:
    Settings() = default;
};
