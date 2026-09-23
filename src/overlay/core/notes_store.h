#pragma once
#include <windows.h>
#include <string>

// Owns the per-game notes buffer and its persistence state. Notes live in
// STAR/notes.txt next to the game copy; edits autosave after a short idle.
class NotesStore {
public:
    static constexpr size_t kMaxBytes = 65536; // ponytail: hard cap on note size; raise if anyone needs more
    void load();
    void save();

    const std::string& text() const { return text_; }
    void set_text(const std::string& value) {
        text_ = value;
        dirty_ = true;
        last_edit_ = GetTickCount();
    }

    bool dirty() const { return dirty_; }
    bool autosave_due(DWORD now) const {
        return dirty_ && (now - last_edit_) > 2000;
    }

private:
    std::string text_;
    bool dirty_ = false;
    DWORD last_edit_ = 0;
};
