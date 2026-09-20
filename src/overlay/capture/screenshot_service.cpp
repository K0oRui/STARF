#include "overlay/capture/screenshot_service.h"
#include "overlay/core/pixel_copy.h"
#include "core/settings.h"
#include "core/storage.h"
#include "core/star_common.h"
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")
#include <shlobj.h>

std::string ScreenshotService::dir()
{
    // Per-game subfolder so shots never mix across titles.
    // Falls back to the game-local STAR folder when Documents is unavailable.
    std::string leaf = "\\STAR\\screenshots\\" + std::to_string(Settings::get().app_id);
    char docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)) && docs[0])
        return std::string(docs) + leaf;
    return Settings::get().settings_dir + leaf;
}

std::string ScreenshotService::next_path()
{
    std::string base_dir = dir();
    Storage::ensure_dir(base_dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    static std::atomic<unsigned> sequence{0};
    char base[128];
    snprintf(base, sizeof(base), "STAR_%u_%04d%02d%02d_%02d%02d%02d_%u",
        Settings::get().app_id, (int)st.wYear, (int)st.wMonth, (int)st.wDay,
        (int)st.wHour, (int)st.wMinute, (int)st.wSecond, sequence.fetch_add(1));
    for (int i = 0; i < 100; i++) {
        char full[MAX_PATH];
        if (i == 0) snprintf(full, sizeof(full), "%s\\%s.png", base_dir.c_str(), base);
        else snprintf(full, sizeof(full), "%s\\%s_%d.png", base_dir.c_str(), base, i);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) return full;
    }
    return "";
}

bool ScreenshotService::save_rgba_png(const std::string& path, const uint8_t* rgba, int w, int h)
{
    if (path.empty() || !rgba || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    // Screenshots must be opaque: backbuffers often carry garbage/zero in
    // alpha (X8 formats, GL default framebuffer), which viewers show as
    // black or checkered "strange colors".
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        px[i * 4 + 0] = rgba[i * 4 + 0];
        px[i * 4 + 1] = rgba[i * 4 + 1];
        px[i * 4 + 2] = rgba[i * 4 + 2];
        px[i * 4 + 3] = 255;
    }
    const uint8_t* data = px.data();
    bool com_here = false;
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (cohr == S_OK) com_here = true;
    else if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory, (void**)&factory)) && factory) {
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath((size_t)(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
        if (SUCCEEDED(factory->CreateStream(&stream)) && stream &&
            SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && encoder &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && frame &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize((UINT)w, (UINT)h))) {
            GUID fmt = GUID_WICPixelFormat32bppRGBA;
            // NOTE: the PNG encoder may coerce this (typically to BGRA);
            // the buffer must match whatever comes back, or R/B swap.
            if (SUCCEEDED(frame->SetPixelFormat(&fmt))) {
                const uint8_t* src = data;
                std::vector<uint8_t> bgra;
                bool fmt_ok = true;
                if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
                    bgra.resize((size_t)w * h * 4);
                    copy_pixels32(bgra.data(), (size_t)w * 4, data, (size_t)w * 4, w, h, true);
                    src = bgra.data();
                } else if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
                    STAR_LOG("Screenshot: unexpected pixel format, aborting %s", path.c_str());
                    fmt_ok = false;
                }
                if (fmt_ok &&
                    SUCCEEDED(frame->WritePixels((UINT)h, (UINT)w * 4, (UINT)(w * h * 4), (BYTE*)src)) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
                    ok = true;
                }
            }
        }
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        factory->Release();
    }
    if (com_here) CoUninitialize();
    if (!ok) STAR_LOG("Screenshot encode failed: %s", path.c_str());
    return ok;
}


bool ScreenshotService::save_async(const std::string& path, std::vector<uint8_t> rgba, int w, int h, bool dark)
{
    if (path.empty() || w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
        rgba.size() != (size_t)w * h * 4) return false;
    size_t bytes = rgba.size();
    if (outstanding_.fetch_add(1) >= 3) { --outstanding_; return false; }
    bool accepted = worker_.submit([this, path, rgba = std::move(rgba), w, h, dark] {
        if (save_rgba_png(path, rgba.data(), w, h)) {
            std::lock_guard<std::mutex> lock(completed_mutex_);
            completed_.push_back({path, dark});
        }
        // Count only in-flight encodes: completed-but-unconsumed notifications
        // must not block the gate when the UI is not rendering.
        --outstanding_;
    }, bytes);
    if (!accepted) { --outstanding_; STAR_LOG("Screenshot queue full: %s", path.c_str()); }
    return accepted;
}

std::vector<ScreenshotService::Completed> ScreenshotService::take_completed()
{
    std::lock_guard<std::mutex> lock(completed_mutex_);
    std::vector<Completed> result;
    result.swap(completed_);
    return result;
}
