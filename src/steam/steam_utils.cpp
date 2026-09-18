#include "steam/steam_utils.h"
#include "core/callbacks.h"
#include "core/settings.h"
#include <wincodec.h>
#pragma comment(lib, "WindowsCodecs.lib")

StarSteamUtils::StarSteamUtils()
    : start_time_(std::chrono::steady_clock::now())
{
}

StarSteamUtils& StarSteamUtils::get()
{
    static StarSteamUtils instance;
    return instance;
}

uint32 StarSteamUtils::GetSecondsSinceAppActive()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    return (uint32)elapsed;
}

uint32 StarSteamUtils::GetSecondsSinceComputerActive()
{

    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    GetLastInputInfo(&lii);
    DWORD tick = GetTickCount();
    DWORD idle_ms = tick - lii.dwTime;
    return idle_ms / 1000u;
}

EUniverse StarSteamUtils::GetConnectedUniverse()
{
    return k_EUniversePublic;
}

uint32 StarSteamUtils::GetServerRealTime()
{
    return (uint32)time(nullptr);
}

const char* StarSteamUtils::GetIPCountry()
{
    return "US";
}

bool StarSteamUtils::GetImageSize(int iImage, uint32* pnWidth, uint32* pnHeight)
{
    if (iImage <= 0) return false;
    std::lock_guard<std::mutex> lock(images_mutex_);
    int idx = iImage - 1;
    if (idx < 0 || idx >= (int)images_.size()) return false;
    if (pnWidth)  *pnWidth  = images_[idx].width;
    if (pnHeight) *pnHeight = images_[idx].height;
    return true;
}

bool StarSteamUtils::GetImageRGBA(int iImage, uint8* pubDest, int nDestBufferSize)
{
    if (iImage <= 0 || !pubDest) return false;
    std::lock_guard<std::mutex> lock(images_mutex_);
    int idx = iImage - 1;
    if (idx < 0 || idx >= (int)images_.size()) return false;
    const auto& img = images_[idx];
    int needed = (int)(img.width * img.height * 4);
    if (nDestBufferSize < needed) return false;
    memcpy(pubDest, img.rgba.data(), needed);
    return true;
}

bool StarSteamUtils::GetCSERIPPort(uint32* unIP, uint16* usPort)
{
    STAR_UNREFERENCED(unIP); STAR_UNREFERENCED(usPort);
    return false;
}

uint8 StarSteamUtils::GetCurrentBatteryPower()
{
    SYSTEM_POWER_STATUS sps{};
    GetSystemPowerStatus(&sps);
    if (sps.ACLineStatus == 1) return 255;
    return sps.BatteryLifePercent;
}

uint32 StarSteamUtils::GetAppID()
{
    return Settings::get().app_id;
}

void StarSteamUtils::SetOverlayNotificationPosition(ENotificationPosition eNotificationPosition)
{
    STAR_UNREFERENCED(eNotificationPosition);
}

bool StarSteamUtils::IsAPICallCompleted(SteamAPICall_t hSteamAPICall, bool* pbFailed)
{
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    auto it = g_pending_results.find(hSteamAPICall);
    bool found = (it != g_pending_results.end());
    if (pbFailed) *pbFailed = found ? it->second.io_failure : false;
    return found;
}

ESteamAPICallFailure StarSteamUtils::GetAPICallFailureReason(SteamAPICall_t hSteamAPICall)
{
    STAR_UNREFERENCED(hSteamAPICall);
    return k_ESteamAPICallFailureNone;
}

bool StarSteamUtils::GetAPICallResult(SteamAPICall_t hSteamAPICall, void* pCallback, int cubCallback, int iCallbackExpected, bool* pbFailed)
{
    std::lock_guard<std::mutex> lock(g_callbacks_mutex);
    auto it = g_pending_results.find(hSteamAPICall);
    if (it == g_pending_results.end()) return false;
    if (it->second.callback_id != iCallbackExpected) return false;
    if (pbFailed) *pbFailed = it->second.io_failure;
    if (pCallback && cubCallback > 0 && !it->second.data.empty()) {
        int bytes = std::min(cubCallback, (int)it->second.data.size());
        memcpy(pCallback, it->second.data.data(), bytes);
    }
    g_pending_results.erase(it);
    return true;
}

void StarSteamUtils::RunFrame()
{
    STAR_RunCallbacks();
}

uint32 StarSteamUtils::GetIPCCallCount()
{
    return 1;
}

void StarSteamUtils::SetWarningMessageHook(SteamAPIWarningMessageHook_t pFunction)
{
    STAR_UNREFERENCED(pFunction);
}

bool StarSteamUtils::IsOverlayEnabled()
{
    return Settings::get().overlay_enabled;
}

bool StarSteamUtils::BOverlayNeedsPresent()
{
    return false;
}

SteamAPICall_t StarSteamUtils::CheckFileSignature(const char* szFileName)
{
    STAR_UNREFERENCED(szFileName);
    CheckFileSignature_t result{};
    result.m_eCheckFileSignature = k_ECheckFileSignatureNoSignaturesFoundForThisApp;
    return STAR_PostCallResult(CheckFileSignature_t::k_iCallback, &result, sizeof(result));
}

bool StarSteamUtils::ShowGamepadTextInput(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, const char* pchDescription, uint32 unCharMax, const char* pchExistingText)
{
    STAR_UNREFERENCED(eInputMode); STAR_UNREFERENCED(eLineInputMode);
    STAR_UNREFERENCED(pchDescription); STAR_UNREFERENCED(unCharMax); STAR_UNREFERENCED(pchExistingText);
    return false;
}

uint32 StarSteamUtils::GetEnteredGamepadTextLength() { return 0; }

bool StarSteamUtils::GetEnteredGamepadTextInput(char* pchText, uint32 cchText)
{
    STAR_UNREFERENCED(cchText);
    if (pchText) pchText[0] = 0;
    return false;
}

const char* StarSteamUtils::GetSteamUILanguage()
{
    return Settings::get().language.c_str();
}

bool StarSteamUtils::IsSteamRunningInVR() { return false; }

void StarSteamUtils::SetOverlayNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    STAR_UNREFERENCED(nHorizontalInset); STAR_UNREFERENCED(nVerticalInset);
}

int StarSteamUtils::StoreImage(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba)
{
    std::lock_guard<std::mutex> lock(images_mutex_);
    StarImage img;
    img.width = w;
    img.height = h;
    img.rgba = rgba;
    images_.push_back(std::move(img));
    return (int)images_.size();
}

namespace {
// WIC needs COM on THIS thread; overlay Present hooks run on the game's
// render thread, which may never have called CoInitialize.
struct WicFrame {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool com_here = false;
    ~WicFrame() {
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        if (com_here) CoUninitialize();
    }
    bool open(const std::string& path) {
        HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (cohr == S_OK) com_here = true;
        else if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE) {
            STAR_LOG("WIC: CoInitializeEx failed hr=0x%08x for %s", (unsigned)cohr, path.c_str());
            return false;
        }
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory, (void**)&factory);
        if (FAILED(hr) || !factory) {
            STAR_LOG("WIC: factory failed hr=0x%08x for %s", (unsigned)hr, path.c_str());
            return false;
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) {
            STAR_LOG("WIC: bad path %s", path.c_str());
            return false;
        }
        std::wstring wpath((size_t)wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
        hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr) || !decoder) {
            STAR_LOG("WIC: no decoder hr=0x%08x for %s", (unsigned)hr, path.c_str());
            return false;
        }
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) {
            STAR_LOG("WIC: no frame hr=0x%08x for %s", (unsigned)hr, path.c_str());
            return false;
        }
        return true;
    }
};
} // namespace

int StarSteamUtils::LoadImageFromFile(const std::string& path){
    if (path.empty()) return 0;

    WicFrame fr;
    if (!fr.open(path)) return 0;

    IWICFormatConverter* converter = nullptr;
    fr.factory->CreateFormatConverter(&converter);
    if (!converter) {
        STAR_LOG("WIC: no converter for %s", path.c_str());
        return 0;
    }

    HRESULT hr = converter->Initialize(fr.frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        STAR_LOG("WIC: convert failed hr=0x%08x for %s", (unsigned)hr, path.c_str());
        converter->Release();
        return 0;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        STAR_LOG("WIC: bad size %ux%u for %s", w, h, path.c_str());
        converter->Release();
        return 0;
    }

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data());
    converter->Release();
    if (FAILED(hr)) {
        STAR_LOG("WIC: CopyPixels failed hr=0x%08x for %s", (unsigned)hr, path.c_str());
        return 0;
    }

    return StoreImage(w, h, rgba);
}

bool StarSteamUtils::GetImageFileSize(const std::string& path, uint32* w, uint32* h){
    if (path.empty()) return false;
    WicFrame fr;
    if (!fr.open(path)) return false;
    UINT fw = 0, fh = 0;
    if (FAILED(fr.frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) return false;
    if (w) *w = fw;
    if (h) *h = fh;
    return true;
}

bool StarSteamUtils::LoadIconFile(const std::string& full_path, std::vector<uint8_t>& rgba, int& w, int& h)
{
    rgba.clear(); w = 0; h = 0;
    int handle = LoadImageFromFile(full_path);
    if (handle <= 0) return false;
    uint32 uw = 0, uh = 0;
    if (!GetImageSize(handle, &uw, &uh) || uw == 0 || uh == 0) return false;
    w = (int)uw; h = (int)uh;
    rgba.resize((size_t)uw * uh * 4);
    return GetImageRGBA(handle, rgba.data(), (int)rgba.size());
}

bool StarSteamUtils::LoadSummaryIcon(std::vector<uint8_t>& rgba, int& w, int& h)
{
    rgba.clear(); w = 0; h = 0;
    std::string full = Settings::get().settings_dir + "\\Icons\\summary.png";
    if (!LoadIconFile(full, rgba, w, h)) return false;
    // Monochrome mask art (e.g. a black glyph): tint gold, keep per-pixel
    // alpha for smooth edges. Colored art passes through untouched.
    long spread = 0;
    size_t n = 0;
    for (size_t i = 0; i < (size_t)w * h; i += 7) {
        if (rgba[i * 4 + 3] > 20) {
            int r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            spread += (mx - mn); n++;
        }
    }
    if (n > 0 && spread / (long)n < 48) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 205;
            rgba[i * 4 + 2] = 70;
        }
    }
    return true;
}

bool StarSteamUtils::ShowGamepadTextInput(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, const char* pchDescription, uint32 unCharMax)
{
    return ShowGamepadTextInput(eInputMode, eLineInputMode, pchDescription, unCharMax, nullptr);
}

bool StarSteamUtils::IsSteamInBigPictureMode() { return false; }
void StarSteamUtils::StartVRDashboard() {}
bool StarSteamUtils::IsVRHeadsetStreamingEnabled() { return false; }
void StarSteamUtils::SetVRHeadsetStreamingEnabled(bool bEnabled) { STAR_UNREFERENCED(bEnabled); }
bool StarSteamUtils::IsSteamChinaLauncher() { return false; }

ESteamIPv6ConnectivityState StarSteamUtils::GetIPv6ConnectivityState(ESteamIPv6ConnectivityProtocol eProtocol)
{
    STAR_UNREFERENCED(eProtocol);
    return k_ESteamIPv6ConnectivityState_Unknown;
}

bool StarSteamUtils::InitFilterText() { return false; }

int StarSteamUtils::FilterText(char* pchOutFilteredText, uint32 nByteSizeOutFilteredText, const char* pchInputMessage, bool bLegalOnly)
{
    STAR_UNREFERENCED(bLegalOnly);

    if (pchOutFilteredText && nByteSizeOutFilteredText > 0) {
        if (pchInputMessage) strncpy_s(pchOutFilteredText, nByteSizeOutFilteredText, pchInputMessage, _TRUNCATE);
        else pchOutFilteredText[0] = 0;
    }
    return pchInputMessage ? (int)strlen(pchInputMessage) : 0;
}

bool StarSteamUtils::InitFilterText(uint32 unFilterOptions) { STAR_UNREFERENCED(unFilterOptions); return false; }

int StarSteamUtils::FilterText(ETextFilteringContext eContext, CSteamID sourceSteamID, const char* pchInputMessage, char* pchOutFilteredText, uint32 nByteSizeOutFilteredText)
{
    STAR_UNREFERENCED(eContext); STAR_UNREFERENCED(sourceSteamID);
    if (pchOutFilteredText && nByteSizeOutFilteredText > 0) {
        if (pchInputMessage) strncpy_s(pchOutFilteredText, nByteSizeOutFilteredText, pchInputMessage, _TRUNCATE);
        else pchOutFilteredText[0] = 0;
    }
    return pchInputMessage ? (int)strlen(pchInputMessage) : 0;
}

bool StarSteamUtils::IsSteamRunningOnSteamDeck() { return false; }

bool StarSteamUtils::ShowFloatingGamepadTextInput(EFloatingGamepadTextInputMode eKeyboardMode, int x, int y, int w, int h)
{
    STAR_UNREFERENCED(eKeyboardMode); STAR_UNREFERENCED(x); STAR_UNREFERENCED(y);
    STAR_UNREFERENCED(w); STAR_UNREFERENCED(h);
    return false;
}

void StarSteamUtils::SetGameLauncherMode(bool bLauncherMode) { STAR_UNREFERENCED(bLauncherMode); }
bool StarSteamUtils::DismissFloatingGamepadTextInput() { return false; }
bool StarSteamUtils::DismissGamepadTextInput() { return false; }
