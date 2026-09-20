#include "core/storage.h"

Storage& Storage::get()
{
    static Storage instance;
    return instance;
}

void Storage::init(uint32_t app_id, uint64_t steam_id)
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::string roaming = (len > 0) ? wstring_to_utf8(appdata) : "C:\\Users\\Default\\AppData\\Roaming";

    base_path_ = roaming + "\\STAR\\" + std::to_string(app_id) + "\\" + std::to_string(steam_id);
    remote_dir_ = base_path_ + "\\remote";

    ensure_dir(base_path_);
    ensure_dir(remote_dir_);

    STAR_LOG("Storage initialized at: %s", base_path_.c_str());
}

static bool create_dir_recursive(const std::string& path)
{
    if (path.empty()) return false;
    
    std::wstring wpath = utf8_to_wstring(path);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    size_t last_slash = path.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        std::string parent = path.substr(0, last_slash);
        if (!parent.empty() && parent.back() != ':') {
            if (!create_dir_recursive(parent)) return false;
        }
    }

    return CreateDirectoryW(wpath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool Storage::ensure_dir(const std::string& path)
{
    return create_dir_recursive(path);
}

bool Storage::write_json(const std::string& path, const nlohmann::json& data)
{
    std::ofstream f(utf8_to_wstring(path));
    if (!f.is_open()) return false;
    f << data.dump(2);
    return true;
}

bool Storage::read_json(const std::string& path, nlohmann::json& out)
{
    std::ifstream f(utf8_to_wstring(path));
    if (!f.is_open()) return false;
    try {
        f >> out;
        return true;
    } catch (...) {
        return false;
    }
}

bool Storage::load_achievements(nlohmann::json& out)
{
    return read_json(base_path_ + "\\achievements.json", out);
}

bool Storage::save_achievements(const nlohmann::json& data)
{
    return write_json(base_path_ + "\\achievements.json", data);
}

bool Storage::load_stats(nlohmann::json& out)
{
    return read_json(base_path_ + "\\stats.json", out);
}

bool Storage::save_stats(const nlohmann::json& data)
{
    return write_json(base_path_ + "\\stats.json", data);
}

bool Storage::load_playtime(uint64_t& total_seconds)
{
    total_seconds = 0;
    nlohmann::json j;
    if (!read_json(base_path_ + "\\playtime.json", j)) return false;
    try {
        total_seconds = j.value("total_seconds", 0ULL);
        return true;
    } catch (...) {
        return false;
    }
}

bool Storage::save_playtime(uint64_t total_seconds)
{
    nlohmann::json j = nlohmann::json::object();
    j["total_seconds"] = total_seconds;
    return write_json(base_path_ + "\\playtime.json", j);
}

std::string Storage::remote_path(const std::string& filename)
{
    // Cloud names may carry subdirectories ("characters/x.fch") - preserve
    // them like real Steam/Goldberg do. Anything that would escape the
    // remote dir (drive letters, UNC, "..") is folded back inside.
    std::string rel = filename;
    for (char& c : rel) {
        if (c == '/') c = '\\';
    }
    std::vector<std::string> parts;
    std::string cur;
    auto push = [&]() {
        if (cur.empty() || cur == ".") { cur.clear(); return; }
        if (cur == "..") { if (!parts.empty()) parts.pop_back(); cur.clear(); return; }
        // Strip drive colon + illegal Windows name chars, keep the rest.
        std::string clean;
        for (char c : cur) {
            if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' ||
                (unsigned char)c < 0x20)
                clean += '_';
            else
                clean += c;
        }
        // A bare drive letter ("C") becomes a plain folder, keeping C: vs D: distinct.
        if (!clean.empty()) parts.push_back(clean);
        cur.clear();
    };
    for (char c : rel) {
        if (c == '\\') push();
        else cur += c;
    }
    push();
    std::string path = remote_dir_;
    for (auto& p : parts) path += "\\" + p;
    return path;
}

// Pre-subdirectory layout: every separator became '_'. Read-side fallback
// so files written by older builds (e.g. "_option.sav") stay reachable.
static std::string remote_path_legacy(const std::string& remote_dir, const std::string& filename)
{
    std::string safe = filename;
    for (char& c : safe) {
        if (c == '/' || c == '\\') c = '_';
    }
    return remote_dir + "\\" + safe;
}

static bool file_exists_w(const std::string& path)
{
    DWORD attr = GetFileAttributesW(utf8_to_wstring(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool Storage::write_remote_file(const std::string& filename, const void* data, size_t size)
{
    std::string path = remote_path(filename);
    size_t last_slash = path.find_last_of("\\/");
    if (last_slash != std::string::npos) ensure_dir(path.substr(0, last_slash));
    std::ofstream f(utf8_to_wstring(path), std::ios::binary);
    if (!f.is_open()) return false;
    if (data && size > 0) {
        f.write(reinterpret_cast<const char*>(data), size);
    }
    return f.good();
}

bool Storage::read_remote_file(const std::string& filename, std::vector<uint8_t>& out, size_t offset, size_t count)
{
    std::string path = remote_path(filename);
    if (!file_exists_w(path)) path = remote_path_legacy(remote_dir_, filename);
    std::ifstream f(utf8_to_wstring(path), std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    auto end = f.tellg();
    if (end < 0 || offset > (size_t)end) return false;
    size_t size = std::min(count, (size_t)end - offset);
    f.seekg((std::streamoff)offset);
    out.resize(size);
    if (size) f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good();
}

bool Storage::remote_file_exists(const std::string& filename)
{
    if (file_exists_w(remote_path(filename))) return true;
    return file_exists_w(remote_path_legacy(remote_dir_, filename));
}

bool Storage::delete_remote_file(const std::string& filename)
{
    bool ok = DeleteFileW(utf8_to_wstring(remote_path(filename)).c_str()) != 0;
    // Also clear a legacy flattened twin so it can't ghost in listings.
    DeleteFileW(utf8_to_wstring(remote_path_legacy(remote_dir_, filename)).c_str());
    return ok;
}

#include <filesystem>

std::vector<std::string> Storage::list_remote_files()
{
    // Names relative to the remote dir with '/' separators (the form games
    // pass back into FileRead/FileDelete).
    std::vector<std::string> files;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(remote_dir_, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        // generic_wstring keeps '\\'; convert to UTF-8 with '/' like before.
        std::string rel = wstring_to_utf8(it->path().lexically_relative(remote_dir_).generic_wstring());
        for (char& c : rel) if (c == '\\') c = '/';
        files.push_back(rel);
    }
    return files;
}

int64_t Storage::remote_file_size(const std::string& filename)
{
    std::string path = remote_path(filename);
    if (!file_exists_w(path)) path = remote_path_legacy(remote_dir_, filename);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(utf8_to_wstring(path).c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        return size.QuadPart;
    }
    return -1;
}
