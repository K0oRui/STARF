#include "overlay/overlay_internal.h"
#include "overlay/capture/screenshot_service.h"

void StarOverlay::request_screenshot()
{
    if (!enabled_) return;
    screenshots_.request();
    STAR_LOG("Screenshot requested");
}

void StarOverlay::notify_screenshot(const std::string& file, bool dark)
{
    std::string name = file_name(file);
    // Thumbnail preview so the toast isn't a grey box. Decode off-thread so
    // a big PNG never stalls the render loop.
    AchievementNotification toast;
    toast.title = name;
    std::string key = "shot_" + name;
    toast.icon_key = key;
    toast.header = "SCREENSHOT SAVED";
    toast.description = dark ? "All black? Try Borderless mode." : "Saved to " + ScreenshotService::dir();
    notifications_.push(std::move(toast));
    enqueue_icon_decode(file, key);
    // Pop the viewer so the user gets an instant preview on next panel open.
    viewer_file_ = file;
    STAR_LOG("Screenshot saved: %s", file.c_str());
}
