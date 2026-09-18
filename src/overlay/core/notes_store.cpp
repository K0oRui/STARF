#include "overlay/core/notes_store.h"
#include "core/settings.h"
#include "core/star_common.h"
#include <fstream>
#include <iterator>

void NotesStore::load()
{
    text_.clear();
    loaded_ = true;
    dirty_ = false;
    std::string path = Settings::get().settings_dir + "\\notes.txt";
    std::ifstream in(utf8_to_wstring(path), std::ios::binary);
    if (!in.is_open()) return;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 65536) data.resize(65536);
    // Strip UTF-8 BOM if present.
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    text_ = data;
}

void NotesStore::save()
{
    if (!dirty_) return;
    dirty_ = false;
    last_edit_ = GetTickCount();
    std::string path = Settings::get().settings_dir + "\\notes.txt";
    std::ofstream out(utf8_to_wstring(path), std::ios::binary | std::ios::trunc);
    if (out.is_open()) out << text_;
}
