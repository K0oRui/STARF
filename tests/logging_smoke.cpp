#ifdef NDEBUG
#undef NDEBUG
#endif
// Unit coverage for the file logger: null safety, truncation marker,
// trace gating, rate limiting, PII masking, and end-to-end file output
// with timestamp/pid/tid/level prefix.
#include "core/logging.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

static std::string read_all(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    assert(f.is_open());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main()
{
    // Crash handler install/uninstall is idempotent and chain-safe.
    STAR_LogInstallCrashHandler();
    STAR_LogUninstallCrashHandler();
    STAR_LogInstallCrashHandler();

    // Null format strings never crash.
    STAR_WriteLog(nullptr);
    STAR_WriteTraceLog(nullptr);
    STAR_WriteLogLevel(StarLogLevel::Info, nullptr);

    // Trace is opt-in; the test process does not set STAR_TRACE_API.
    assert(!STAR_LogTraceEnabled());

    // Rate limiter: first call passes, immediate second is held back.
    assert(STAR_LogRateAllow("logging-smoke-key", 60000));
    assert(!STAR_LogRateAllow("logging-smoke-key", 60000));
    assert(STAR_LogRateAllow("logging-smoke-other-key", 60000));

    // PII masking.
    assert(STAR_MaskSteamId(0) == "0");
    assert(STAR_MaskSteamId(456) == "****"); // short/test IDs never throw
    assert(STAR_MaskSteamId(76561198000000001ULL) == "7656...0001");
    assert(STAR_MaskName("") == "(empty)");
    assert(STAR_MaskName("Alice") == "A*** (5 chars)");

    // End-to-end: init into an isolated temp dir, write, flush, read back.
    // A stale file from a previous run must be cleared at init.
    wchar_t temp[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temp) != 0);
    std::wstring unique = std::wstring(temp) + L"star-logging-smoke";
    CreateDirectoryW(unique.c_str(), nullptr);
    CreateDirectoryW((unique + L"\\STAR").c_str(), nullptr);
    std::wstring log_path = unique + L"\\STAR\\star.log";
    { std::ofstream stale(log_path, std::ios::binary); stale << "stale from last run\n"; }

    char dll_dir[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, unique.c_str(), -1, dll_dir, MAX_PATH, nullptr, nullptr);
    STAR_LogInit(dll_dir);

    STAR_WriteLog("hello %d", 42);
    STAR_WriteLogLevel(StarLogLevel::Error, "boom %s", "xyz");
    std::string big(2000, 'x');
    STAR_WriteLog("%s", big.c_str());
    STAR_WriteTraceLog("trace spam that must not land without STAR_TRACE_API=1");
    STAR_FlushLog();

    std::string body = read_all(log_path);
    assert(body.find("stale from last run") == std::string::npos); // cleared at init
    assert(body.find("hello 42") != std::string::npos);
    assert(body.find(" INFO]") != std::string::npos);
    assert(body.find("boom xyz") != std::string::npos);
    assert(body.find(" ERROR]") != std::string::npos);
    assert(body.find("pid=") != std::string::npos);
    assert(body.find("tid=") != std::string::npos);
    assert(body.find("T") != std::string::npos); // UTC timestamp separator
    assert(body.find("...") != std::string::npos); // truncation marker
    assert(body.find("trace spam") == std::string::npos); // gated off

    std::printf("logging_smoke: all checks passed (%zu bytes)\n", body.size());
    return 0;
}
