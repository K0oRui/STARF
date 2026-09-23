#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

struct AchievementNotification {
    std::string header = "ACHIEVEMENT UNLOCKED";
    bool summary = false; // all-complete celebration: gold styling + star
    std::string title;
    std::string icon_key;
    std::string description;
    std::shared_ptr<const std::vector<uint8_t>> icon_rgba;
    int   icon_width  = 0;
    int   icon_height = 0;
    float time_remaining = 5.0f;
    float age = 0.0f;
};

// Thread-safe toast queue: producers (achievement unlock callbacks, screenshot
// notifications) push from arbitrary threads; the render thread advances
// timers and takes snapshots.
class NotificationQueue {
public:
    void push(AchievementNotification n) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(n));
    }

    // Advance timers, drop expired toasts, and return a render snapshot.
    std::vector<AchievementNotification> advance_and_snapshot(float dt) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& n : items_) { n.time_remaining -= dt; n.age += dt; }
        items_.erase(std::remove_if(items_.begin(), items_.end(),
            [](const AchievementNotification& n){ return n.time_remaining <= 0.f; }),
            items_.end());
        return items_;
    }

    // Non-blocking emptiness probe. Returns true if the lock is contended so
    // the caller conservatively renders rather than skipping a toast.
    bool has_pending() const {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return true;
        return !items_.empty();
    }

    // Attach decoded icon pixels to the first toast with the given title.
    // Used by the icon-decode worker drain: the toast renders immediately
    // with a placeholder and swaps in the real icon once decoded.
    void attach_icon(const std::string& title, std::vector<uint8_t> rgba,
                     int w, int h) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& n : items_) {
            if (n.title == title) {
                n.icon_rgba = std::make_shared<const std::vector<uint8_t>>(std::move(rgba));
                n.icon_width = w;
                n.icon_height = h;
                break;
            }
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<AchievementNotification> items_;
};
