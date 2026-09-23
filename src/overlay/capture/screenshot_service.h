#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "core/work_queue.h"

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

    struct Completed { std::string path; bool dark; };
    void start() { worker_.start(); }
    void stop() { worker_.stop(); }
    bool save_async(const std::string& path, std::vector<uint8_t> rgba, int w, int h, bool dark = false);
    std::vector<Completed> take_completed();
    bool busy() const {
        if (outstanding_.load() != 0) return true;
        std::lock_guard<std::mutex> lock(completed_mutex_);
        return !completed_.empty();
    }
private:
    WorkQueue worker_;
    std::atomic<unsigned> outstanding_{0};
    mutable std::mutex completed_mutex_;
    std::vector<Completed> completed_;
    std::atomic<bool> requested_{ false };
};
