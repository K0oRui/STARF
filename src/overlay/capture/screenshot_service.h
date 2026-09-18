#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Owns the screenshot request flag and the filesystem/pixel helpers used by
// every capture backend. Backends call consume() once per present and encode
// through the shared PNG writer.
class ScreenshotService {
public:
    void request() { requested_ = true; }
    bool consume() { return requested_.exchange(false); }
    bool pending() const { return requested_.load(); }

    static std::string dir();
    static std::string next_path();
    static bool save_rgba_png(const std::string& path, const uint8_t* rgba, int w, int h);

private:
    std::atomic<bool> requested_{ false };
};
