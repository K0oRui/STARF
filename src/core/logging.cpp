#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

LONG WINAPI STAR_LogCrashFlush(EXCEPTION_POINTERS*);

namespace {
// Single shared file: <dll_dir>\STAR\star.log, fallback %TEMP%\star.log.
// Writes use a Win32 HANDLE opened with FILE_APPEND_DATA so concurrent
// launcher + game processes append atomically (one WriteFile per line).
// No file I/O runs under the loader lock: DllMain sets the loader flag
// and messages are buffered in memory until STAR_LogInit drains them.

constexpr size_t kLogMsgMax = 1024;
constexpr size_t kLogLineMax = 1280;
constexpr LONGLONG kLogMaxBytes = 5LL * 1024 * 1024;
constexpr size_t kLogPendingMax = 200;

std::mutex g_log_mutex;
HANDLE g_log_handle = INVALID_HANDLE_VALUE;
bool g_log_is_fallback = false;
std::string g_log_dll_dir;
std::vector<std::string> g_log_pending;
std::atomic<bool> g_log_in_loader{false};
bool g_log_crash_handler = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_crash_filter = nullptr;
std::unordered_map<std::string, DWORD> g_log_rate_marks;

const char* level_name(StarLogLevel level)
{
    switch (level) {
    case StarLogLevel::Warn: return "WARN";
    case StarLogLevel::Error: return "ERROR";
    case StarLogLevel::Trace: return "TRACE";
    default: return "INFO";
    }
}

void log_write_line(const std::string& line, bool flush_now);

HANDLE open_append(const std::wstring& path)
{
    return CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_open_handle(const std::wstring& path)
{
    if (g_log_handle != INVALID_HANDLE_VALUE) return;
    HANDLE h = open_append(path);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart >= kLogMaxBytes) {
        CloseHandle(h);
        std::wstring backup = path;
        size_t dot = backup.find_last_of(L'.');
        backup = (dot == std::wstring::npos) ? (backup + L".1") : (backup.substr(0, dot) + L".1.log");
        // Best effort rotation; if another process rotates at the same time
        // the last writer wins and at most one window is lost.
        MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
        h = open_append(path);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    g_log_handle = h;
}

void log_ensure_open(bool fresh)
{
    if (g_log_handle != INVALID_HANDLE_VALUE) return;
    if (!g_log_dll_dir.empty()) {
        std::wstring dir = utf8_to_wstring(g_log_dll_dir + "\\STAR");
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\star.log";
        if (fresh) DeleteFileW(path.c_str()); // clear on each launch
        log_open_handle(path);
    }
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        wchar_t temp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temp)) {
            std::wstring path = std::wstring(temp) + L"star.log";
            if (fresh) DeleteFileW(path.c_str()); // clear on each launch
            log_open_handle(path);
            g_log_is_fallback = (g_log_handle != INVALID_HANDLE_VALUE);
        }
    }
    for (const auto& line : g_log_pending) {
        if (g_log_handle == INVALID_HANDLE_VALUE) break;
        log_write_line(line, false);
    }
    g_log_pending.clear();
}

void log_write_line(const std::string& line, bool flush_now)
{
    if (g_log_handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_log_handle, line.data(), (DWORD)line.size(), &written, nullptr);
    if (flush_now) {
        FlushFileBuffers(g_log_handle);
        return;
    }
    // Throttled OS flush; errors flush now, as do Shutdown (explicit
    // STAR_FlushLog at the call site) and the crash handler.
    static auto last_flush = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last_flush >= std::chrono::seconds(1)) {
        FlushFileBuffers(g_log_handle);
        last_flush = now;
    }
}

void log_write_leveled(StarLogLevel level, const char* fmt, va_list args)
{
    char buf[kLogMsgMax];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) {
        const char suffix[] = "...";
        memcpy(buf + sizeof(buf) - sizeof(suffix), suffix, sizeof(suffix));
    }

    SYSTEMTIME st{};
    GetSystemTime(&st);
    char out_buf[kLogLineMax];
    snprintf(out_buf, sizeof(out_buf), "[STAR %04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu tid=%lu %s] %s\n",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(),
        level_name(level), buf);

    OutputDebugStringA(out_buf);

    bool flush_now = (level == StarLogLevel::Error);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_in_loader.load(std::memory_order_acquire)) {
        if (g_log_pending.size() < kLogPendingMax) g_log_pending.emplace_back(out_buf);
        return;
    }
    log_ensure_open(false);
    if (g_log_handle == INVALID_HANDLE_VALUE) return;
    log_write_line(out_buf, flush_now);
}
} // namespace

// Shared UTF-8/UTF-16 converters declared in logging.h. File scope (not the
// anonymous namespace above) so other translation units can link them.
std::wstring utf8_to_wstring(const std::string& str)
{
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (size_needed == 0) return L"";
    std::wstring out(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &out[0], size_needed);
    return out;
}

std::string wstring_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed == 0) return "";
    std::string out(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &out[0], size_needed, nullptr, nullptr);
    return out;
}

bool STAR_LogTraceEnabled()
{
    static const bool trace_api = [] {
        char value[2]{};
        return GetEnvironmentVariableA("STAR_TRACE_API", value, 2) == 1 && value[0] == '1';
    }();
    return trace_api;
}

void STAR_WriteLogLevel(StarLogLevel level, const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    log_write_leveled(level, fmt, args);
    va_end(args);
}

void STAR_WriteLog(const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    log_write_leveled(StarLogLevel::Info, fmt, args);
    va_end(args);
}

void STAR_WriteTraceLog(const char* fmt, ...)
{
    if (!fmt || !STAR_LogTraceEnabled()) return;
    va_list args;
    va_start(args, fmt);
    log_write_leveled(StarLogLevel::Trace, fmt, args);
    va_end(args);
}

void STAR_FlushLog()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_handle != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log_handle);
}

void STAR_LogInit(const std::string& dll_dir)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!dll_dir.empty()) g_log_dll_dir = dll_dir;
    if (g_log_is_fallback && !g_log_dll_dir.empty()) {
        // A pre-init message opened the TEMP fallback before the real dir was
        // known (late-injected hosts log before SteamAPI_Init). Move to the
        // real file so the session lands in one place.
        CloseHandle(g_log_handle);
        g_log_handle = INVALID_HANDLE_VALUE;
        g_log_is_fallback = false;
    }
    log_ensure_open(true);
}

void STAR_LogSetLoaderLock(bool in_loader)
{
    g_log_in_loader.store(in_loader, std::memory_order_release);
}

void STAR_LogInstallCrashHandler()
{
    if (g_log_crash_handler) return;
    g_log_crash_handler = true;
    g_prev_crash_filter = SetUnhandledExceptionFilter(STAR_LogCrashFlush);
}

void STAR_LogUninstallCrashHandler()
{
    if (!g_log_crash_handler) return;
    g_log_crash_handler = false;
    SetUnhandledExceptionFilter(g_prev_crash_filter);
    g_prev_crash_filter = nullptr;
}

LONG WINAPI STAR_LogCrashFlush(EXCEPTION_POINTERS* info)
{
    // Lock-free: the crashing thread may hold g_log_mutex. A torn flush is
    // better than deadlocking inside the crash handler.
    if (g_log_handle != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log_handle);
    // Chain: the game may have installed its own reporter before us.
    if (g_prev_crash_filter) return g_prev_crash_filter(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

bool STAR_LogRateAllow(const char* key, unsigned interval_ms)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    DWORD now = GetTickCount();
    auto it = g_log_rate_marks.find(key);
    if (it == g_log_rate_marks.end() || (now - it->second) >= interval_ms) {
        g_log_rate_marks[key] = now;
        return true;
    }
    return false;
}

std::string STAR_MaskSteamId(uint64_t sid)
{
    if (sid == 0) return "0";
    std::string s = std::to_string(sid);
    if (s.size() < 8) return "****"; // test/small IDs: substr would throw
    return s.substr(0, 4) + "..." + s.substr(s.size() - 4);
}

std::string STAR_MaskName(const std::string& name)
{
    if (name.empty()) return "(empty)";
    if (STAR_LogTraceEnabled()) return name;
    return std::string(1, name[0]) + "*** (" + std::to_string(name.size()) + " chars)";
}
