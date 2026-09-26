#include "core/settings.h"
#include "core/config.h"
#include "core/art_stamp.h"
#include "core/storage.h"
#include <nlohmann/json.hpp>

Settings& Settings::get()
{
    static Settings instance;
    return instance;
}

static void write_default_file(const std::string& path, const char* content)
{
    std::ifstream probe(utf8_to_wstring(path));
    if (probe.is_open()) return; // never overwrite user files
    std::ofstream out(utf8_to_wstring(path));
    if (!out.is_open()) {
        STAR_LOG_ERROR("bootstrap: cannot create %s", path.c_str());
        return;
    }
    out << content;
    STAR_LOG("bootstrap: created %s", path.c_str());
}

// First-run bootstrap: recipients of bare DLLs get a complete, working
// STAR folder (defaults only; existing files are never touched).
static void bootstrap_star_folder(const std::string& dir)
{
    if (!Storage::ensure_dir(dir)) {
        STAR_LOG_ERROR("bootstrap: cannot create settings dir %s", dir.c_str());
        return;
    }
    Storage::ensure_dir(dir + "\\Fonts"); // custom overlay TTFs live here
    write_default_file(dir + "\\identity.star",
        "# Fake Steam identity shown to the game.\n"
        "# display_name: anything - shown in overlay + reported to the game.\n"
        "display_name = STAR Player\n"
        "# xuid: 64-bit SteamID, must be greater than 76561190000000000 (see steamid.io).\n"
        "xuid = 76561198000000001\n"
        "# locale: Steam language name (english, french, german, spanish, ...).\n"
        "# Must also be listed in languages.star or it falls back to the first entry.\n"
        "locale = english\n");
    write_default_file(dir + "\\game.star",
        "# Game branch config.\n"
        "# beta: true | false - report the beta branch instead of public.\n"
        "beta = false\n"
        "# branch: branch name reported to the game (usually \"public\").\n"
        "branch = public\n"
        "# dlc.unlock_all: true | false - report every DLC as owned.\n"
        "dlc.unlock_all = false\n");
    write_default_file(dir + "\\languages.star",
        "# Languages the game may claim to support. One \"<name> = 1\" per line,\n"
        "# same names as identity.star locale. Missing locale falls back to first entry.\n"
        "[languages]\n"
        "english = 1\n"
        "french = 1\n"
        "german = 1\n"
        "spanish = 1\n"
        "russian = 1\n"
        "schinese = 1\n"
        "japanese = 1\n");
    write_default_file(dir + "\\overlay.star",
        "# STAR overlay config. Delete any key to restore its default.\n"
        "# Hotkeys: Shift+Tab (or Shift+`) opens the panel, F12 takes a screenshot.\n"
        "# Screenshots go to Documents\\STAR\\screenshots\\<appid>\\.\n"
        "# enabled: true | false - master switch. false = no hooks, no window.\n"
        "enabled = true\n"
        "# mode: auto | hook | external - auto tries hooks every launch, falls back to external window for that session if title hostile or tiny.\n"
        "#       A crash under hooks pins the next launch to external for one session. hook = never auto-fall back. external = always use the window.\n"
        "mode = auto\n"
        "# scale: 0.75 - 2.0 - UI size multiplier. Needs game restart.\n"
        "scale = 1.25\n"
        "# accent: blue | red | green | purple | orange | yellow\n"
        "accent = blue\n"
        "# show_fps: true | false - FPS counter pill, top-left, always visible.\n"
        "show_fps = false\n"
        "# show_playtime: true | false - session clock pill next to FPS.\n"
        "show_playtime = false\n"
        "# play_sound: true | false - achievement unlock jingle.\n"
        "play_sound = true\n"
        "# click_through: true | false - false keeps mouse hits on the overlay when closed.\n"
        "click_through = true\n"
        "# notify_pos: top_left | top_right | bottom_left | bottom_right (tl | tr | bl | br work too)\n"
        "notify_pos = bottom_right\n"
        "# font: custom TTF for the overlay. Put the file in STAR/Fonts and name it here (e.g. poppins.ttf).\n"
        "# Absolute paths work too. Empty = system font. Needs game restart.\n"
        "font = \n");
    write_default_file(dir + "\\achievements.json", "[]\n");
    write_default_file(dir + "\\README.txt",
        "STAR config folder (auto-generated with defaults).\n"
        "What matters most:\n"
        "  steam_appid.txt  - the game's numeric AppID (Steam store URL / SteamDB).\n"
        "                     STAR also accepts steam_appid.txt next to the game EXE.\n"
        "  identity.star    - display name, SteamID (xuid), language.\n"
        "  achievements.json - achievement API names + display text + icon paths.\n"
        "                     'name' must EXACTLY match what the game unlocks.\n"
        "                     Icons live under STAR/ (e.g. Icons/win.png).\n"
        "  overlay.star     - overlay on/off, scale, accent, HUD toggles.\n"
        "  Controller/<SET>.txt - Steam Input bindings, Goldberg format\n"
        "                     (ACTION=BUTTON or ACTION=ANALOG=MODE). See Controller/ExampleSet.txt.\n"
        "  notes.txt        - your in-overlay game notes (created on first edit).\n"
        "  Sounds/achievement.mp3|achievement.wav (.mp3 wins) - unlock sound.\n"
        "  Sounds/completion.mp3|completion.wav                  - 100% jingle (optional).\n"
        "  Icons/summary.png       - 100% toast icon (optional).\n"
        "Screenshots go to Documents\\STAR\\screenshots\\<appid>\\. Saves live in\n"
        "%APPDATA%\\STAR\\<appid>\\<steamid>\\. Shift+Tab opens the overlay.\n");
}

void Settings::load(const std::string& dir)
{
    settings_dir = dir;
    STAR_LOG("Settings::load from %s", dir.c_str());
    bootstrap_star_folder(dir);
    stamp_star_configs(dir);

    {

        std::string appid_path = dir + "\\steam_appid.txt";
        std::ifstream f(utf8_to_wstring(appid_path));
        if (f.is_open()) {
            std::string s;
            std::getline(f, s);

            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
                s.pop_back();
            try { app_id = (uint32_t)std::stoul(s); } catch (...) {}
        }
    }

    if (app_id == 0) {
        std::string stripped = dir;
        while (!stripped.empty() && (stripped.back() == '\\' || stripped.back() == '/'))
            stripped.pop_back();

        std::string parent;
        size_t last_slash = stripped.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            parent = stripped.substr(0, last_slash);
        } else {
            parent = stripped;
        }
        std::string appid_path = parent + "\\steam_appid.txt";
        std::ifstream f(utf8_to_wstring(appid_path));
        if (f.is_open()) {
            std::string s;
            std::getline(f, s);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
                s.pop_back();
            try { app_id = (uint32_t)std::stoul(s); } catch (...) {}
        }
    }

    STAR_LOG("App ID: %u", app_id);
    if (app_id == 0) {
        STAR_LOG_WARN("WARNING: no App ID found - put the numeric App ID in STAR/steam_appid.txt "
            "(or steam_appid.txt next to the game EXE). Saves and achievements need it.");
    }

    {
        IniFile ini;
        if (ini.load(dir + "\\identity.star")) {
            account_name = ini.get("", "display_name", "STAR Player");
            language     = ini.get("", "locale",       "english");
            std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) { return (char)tolower(c); });
            std::string sid_str = ini.get("", "xuid", "");
            if (!sid_str.empty()) {
                try {
                    uint64_t parsed = std::stoull(sid_str);
                    if (parsed > 76561190000000000ULL) steam_id = parsed;
                } catch (...) {}
            }
        }
    }

    {
        supported_languages.clear();
        IniFile ini;
        if (ini.load(dir + "\\languages.star")) {
            auto langs = ini.get_section("languages");
            if (langs.empty()) {
                langs = ini.get_section("");
            }
            for (const auto& kv : langs) {
                std::string lang = kv.first;
                std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return (char)tolower(c); });
                if (!lang.empty()) {
                    supported_languages.push_back(lang);
                }
            }
        }

        bool lang_found = false;
        for (const auto& l : supported_languages) {
            if (l == language) {
                lang_found = true;
                break;
            }
        }
        if (!lang_found) {
            if (!supported_languages.empty()) {
                STAR_LOG_WARN("Configured language '%s' not found in languages.star. Falling back to '%s'", language.c_str(), supported_languages.front().c_str());
                language = supported_languages.front();
            } else {
                supported_languages.push_back(language);
            }
        }

        supported_languages_str.clear();
        for (size_t i = 0; i < supported_languages.size(); ++i) {
            if (i > 0) {
                supported_languages_str += ",";
            }
            supported_languages_str += supported_languages[i];
        }
    }

    {
        IniFile ini;
        if (ini.load(dir + "\\game.star")) {
            is_beta_branch = ini.get_bool("", "beta",  false);
            branch_name    = ini.get("",     "branch", "public");
            unlock_all_dlc = ini.get_bool("", "dlc.unlock_all", false);

            for (auto& [k, v] : ini.get_section("")) {
                if (k.size() > 4 && k.substr(0, 4) == "dlc." && k != "dlc.unlock_all") {
                    try {
                        uint32_t dlc_id = (uint32_t)std::stoul(k.substr(4));
                        DlcEntry entry; entry.app_id = dlc_id; entry.name = v;
                        dlc_list.push_back(entry);
                    } catch (...) {}
                }
            }
        }
    }

    {
        IniFile ini;
        if (ini.load(dir + "\\overlay.star")) {
            overlay_enabled = ini.get_bool("", "enabled", true);
            overlay_scale = std::clamp(ini.get_float("", "scale", 1.25f), 0.75f, 2.0f);
            overlay_accent = ini.get("", "accent", "blue");
            std::transform(overlay_accent.begin(), overlay_accent.end(), overlay_accent.begin(),
                [](unsigned char c) { return (char)tolower(c); });
            overlay_show_fps = ini.get_bool("", "show_fps", false);
            overlay_show_playtime = ini.get_bool("", "show_playtime", false);
            overlay_play_sound = ini.get_bool("", "play_sound", true);
            overlay_click_through = ini.get_bool("", "click_through", true);
            overlay_font = ini.get("", "font", "");
            overlay_mode = ini.get("", "mode", "auto");
            std::transform(overlay_mode.begin(), overlay_mode.end(), overlay_mode.begin(),
                [](unsigned char c) { return (char)tolower(c); });
            if (overlay_mode != "hook" && overlay_mode != "external") overlay_mode = "auto";
            overlay_notify_pos = ini.get("", "notify_pos", "bottom_right");
            std::transform(overlay_notify_pos.begin(), overlay_notify_pos.end(), overlay_notify_pos.begin(),
                [](unsigned char c) { return (char)tolower(c); });
            if (overlay_notify_pos == "tl") overlay_notify_pos = "top_left";
            else if (overlay_notify_pos == "tr") overlay_notify_pos = "top_right";
            else if (overlay_notify_pos == "bl") overlay_notify_pos = "bottom_left";
            else if (overlay_notify_pos == "br") overlay_notify_pos = "bottom_right";
            else if (overlay_notify_pos != "top_left" && overlay_notify_pos != "top_right" &&
                     overlay_notify_pos != "bottom_left" && overlay_notify_pos != "bottom_right")
                overlay_notify_pos = "bottom_right";
        }
    }

    {
        std::string ach_path = dir + "\\achievements.json";
        std::ifstream f(utf8_to_wstring(ach_path));
        if (f.is_open()) {
            try {
                nlohmann::json j;
                f >> j;
                if (j.is_array()) {
                    for (auto& item : j) {
                        AchievementDef def;
                        def.name = item.value("name", "");
                        def.display_name = item.value("displayName", item.value("display_name", def.name));
                        def.description = item.value("description", "");
                        def.icon_path = item.value("icon", "");
                        def.icon_gray_path = item.value("icongray", item.value("icon_gray", ""));
                        std::string hidden_str = item.value("hidden", "0");
                        def.hidden = (hidden_str == "1" || hidden_str == "true");
                        if (!def.name.empty()) {
                            achievements.push_back(def);
                        }
                    }
                }
            } catch (...) {
                STAR_LOG_WARN("Failed to parse achievements.json");
            }
        }
    }

    STAR_LOG("Settings loaded: app_id=%u name='%s' achievements=%zu dlcs=%zu",
        app_id, STAR_MaskName(account_name).c_str(), achievements.size(), dlc_list.size());
}
