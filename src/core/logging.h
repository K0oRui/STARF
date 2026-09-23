#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// UTF-8/UTF-16 converters shared by the whole codebase (defined once in
// logging.cpp so this header stays dependency-free).
std::wstring utf8_to_wstring(const std::string& str);
std::string wstring_to_utf8(const std::wstring& wstr);

enum class StarLogLevel : uint8_t { Info, Warn, Error, Trace };

// Core logger. See core/logging.cpp for behavior:
// single shared file, atomic appends, rotation, loader-lock deferral.
void STAR_WriteLog(const char* fmt, ...);
void STAR_WriteLogLevel(StarLogLevel level, const char* fmt, ...);
// Gated on STAR_TRACE_API=1. Use for per-frame/per-query spam.
void STAR_WriteTraceLog(const char* fmt, ...);
void STAR_FlushLog();

// Must be called off the loader lock. Idempotent. Drains early messages.
void STAR_LogInit(const std::string& dll_dir);
// True while DllMain runs; messages are buffered instead of touching files.
void STAR_LogSetLoaderLock(bool in_loader);
// Idempotent. Flushes the log on unhandled exceptions.
void STAR_LogInstallCrashHandler();
// Restores the previous filter. Call on unload so it never dangles.
void STAR_LogUninstallCrashHandler();

bool STAR_LogTraceEnabled();
// Per-key throttle for hot paths. Returns true when the caller should log.
bool STAR_LogRateAllow(const char* key, unsigned interval_ms);

// PII helpers: SteamIDs and display names are masked unless tracing is on.
std::string STAR_MaskSteamId(uint64_t sid);
std::string STAR_MaskName(const std::string& name);

#define STAR_LOG(fmt, ...) STAR_WriteLog(fmt, ##__VA_ARGS__)
#define STAR_LOG_WARN(fmt, ...) STAR_WriteLogLevel(StarLogLevel::Warn, fmt, ##__VA_ARGS__)
#define STAR_LOG_ERROR(fmt, ...) STAR_WriteLogLevel(StarLogLevel::Error, fmt, ##__VA_ARGS__)
#define STAR_LOG_TRACE(fmt, ...) STAR_WriteTraceLog(fmt, ##__VA_ARGS__)
// Throttled log for hot paths (file-hook redirects): one line per key per interval.
#define STAR_LOG_RATE(key, ms, fmt, ...) do { if (STAR_LogRateAllow(key, ms)) STAR_LOG(fmt, ##__VA_ARGS__); } while (0)
// One-shot log for init paths (first frame, first upload, first failure).
#define STAR_LOG_ONCE(fmt, ...) do { static std::atomic<bool> star_logged_once{false}; if (!star_logged_once.exchange(true, std::memory_order_relaxed)) STAR_LOG(fmt, ##__VA_ARGS__); } while (0)
