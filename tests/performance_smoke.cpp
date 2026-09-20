#include "core/work_queue.h"
#include "overlay/ui/notification_queue.h"
#include "overlay/ui/draw_snapshot.h"
#include <cassert>
#include <future>
#include <cstdio>

int main() {
    NotificationQueue notifications;
    AchievementNotification toast;
    toast.title = "screenshot";
    toast.icon_rgba = std::make_shared<const std::vector<uint8_t>>(3840u * 2160u * 4u, 127);
    auto pixels = toast.icon_rgba;
    notifications.push(std::move(toast));
    auto first = notifications.advance_and_snapshot(0);
    auto second = notifications.advance_and_snapshot(0);
    assert(first[0].icon_rgba == pixels && second[0].icon_rgba == pixels);
    notifications.attach_icon("screenshot", {1, 2, 3, 4}, 1, 1);
    assert((*first[0].icon_rgba)[0] == 127); // Existing snapshots stay immutable.
    assert((*notifications.advance_and_snapshot(0)[0].icon_rgba)[0] == 1);
    assert(notifications.advance_and_snapshot(6).empty());
    assert((*first[0].icon_rgba)[0] == 127); // Lifetime extends beyond expiry.

    WorkQueue worker;
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    assert(worker.submit([&] { started.set_value(); gate.wait(); }, 128u * 1024u * 1024u));
    started.get_future().wait();
    assert(!worker.submit([] {}, 1)); // Running work counts against the memory bound.
    std::vector<int> order;
    for (int i = 0; i < 15; ++i) assert(worker.submit([&, i] { order.push_back(i); }));
    assert(!worker.submit([] {}));
    release.set_value();
    worker.stop(); // Drains all accepted work before returning.
    assert(order.size() == 15);
    for (int i = 0; i < 15; ++i) assert(order[i] == i);
    assert(!worker.submit([] {}));
    worker.start();
    assert(worker.submit([&] { order.push_back(15); }));
    worker.stop();
    assert(order.back() == 15);

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    unsigned char* font_pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &width, &height);
    DrawSnapshot snapshot;
    auto frame = [&](ImU32 color) {
        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddRectFilled({10, 10}, {100, 40}, color);
        ImGui::Render();
        return snapshot.changed(ImGui::GetDrawData());
    };
    assert(frame(IM_COL32_WHITE));
    assert(!frame(IM_COL32_WHITE));
    assert(frame(IM_COL32(255, 0, 0, 255)));
    snapshot.clear();
    assert(frame(IM_COL32(255, 0, 0, 255)));
    ImGui::DestroyContext();
    std::puts("performance smoke passed");
}
