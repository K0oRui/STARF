#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/storage.h"
#include "core/settings.h"
#include "core/callbacks.h"
#include "steam/steam_remote_storage.h"
#include "steam/steam_utils.h"
#include "steam/steam_user_stats.h"
#include "overlay/overlay.h"
#include "overlay/capture/screenshot_service.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

void STAR_WriteLog(const char*, ...) {}
Settings& Settings::get() { static Settings settings; return settings; }

struct TestOverlay : Overlay {
    int unlocks = 0, revokes = 0;
    void init() override {}
    void shutdown() override {}
    void push_achievement(const std::string&, const std::string&, const std::vector<uint8_t>&, int, int, const std::string&, bool) override {}
    void request_screenshot() override {}
    void enqueue_icon_decode(const std::string&, const std::string&, const std::string&) override {}
    void note_session_unlock() override { ++unlocks; }
    void note_session_revoke() override { ++revokes; }
    bool is_enabled() const override { return false; }
    bool is_open() const override { return false; }
    void open_panel() override {}
};
Overlay& Overlay::get() { static TestOverlay overlay; return overlay; }

template<class Result> Result result_for(SteamAPICall_t handle) {
    assert(handle != k_uAPICallInvalid);
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    auto it = g_pending_results.find(handle);
    assert(it != g_pending_results.end());
    assert(it->second.callback_id == Result::k_iCallback);
    assert(it->second.data.size() == sizeof(Result));
    Result result;
    memcpy(&result, it->second.data.data(), sizeof(result));
    g_pending_results.erase(it);
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 2); // Caller supplies an isolated directory under build/.
    auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    SetEnvironmentVariableW(L"APPDATA", root.c_str());
    Storage::get().init(123, 456);
    auto& remote = StarSteamRemoteStorage::get();
    char data[] = "abcdefghij";
    auto write = remote.FileWriteAsync("save.bin", data, 10);
    memset(data, 'X', 10); // Worker must own the original bytes.
    auto read = remote.FileReadAsync("save.bin", 3, 4); // Ordered after the write.
    auto remainder = remote.FileReadAsync("save.bin", 5, 0);
    auto eof = remote.FileReadAsync("save.bin", 10, 5);
    auto invalid = remote.FileReadAsync("save.bin", 11, 1);
    auto missing = remote.FileReadAsync("missing.bin", 0, 1);
    remote.stop_async();
    assert(result_for<RemoteStorageFileWriteAsyncComplete_t>(write).m_eResult == k_EResultOK);
    auto info = result_for<RemoteStorageFileReadAsyncComplete_t>(read);
    assert(info.m_eResult == k_EResultOK && info.m_cubRead == 4 && info.m_nOffset == 3);
    char output[5]{};
    assert(remote.FileReadAsyncComplete(read, output, 4));
    assert(strcmp(output, "defg") == 0);
    assert(!remote.FileReadAsyncComplete(read, output, 4));
    assert(result_for<RemoteStorageFileReadAsyncComplete_t>(remainder).m_cubRead == 5);
    assert(result_for<RemoteStorageFileReadAsyncComplete_t>(eof).m_cubRead == 0);
    assert(result_for<RemoteStorageFileReadAsyncComplete_t>(invalid).m_eResult == k_EResultFail);
    assert(result_for<RemoteStorageFileReadAsyncComplete_t>(missing).m_eResult == k_EResultFail);
    assert(remote.FileRead("save.bin", output, 2) == 2 && output[0] == 'a' && output[1] == 'b');
    assert(remote.FileRead("save.bin", output, -1) == 0);
    assert(remote.FileWriteAsync("bad", nullptr, 1) == k_uAPICallInvalid);
    remote.start_async();
    auto again = remote.FileReadAsync("save.bin", 9, 20);
    remote.stop_async();
    assert(result_for<RemoteStorageFileReadAsyncComplete_t>(again).m_cubRead == 1);

    ScreenshotService screenshots;
    auto image = (root / "async.png").string();
    assert(screenshots.save_async(image, {255, 0, 0, 0, 0, 255, 0, 0}, 2, 1, true));
    screenshots.stop();
    auto completed = screenshots.take_completed();
    assert(completed.size() == 1 && completed[0].path == image && completed[0].dark);
    assert(!screenshots.busy());
    std::ifstream png(image, std::ios::binary);
    unsigned char signature[8]{};
    png.read((char*)signature, 8);
    const unsigned char expected[] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(memcmp(signature, expected, 8) == 0);
    auto& utils = StarSteamUtils::get();
    std::vector<uint8_t> decoded;
    int w = 0, h = 0;
    assert(utils.LoadIconFile(image, decoded, w, h));
    assert(w == 2 && h == 1 && decoded == std::vector<uint8_t>({255, 0, 0, 255, 0, 255, 0, 255}));
    uint32 image_width = 0, image_height = 0;
    assert(!utils.GetImageSize(1, &image_width, &image_height)); // Overlay decode creates no Steam handle.
    auto handle = utils.LoadImageFromFile(image);
    assert(handle > 0 && utils.LoadImageFromFile(image) == handle);
    assert(utils.LoadIconFile(image, decoded, w, h, 1) && w == 1 && h == 1);
    assert(!utils.GetImageSize(handle + 1, &image_width, &image_height));
    screenshots.start();
    assert(screenshots.save_async((root / "missing" / "fail.png").string(), {1, 2, 3, 4}, 1, 1));
    screenshots.stop();
    assert(screenshots.take_completed().empty() && !screenshots.busy());
    auto& settings = Settings::get();
    for (int i = 0; i < 1000; ++i) {
        AchievementDef def;
        def.name = "achievement_" + std::to_string(i);
        settings.achievements.push_back(std::move(def));
    }
    auto& stats = StarSteamUserStats::get();
    stats.set_all_achievements(false);
    stats.set_all_achievements(true);
    auto states = stats.achievement_snapshot();
    assert(states.size() == 1000);
    for (const auto& def : settings.achievements) {
        bool achieved = false;
        assert(stats.GetAchievement(def.name.c_str(), &achieved) && achieved);
        assert(states.at(def.name).unlock_time > 0);
    }
    auto& overlay = static_cast<TestOverlay&>(Overlay::get());
    assert(overlay.unlocks == 1000);
    stats.set_all_achievements(true);
    assert(overlay.unlocks == 1000); // Repeating the bulk action creates no duplicate unlocks.
    stats.set_all_achievements(false);
    bool achieved = true;
    assert(stats.GetAchievement("achievement_999", &achieved) && !achieved);
    assert(!stats.GetAchievement("unknown", &achieved));
    nlohmann::json persisted;
    assert(Storage::get().load_achievements(persisted));
    assert(persisted.size() == 1000 && persisted["achievement_999"]["achieved"] == false);
    std::puts("async I/O and achievement smoke passed");
}
