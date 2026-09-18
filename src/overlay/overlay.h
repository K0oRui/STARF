#pragma once
// Public overlay facade. The full StarOverlay implementation — backend
// devices, capture state, UI internals — lives in overlay_internal.h and is
// only visible to the overlay's own translation units. External callers (the
// steam_* modules and DllMain path) use this interface alone.
#include <cstdint>
#include <string>
#include <vector>

class Overlay {
public:
    static Overlay& get();

    virtual ~Overlay() = default;

    virtual void init() = 0;
    virtual void shutdown() = 0;

    virtual void push_achievement(const std::string& name, const std::string& desc,
                                  const std::vector<uint8_t>& icon_rgba, int iw, int ih,
                                  const std::string& header = "ACHIEVEMENT UNLOCKED",
                                  bool summary = false) = 0;

    virtual void request_screenshot() = 0;

    virtual void enqueue_icon_decode(const std::string& path, const std::string& key,
                                     const std::string& toast_title = "") = 0;

    virtual void note_session_unlock() = 0;
    virtual void note_session_revoke() = 0;

    virtual bool is_enabled() const = 0;
    virtual bool is_open() const = 0;
    virtual void open_panel() = 0;
};