#include "steam/steam_user_stats.h"
#include "core/callbacks.h"
#include "core/settings.h"
#include "core/storage.h"
#include "steam/steam_utils.h"
#include "overlay/overlay.h"
#include <atomic>
#include <thread>
#include <mmsystem.h>

StarSteamUserStats& StarSteamUserStats::get()
{
    static StarSteamUserStats instance;
    return instance;
}

void StarSteamUserStats::load_from_storage()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stats_loaded_) return;
    stats_loaded_ = true;
    for (const auto& def : Settings::get().achievements) known_achievements_.insert(def.name);

    nlohmann::json ach_json;
    if (Storage::get().load_achievements(ach_json) && ach_json.is_object()) {
        for (auto it = ach_json.begin(); it != ach_json.end(); ++it) {
            AchievementState state;
            state.achieved = it.value().value("achieved", false);
            state.unlock_time = it.value().value("unlock_time", 0u);
            achievements_[it.key()] = state;
        }
    }

    nlohmann::json stats_json;
    if (Storage::get().load_stats(stats_json) && stats_json.is_object()) {
        for (auto it = stats_json.begin(); it != stats_json.end(); ++it) {
            StatValue sv;
            std::string type = it.value().value("type", "int");
            if (type == "float") {
                sv.type = StatValue::FLOAT_TYPE;
                sv.float_val = it.value().value("value", 0.0f);
            } else {
                sv.type = StatValue::INT_TYPE;
                sv.int_val = it.value().value("value", 0);
            }
            stats_[it.key()] = sv;
        }
    }
    STAR_LOG("UserStats loaded: %zu achievements, %zu stats", achievements_.size(), stats_.size());
}

bool StarSteamUserStats::RequestCurrentStats()
{
    load_from_storage();

    UserStatsReceived_t cb{};
    cb.m_nGameID = Settings::get().app_id;
    cb.m_eResult = k_EResultOK;
    cb.m_steamIDUser = CSteamID(Settings::get().steam_id);
    STAR_PostCallback(UserStatsReceived_t::k_iCallback, &cb, sizeof(cb));
    STAR_LOG("RequestCurrentStats -> queued UserStatsReceived_t");
    return true;
}

bool StarSteamUserStats::GetStat(const char* pchName, int32* pData)
{
    if (!pchName || !pData) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(pchName);
    if (it == stats_.end()) { *pData = 0; return false; }
    if (it->second.type == StatValue::FLOAT_TYPE) {
        *pData = (int32)it->second.float_val;
    } else {
        *pData = it->second.int_val;
    }
    return true;
}

bool StarSteamUserStats::GetStat(const char* pchName, float* pData)
{
    if (!pchName || !pData) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(pchName);
    if (it == stats_.end()) { *pData = 0.0f; return false; }
    if (it->second.type == StatValue::INT_TYPE) {
        *pData = (float)it->second.int_val;
    } else {
        *pData = it->second.float_val;
    }
    return true;
}

bool StarSteamUserStats::SetStat(const char* pchName, int32 nData)
{
    if (!pchName) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& sv = stats_[pchName];
    sv.type = StatValue::INT_TYPE;
    sv.int_val = nData;
    return true;
}

bool StarSteamUserStats::SetStat(const char* pchName, float fData)
{
    if (!pchName) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& sv = stats_[pchName];
    sv.type = StatValue::FLOAT_TYPE;
    sv.float_val = fData;
    return true;
}

bool StarSteamUserStats::UpdateAvgRateStat(const char* pchName, float flCountThisSession, double dSessionLength)
{
    if (!pchName || dSessionLength <= 0.0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& sv = stats_[pchName];
    sv.type = StatValue::FLOAT_TYPE;

    float session_avg = (float)(flCountThisSession / dSessionLength);
    if (sv.float_val == 0.0f) {
        sv.float_val = session_avg;
    } else {
        sv.float_val = (sv.float_val + session_avg) * 0.5f;
    }
    return true;
}

bool StarSteamUserStats::GetAchievement(const char* name, bool* achieved)
{
    if (!name) return false;
    load_from_storage();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = achievements_.find(name);
    if (achieved) *achieved = it != achievements_.end() && it->second.achieved;
    return it != achievements_.end() || known_achievements_.count(name) != 0;
}

std::unordered_map<std::string, AchievementState> StarSteamUserStats::achievement_snapshot()
{
    load_from_storage();
    std::lock_guard<std::mutex> lock(mutex_);
    return achievements_;
}

void StarSteamUserStats::set_all_achievements(bool unlocked)
{
    load_from_storage();
    std::vector<std::string> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = (uint32_t)time(nullptr);
        for (const auto& def : Settings::get().achievements) {
            auto& state = achievements_[def.name];
            if (state.achieved == unlocked) continue;
            state.achieved = unlocked;
            state.unlock_time = unlocked ? now : 0;
            changed.push_back(def.name);
        }
        if (!changed.empty()) {
            nlohmann::json json = nlohmann::json::object();
            for (const auto& [name, state] : achievements_)
                json[name] = {{"achieved", state.achieved}, {"unlock_time", state.unlock_time}};
            Storage::get().save_achievements(json);
        }
    }
    for (const auto& name : changed) {
        if (!unlocked) { Overlay::get().note_session_revoke(); continue; }
        Overlay::get().note_session_unlock();
        UserAchievementStored_t cb{};
        cb.m_nGameID = Settings::get().app_id;
        strncpy_s(cb.m_rgchAchievementName, name.c_str(), _TRUNCATE);
        STAR_DispatchCallback(UserAchievementStored_t::k_iCallback, &cb, sizeof(cb));
    }
}

static void play_sound_file(const std::string& full_path);
static void play_completion_file();

// Sounds/achievement.<mp3|wav> - .mp3 wins when both exist.
static std::string resolve_sound(const std::string& stem)
{
    for (const char* ext : { ".mp3", ".wav" }) {
        std::string p = Settings::get().settings_dir + "\\Sounds\\" + stem + ext;
        DWORD attr = GetFileAttributesW(utf8_to_wstring(p).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return p;
    }
    return {};
}

bool StarSteamUserStats::SetAchievement(const char* pchName)
{
    if (!pchName) return false;
    load_from_storage();
    bool was_newly_achieved = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = achievements_[pchName];
        if (!state.achieved) {
            state.achieved = true;
            state.unlock_time = (uint32_t)time(nullptr);
            was_newly_achieved = true;
        }
    }
    if (was_newly_achieved) {
        Overlay::get().note_session_unlock();
        {
            nlohmann::json ach_json = nlohmann::json::object();
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [name, state] : achievements_) {
                ach_json[name] = {
                    {"achieved", state.achieved},
                    {"unlock_time", state.unlock_time}
                };
            }
            Storage::get().save_achievements(ach_json);
        }

        UserAchievementStored_t cb{};
        cb.m_nGameID = Settings::get().app_id;
        cb.m_bGroupAchievement = false;
        strncpy_s(cb.m_rgchAchievementName, pchName, _TRUNCATE);
        cb.m_nCurProgress = 0;
        cb.m_nMaxProgress = 0;
        STAR_DispatchCallback(UserAchievementStored_t::k_iCallback, &cb, sizeof(cb));

        {
            // Completing the set gets the dedicated jingle + summary instead
            // of the normal unlock sound.
            auto& defs = Settings::get().achievements;
            bool completes = false;
            if (!defs.empty()) {
                int done = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& d : defs) {
                    auto it = achievements_.find(d.name);
                    if (it != achievements_.end() && it->second.achieved) done++;
                }
                completes = (done == (int)defs.size());
            }
            notify_achievement_unlock(pchName, !completes);
            if (completes) {
                play_completion_file();
                char msg[64];
                snprintf(msg, sizeof(msg), "%d achievements", (int)Settings::get().achievements.size());
                std::vector<uint8_t> sum_rgba; int sum_w = 0, sum_h = 0;
                StarSteamUtils::get().LoadSummaryIcon(sum_rgba, sum_w, sum_h);
                Overlay::get().push_achievement(
                    "All achievements unlocked", msg, sum_rgba, sum_w, sum_h,
                    "100% COMPLETE", true);
            }
        }
        STAR_LOG("Achievement unlocked: %s", pchName);
    }
    return true;
}

static void play_sound_file(const std::string& sound_path)
{
    if (sound_path.empty()) return;

    // Content sniff: misnamed files are common (e.g. WAV saved as .mp3).
    // Try the matching MCI device first, fall back to the other one.
    bool looks_wav = false;
    {
        std::ifstream f(utf8_to_wstring(sound_path), std::ios::binary);
        char magic[12] = {};
        if (f.is_open()) {
            f.read(magic, sizeof(magic));
            if (f.gcount() >= 12 && memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0)
                looks_wav = true;
        }
    }
    std::thread([sound_path, looks_wav]() {
        const std::wstring path = utf8_to_wstring(sound_path);
        const wchar_t* order[2] = { looks_wav ? L"waveaudio" : L"mpegvideo",
                                    looks_wav ? L"mpegvideo" : L"waveaudio" };
        static std::atomic<uint64_t> seq{0};
        for (int i = 0; i < 2; i++) {
            // Unique alias per request: async play returns instantly so the
            // next unlock starts over this one instead of waiting for it.
            uint64_t id = ++seq;
            wchar_t alias[64];
            swprintf(alias, 64, L"STAR_%llu_%d", (unsigned long long)id, i);
            MCI_OPEN_PARMSW open{};
            open.lpstrElementName = path.c_str();
            open.lpstrDeviceType = order[i];
            open.lpstrAlias = alias;
            MCIERROR err = mciSendCommandW(0, MCI_OPEN,
                MCI_OPEN_ELEMENT | MCI_OPEN_TYPE | MCI_OPEN_ALIAS | MCI_WAIT, (DWORD_PTR)&open);
            if (err != 0) {
                char ebuf[128] = {};
                mciGetErrorStringA(err, ebuf, (UINT)sizeof(ebuf));
                STAR_LOG("Sound: open as %ls failed (%lu/%s): %s",
                    order[i], (unsigned long)err, ebuf, sound_path.c_str());
                continue;
            }
            MCI_PLAY_PARMS play{};
            err = mciSendCommandW(open.wDeviceID, MCI_PLAY, 0, (DWORD_PTR)&play);
            if (err != 0) {
                char ebuf[128] = {};
                mciGetErrorStringA(err, ebuf, (UINT)sizeof(ebuf));
                STAR_LOG("Sound: play failed (%lu/%s): %s",
                    (unsigned long)err, ebuf, sound_path.c_str());
                mciSendCommandW(open.wDeviceID, MCI_CLOSE, MCI_WAIT, 0);
                continue;
            }
            STAR_LOG("Sound: playback started: %s", sound_path.c_str());
            for (int tick = 0; tick < 600; tick++) {
                MCI_STATUS_PARMS status{};
                status.dwItem = MCI_STATUS_MODE;
                if (mciSendCommandW(open.wDeviceID, MCI_STATUS,
                        MCI_STATUS_ITEM | MCI_WAIT, (DWORD_PTR)&status) != 0)
                    break;
                if (status.dwReturn != MCI_MODE_PLAY)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            mciSendCommandW(open.wDeviceID, MCI_CLOSE, MCI_WAIT, 0);
            return;
        }
    }).detach();
}

static void play_achievement_sound()
{
    if (!Settings::get().overlay_play_sound) return;
    play_sound_file(resolve_sound("achievement"));
}

static void play_completion_file()
{
    if (!Settings::get().overlay_play_sound) return;
    // Dedicated completion jingle, falling back to the normal unlock sound.
    std::string comp = resolve_sound("completion");
    play_sound_file(comp.empty() ? resolve_sound("achievement") : comp);
}

void StarSteamUserStats::play_unlock_sound()
{
    play_achievement_sound();
}

void StarSteamUserStats::play_completion_sound()
{
    play_completion_file();
}

void StarSteamUserStats::notify_achievement_unlock(const std::string& name, bool with_sound)
{

    auto& defs = Settings::get().achievements;
    for (auto& def : defs) {
        if (def.name == name) {
            if (with_sound) play_achievement_sound();
            if (!def.icon_path.empty()) {
                std::string full_path = Settings::get().settings_dir + "\\" + def.icon_path;
                for (char& c : full_path) if (c == '/') c = '\\';
                Overlay::get().push_achievement(def.display_name, def.description, {}, 0, 0);
                Overlay::get().enqueue_icon_decode(full_path, def.display_name, def.display_name);
            } else {
                Overlay::get().push_achievement(def.display_name, def.description, {}, 0, 0);
            }
            return;
        }
    }

    if (with_sound) play_achievement_sound();
    Overlay::get().push_achievement(name, "", {}, 0, 0);
}

bool StarSteamUserStats::ClearAchievement(const char* pchName)
{
    if (!pchName) return false;
    nlohmann::json ach_json = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = achievements_.find(pchName);
        if (it != achievements_.end()) {
            if (it->second.achieved) Overlay::get().note_session_revoke();
            it->second.achieved = false;
            it->second.unlock_time = 0;
        }
        for (auto& [name, state] : achievements_) {
            ach_json[name] = {
                {"achieved", state.achieved},
                {"unlock_time", state.unlock_time}
            };
        }
        // Save under the lock: a concurrent SetAchievement must not be able to
        // persist after this stale snapshot and overwrite the unlock on disk.
        Storage::get().save_achievements(ach_json);
    }
    return true;
}

bool StarSteamUserStats::GetAchievementAndUnlockTime(const char* pchName, bool* pbAchieved, uint32* punUnlockTime)
{
    if (!pchName) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = achievements_.find(pchName);
    if (it != achievements_.end()) {
        if (pbAchieved) *pbAchieved = it->second.achieved;
        if (punUnlockTime) *punUnlockTime = it->second.unlock_time;
    } else {
        if (pbAchieved) *pbAchieved = false;
        if (punUnlockTime) *punUnlockTime = 0;
    }
    return true;
}

bool StarSteamUserStats::StoreStats()
{

    {
        nlohmann::json ach_json = nlohmann::json::object();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : achievements_) {
            ach_json[name] = {
                {"achieved", state.achieved},
                {"unlock_time", state.unlock_time}
            };
        }
        Storage::get().save_achievements(ach_json);

        nlohmann::json stats_json = nlohmann::json::object();
        for (auto& [name, sv] : stats_) {
            if (sv.type == StatValue::FLOAT_TYPE) {
                stats_json[name] = {{"type", "float"}, {"value", sv.float_val}};
            } else {
                stats_json[name] = {{"type", "int"}, {"value", sv.int_val}};
            }
        }
        Storage::get().save_stats(stats_json);
    }

    UserStatsStored_t cb{};
    cb.m_nGameID = Settings::get().app_id;
    cb.m_eResult = k_EResultOK;
    STAR_PostCallback(UserStatsStored_t::k_iCallback, &cb, sizeof(cb));
    STAR_LOG("StoreStats: saved");
    return true;
}

int StarSteamUserStats::GetAchievementIcon(const char* pchName)
{
    if (!pchName) return 0;
    auto& defs = Settings::get().achievements;
    for (auto& def : defs) {
        if (def.name == pchName) {
            if (!def.icon_path.empty()) {
                std::string full = Settings::get().settings_dir + "\\" + def.icon_path;
                for (char& c : full) if (c == '/') c = '\\';
                return StarSteamUtils::get().LoadImageFromFile(full);
            }
        }
    }
    return 0;
}

const char* StarSteamUserStats::GetAchievementDisplayAttribute(const char* pchName, const char* pchKey)
{
    if (!pchName || !pchKey) return "";
    auto& defs = Settings::get().achievements;
    for (auto& def : defs) {
        if (def.name == pchName) {
            std::string k(pchKey);
            if (k == "name") return def.display_name.c_str();
            if (k == "desc") return def.description.c_str();
            if (k == "hidden") return def.hidden ? "1" : "0";
        }
    }
    return "";
}

bool StarSteamUserStats::IndicateAchievementProgress(const char* pchName, uint32 nCurProgress, uint32 nMaxProgress)
{
    if (!pchName) return false;
    UserAchievementStored_t cb{};
    cb.m_nGameID = Settings::get().app_id;
    cb.m_bGroupAchievement = false;
    strncpy_s(cb.m_rgchAchievementName, pchName, _TRUNCATE);
    cb.m_nCurProgress = nCurProgress;
    cb.m_nMaxProgress = nMaxProgress;
    STAR_DispatchCallback(UserAchievementStored_t::k_iCallback, &cb, sizeof(cb));
    return true;
}

uint32 StarSteamUserStats::GetNumAchievements()
{
    return (uint32)Settings::get().achievements.size();
}

const char* StarSteamUserStats::GetAchievementName(uint32 iAchievement)
{
    auto& defs = Settings::get().achievements;
    if (iAchievement >= defs.size()) return "";
    return defs[iAchievement].name.c_str();
}

SteamAPICall_t StarSteamUserStats::RequestUserStats(CSteamID steamIDUser)
{
    UserStatsReceived_t result{};
    result.m_nGameID = Settings::get().app_id;
    result.m_eResult = k_EResultOK;
    result.m_steamIDUser = steamIDUser;
    return STAR_PostCallResult(UserStatsReceived_t::k_iCallback, &result, sizeof(result));
}

bool StarSteamUserStats::GetUserStat(CSteamID steamIDUser, const char* pchName, int32* pData)
{
    if (steamIDUser == CSteamID(Settings::get().steam_id)) {
        return GetStat(pchName, pData);
    }
    if (pData) *pData = 0;
    return false;
}

bool StarSteamUserStats::GetUserStat(CSteamID steamIDUser, const char* pchName, float* pData)
{
    if (steamIDUser == CSteamID(Settings::get().steam_id)) {
        return GetStat(pchName, pData);
    }
    if (pData) *pData = 0.0f;
    return false;
}

bool StarSteamUserStats::GetUserAchievement(CSteamID steamIDUser, const char* pchName, bool* pbAchieved)
{
    if (steamIDUser == CSteamID(Settings::get().steam_id)) {
        return GetAchievement(pchName, pbAchieved);
    }
    if (pbAchieved) *pbAchieved = false;
    return false;
}

bool StarSteamUserStats::GetUserAchievementAndUnlockTime(CSteamID steamIDUser, const char* pchName, bool* pbAchieved, uint32* punUnlockTime)
{
    if (steamIDUser == CSteamID(Settings::get().steam_id)) {
        return GetAchievementAndUnlockTime(pchName, pbAchieved, punUnlockTime);
    }
    if (pbAchieved) *pbAchieved = false;
    if (punUnlockTime) *punUnlockTime = 0;
    return false;
}

bool StarSteamUserStats::ResetAllStats(bool bAchievementsToo)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.clear();
    if (bAchievementsToo) achievements_.clear();

    Storage::get().save_stats(nlohmann::json::object());
    if (bAchievementsToo) Storage::get().save_achievements(nlohmann::json::object());
    return true;
}

LeaderboardInfo* StarSteamUserStats::find_leaderboard(SteamLeaderboard_t handle)
{
    if (handle == 0 || handle > leaderboards_.size()) return nullptr;
    return &leaderboards_[(size_t)(handle - 1)];
}

SteamLeaderboard_t StarSteamUserStats::get_or_create_leaderboard(const std::string& name, ELeaderboardSortMethod sort, ELeaderboardDisplayType display)
{
    for (size_t i = 0; i < leaderboards_.size(); ++i) {
        if (leaderboards_[i].name == name) return (SteamLeaderboard_t)(i + 1);
    }
    LeaderboardInfo info;
    info.name = name;
    info.sort_method = sort;
    info.display_type = display;
    info.entry_count = 0;
    leaderboards_.push_back(std::move(info));
    return (SteamLeaderboard_t)leaderboards_.size();
}

SteamAPICall_t StarSteamUserStats::FindOrCreateLeaderboard(const char* pchLeaderboardName, ELeaderboardSortMethod eLeaderboardSortMethod, ELeaderboardDisplayType eLeaderboardDisplayType)
{
    if (!pchLeaderboardName) return k_uAPICallInvalid;
    SteamLeaderboard_t handle = get_or_create_leaderboard(pchLeaderboardName, eLeaderboardSortMethod, eLeaderboardDisplayType);
    LeaderboardFindResult_t result{};
    result.m_hSteamLeaderboard = handle;
    result.m_bLeaderboardFound = 1;
    return STAR_PostCallResult(LeaderboardFindResult_t::k_iCallback, &result, sizeof(result));
}

SteamAPICall_t StarSteamUserStats::FindLeaderboard(const char* pchLeaderboardName)
{
    if (!pchLeaderboardName) return k_uAPICallInvalid;
    SteamLeaderboard_t handle = get_or_create_leaderboard(pchLeaderboardName, k_ELeaderboardSortMethodDescending, k_ELeaderboardDisplayTypeNumeric);
    LeaderboardFindResult_t result{};
    result.m_hSteamLeaderboard = handle;
    result.m_bLeaderboardFound = 1;
    return STAR_PostCallResult(LeaderboardFindResult_t::k_iCallback, &result, sizeof(result));
}

const char* StarSteamUserStats::GetLeaderboardName(SteamLeaderboard_t hSteamLeaderboard)
{
    auto* lb = find_leaderboard(hSteamLeaderboard);
    return lb ? lb->name.c_str() : "";
}

int StarSteamUserStats::GetLeaderboardEntryCount(SteamLeaderboard_t hSteamLeaderboard)
{
    auto* lb = find_leaderboard(hSteamLeaderboard);
    return lb ? lb->entry_count : 0;
}

ELeaderboardSortMethod StarSteamUserStats::GetLeaderboardSortMethod(SteamLeaderboard_t hSteamLeaderboard)
{
    auto* lb = find_leaderboard(hSteamLeaderboard);
    return lb ? lb->sort_method : k_ELeaderboardSortMethodNone;
}

ELeaderboardDisplayType StarSteamUserStats::GetLeaderboardDisplayType(SteamLeaderboard_t hSteamLeaderboard)
{
    auto* lb = find_leaderboard(hSteamLeaderboard);
    return lb ? lb->display_type : k_ELeaderboardDisplayTypeNone;
}

SteamAPICall_t StarSteamUserStats::DownloadLeaderboardEntries(SteamLeaderboard_t hSteamLeaderboard, ELeaderboardDataRequest eLeaderboardDataRequest, int nRangeStart, int nRangeEnd)
{
    STAR_UNREFERENCED(nRangeStart); STAR_UNREFERENCED(nRangeEnd); STAR_UNREFERENCED(eLeaderboardDataRequest);
    LeaderboardScoresDownloaded_t result{};
    result.m_hSteamLeaderboard = hSteamLeaderboard;
    result.m_hSteamLeaderboardEntries = 0;
    result.m_cEntryCount = 0;
    return STAR_PostCallResult(LeaderboardScoresDownloaded_t::k_iCallback, &result, sizeof(result));
}

SteamAPICall_t StarSteamUserStats::DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t hSteamLeaderboard, CSteamID* prgUsers, int cUsers)
{
    STAR_UNREFERENCED(prgUsers); STAR_UNREFERENCED(cUsers);
    LeaderboardScoresDownloaded_t result{};
    result.m_hSteamLeaderboard = hSteamLeaderboard;
    result.m_hSteamLeaderboardEntries = 0;
    result.m_cEntryCount = 0;
    return STAR_PostCallResult(LeaderboardScoresDownloaded_t::k_iCallback, &result, sizeof(result));
}

bool StarSteamUserStats::GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t hSteamLeaderboardEntries, int index, LeaderboardEntry_t* pLeaderboardEntry, int32* pDetails, int cDetailsMax)
{
    STAR_UNREFERENCED(hSteamLeaderboardEntries); STAR_UNREFERENCED(index);
    STAR_UNREFERENCED(pDetails); STAR_UNREFERENCED(cDetailsMax);
    if (pLeaderboardEntry) memset(pLeaderboardEntry, 0, sizeof(*pLeaderboardEntry));
    return false;
}

SteamAPICall_t StarSteamUserStats::UploadLeaderboardScore(SteamLeaderboard_t hSteamLeaderboard, ELeaderboardUploadScoreMethod eLeaderboardUploadScoreMethod, int32 nScore, const int32* pScoreDetails, int cScoreDetailsCount)
{
    STAR_UNREFERENCED(eLeaderboardUploadScoreMethod); STAR_UNREFERENCED(pScoreDetails); STAR_UNREFERENCED(cScoreDetailsCount);
    LeaderboardScoreUploaded_t result{};
    result.m_bSuccess = 1;
    result.m_hSteamLeaderboard = hSteamLeaderboard;
    result.m_nScore = nScore;
    result.m_bScoreChanged = 1;
    result.m_nGlobalRankNew = 1;
    result.m_nGlobalRankPrevious = 1;
    return STAR_PostCallResult(LeaderboardScoreUploaded_t::k_iCallback, &result, sizeof(result));
}

SteamAPICall_t StarSteamUserStats::AttachLeaderboardUGC(SteamLeaderboard_t hSteamLeaderboard, UGCHandle_t hUGC)
{
    STAR_UNREFERENCED(hUGC);
    LeaderboardUGCSet_t result{};
    result.m_eResult = k_EResultOK;
    result.m_hSteamLeaderboard = hSteamLeaderboard;
    return STAR_PostCallResult(LeaderboardUGCSet_t::k_iCallback, &result, sizeof(result));
}

SteamAPICall_t StarSteamUserStats::GetNumberOfCurrentPlayers()
{
    NumberOfCurrentPlayers_t result{};
    result.m_bSuccess = 1;
    result.m_cPlayers = 1;
    return STAR_PostCallResult(NumberOfCurrentPlayers_t::k_iCallback, &result, sizeof(result));
}

SteamAPICall_t StarSteamUserStats::RequestGlobalAchievementPercentages()
{
    GlobalAchievementPercentagesReady_t result{};
    result.m_nGameID = Settings::get().app_id;
    result.m_eResult = k_EResultOK;
    return STAR_PostCallResult(GlobalAchievementPercentagesReady_t::k_iCallback, &result, sizeof(result));
}

int StarSteamUserStats::GetMostAchievedAchievementInfo(char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved)
{
    STAR_UNREFERENCED(pchName); STAR_UNREFERENCED(unNameBufLen);
    STAR_UNREFERENCED(pflPercent); STAR_UNREFERENCED(pbAchieved);
    return -1;
}

int StarSteamUserStats::GetNextMostAchievedAchievementInfo(int iIteratorPrevious, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved)
{
    STAR_UNREFERENCED(iIteratorPrevious); STAR_UNREFERENCED(pchName); STAR_UNREFERENCED(unNameBufLen);
    STAR_UNREFERENCED(pflPercent); STAR_UNREFERENCED(pbAchieved);
    return -1;
}

bool StarSteamUserStats::GetAchievementAchievedPercent(const char* pchName, float* pflPercent)
{
    STAR_UNREFERENCED(pchName);
    if (pflPercent) *pflPercent = 0.0f;
    return false;
}

SteamAPICall_t StarSteamUserStats::RequestGlobalStats(int nHistoryDays)
{
    STAR_UNREFERENCED(nHistoryDays);
    GlobalStatsReceived_t result{};
    result.m_nGameID = Settings::get().app_id;
    result.m_eResult = k_EResultOK;
    return STAR_PostCallResult(GlobalStatsReceived_t::k_iCallback, &result, sizeof(result));
}

bool StarSteamUserStats::GetGlobalStat(const char* pchStatName, int64* pData)
{
    STAR_UNREFERENCED(pchStatName);
    if (pData) *pData = 0;
    return false;
}

bool StarSteamUserStats::GetGlobalStat(const char* pchStatName, double* pData)
{
    STAR_UNREFERENCED(pchStatName);
    if (pData) *pData = 0.0;
    return false;
}

int32 StarSteamUserStats::GetGlobalStatHistory(const char* pchStatName, int64* pData, uint32 cubData)
{
    STAR_UNREFERENCED(pchStatName); STAR_UNREFERENCED(pData); STAR_UNREFERENCED(cubData);
    return 0;
}

int32 StarSteamUserStats::GetGlobalStatHistory(const char* pchStatName, double* pData, uint32 cubData)
{
    STAR_UNREFERENCED(pchStatName); STAR_UNREFERENCED(pData); STAR_UNREFERENCED(cubData);
    return 0;
}

uint32 StarSteamUserStats::GetNumStats(CGameID nGameID)
{
    STAR_UNREFERENCED(nGameID);
    std::lock_guard<std::mutex> lock(mutex_);
    return (uint32)stats_.size();
}

const char* StarSteamUserStats::GetStatName(CGameID nGameID, uint32 iStat)
{
    STAR_UNREFERENCED(nGameID);
    std::lock_guard<std::mutex> lock(mutex_);
    if (iStat >= stats_.size()) return "";
    auto it = stats_.begin();
    std::advance(it, iStat);
    return it->first.c_str();
}

ESteamUserStatType StarSteamUserStats::GetStatType(CGameID nGameID, const char* pchName)
{
    STAR_UNREFERENCED(nGameID);
    if (!pchName) return k_ESteamUserStatTypeINT;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(pchName);
    if (it != stats_.end() && it->second.type == StatValue::FLOAT_TYPE)
        return k_ESteamUserStatTypeFLOAT;
    return k_ESteamUserStatTypeINT;
}

uint32 StarSteamUserStats::GetNumAchievements(CGameID nGameID) { STAR_UNREFERENCED(nGameID); return GetNumAchievements(); }
const char* StarSteamUserStats::GetAchievementName(CGameID nGameID, uint32 i) { STAR_UNREFERENCED(nGameID); return GetAchievementName(i); }
bool StarSteamUserStats::RequestCurrentStats(CGameID nGameID) { STAR_UNREFERENCED(nGameID); return RequestCurrentStats(); }
bool StarSteamUserStats::GetStat(CGameID nGameID, const char* n, int32* p) { STAR_UNREFERENCED(nGameID); return GetStat(n, p); }
bool StarSteamUserStats::GetStat(CGameID nGameID, const char* n, float* p) { STAR_UNREFERENCED(nGameID); return GetStat(n, p); }
bool StarSteamUserStats::SetStat(CGameID nGameID, const char* n, int32 d) { STAR_UNREFERENCED(nGameID); return SetStat(n, d); }
bool StarSteamUserStats::SetStat(CGameID nGameID, const char* n, float d) { STAR_UNREFERENCED(nGameID); return SetStat(n, d); }
bool StarSteamUserStats::UpdateAvgRateStat(CGameID nGameID, const char* n, float c, double s) { STAR_UNREFERENCED(nGameID); return UpdateAvgRateStat(n, c, s); }
bool StarSteamUserStats::GetAchievement(CGameID nGameID, const char* n, bool* a) { STAR_UNREFERENCED(nGameID); return GetAchievement(n, a); }
bool StarSteamUserStats::SetAchievement(CGameID nGameID, const char* n) { STAR_UNREFERENCED(nGameID); return SetAchievement(n); }
bool StarSteamUserStats::StoreStats(CGameID nGameID) { STAR_UNREFERENCED(nGameID); return StoreStats(); }
bool StarSteamUserStats::ClearAchievement(CGameID nGameID, const char* n) { STAR_UNREFERENCED(nGameID); return ClearAchievement(n); }
int StarSteamUserStats::GetAchievementIcon(CGameID nGameID, const char* n) { STAR_UNREFERENCED(nGameID); return GetAchievementIcon(n); }
const char* StarSteamUserStats::GetAchievementDisplayAttribute(CGameID nGameID, const char* n, const char* k) { STAR_UNREFERENCED(nGameID); return GetAchievementDisplayAttribute(n, k); }
bool StarSteamUserStats::IndicateAchievementProgress(CGameID nGameID, const char* n, uint32 c, uint32 m) { STAR_UNREFERENCED(nGameID); return IndicateAchievementProgress(n, c, m); }

uint32 StarSteamUserStats::GetNumGroupAchievements(CGameID nGameID) { STAR_UNREFERENCED(nGameID); return 0; }
const char* StarSteamUserStats::GetGroupAchievementName(CGameID nGameID, uint32 i) { STAR_UNREFERENCED(nGameID); STAR_UNREFERENCED(i); return ""; }
bool StarSteamUserStats::GetGroupAchievement(CGameID nGameID, const char* n, bool* a) { STAR_UNREFERENCED(nGameID); return GetAchievement(n, a); }
bool StarSteamUserStats::SetGroupAchievement(CGameID nGameID, const char* n) { STAR_UNREFERENCED(nGameID); return SetAchievement(n); }
bool StarSteamUserStats::ClearGroupAchievement(CGameID nGameID, const char* n) { STAR_UNREFERENCED(nGameID); return ClearAchievement(n); }

SteamAPICall_t StarSteamUserStats::UploadLeaderboardScore(SteamLeaderboard_t hSteamLeaderboard, int32 nScore, int32* pScoreDetails, int cScoreDetailsCount)
{
    return UploadLeaderboardScore(hSteamLeaderboard, k_ELeaderboardUploadScoreMethodKeepBest, nScore, pScoreDetails, cScoreDetailsCount);
}

bool StarSteamUserStats::GetAchievementProgressLimits(const char* pchName, int32* pnMinProgress, int32* pnMaxProgress)
{
    STAR_UNREFERENCED(pchName);
    if (pnMinProgress) *pnMinProgress = 0;
    if (pnMaxProgress) *pnMaxProgress = 0;
    return false;
}

bool StarSteamUserStats::GetAchievementProgressLimits(const char* pchName, float* pfMinProgress, float* pfMaxProgress)
{
    STAR_UNREFERENCED(pchName);
    if (pfMinProgress) *pfMinProgress = 0.0f;
    if (pfMaxProgress) *pfMaxProgress = 0.0f;
    return false;
}
