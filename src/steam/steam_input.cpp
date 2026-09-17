#include "steam/steam_input.h"
#include "core/settings.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

StarSteamInput& StarSteamInput::get() { static StarSteamInput i; return i; }

// XInput loaded dynamically (1.4, falling back to 9.1.0) so machines
// without the redist still boot - they just report no pads.
namespace {
struct XApi {
    HMODULE mod = nullptr;
    DWORD (WINAPI* GetState)(DWORD, XINPUT_STATE*) = nullptr;
    DWORD (WINAPI* SetState)(DWORD, XINPUT_VIBRATION*) = nullptr;
    bool ensure() {
        if (GetState) return true;
        mod = LoadLibraryA("xinput1_4.dll");
        if (!mod) mod = LoadLibraryA("xinput9_1_0.dll");
        if (!mod) return false;
        GetState = (DWORD (WINAPI*)(DWORD, XINPUT_STATE*))GetProcAddress(mod, "XInputGetState");
        SetState = (DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*))GetProcAddress(mod, "XInputSetState");
        return GetState != nullptr;
    }
} g_xinput;

uint64 hash_action_name(const char* n)
{
    uint64 h = 14695981039346656037ull;
    for (const char* p = n; p && *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    return h ? h : 1;
}

WORD button_bit(const std::string& b)
{
    if (b == "A") return XINPUT_GAMEPAD_A;
    if (b == "B") return XINPUT_GAMEPAD_B;
    if (b == "X") return XINPUT_GAMEPAD_X;
    if (b == "Y") return XINPUT_GAMEPAD_Y;
    if (b == "START") return XINPUT_GAMEPAD_START;
    if (b == "BACK") return XINPUT_GAMEPAD_BACK;
    if (b == "LSTICK") return XINPUT_GAMEPAD_LEFT_THUMB;
    if (b == "RSTICK") return XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b == "LBUMPER") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b == "RBUMPER") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (b == "DUP") return XINPUT_GAMEPAD_DPAD_UP;
    if (b == "DDOWN") return XINPUT_GAMEPAD_DPAD_DOWN;
    if (b == "DLEFT") return XINPUT_GAMEPAD_DPAD_LEFT;
    if (b == "DRIGHT") return XINPUT_GAMEPAD_DPAD_RIGHT;
    return 0;
}

EInputActionOrigin button_origin(const std::string& b, bool sony)
{
    static const struct { const char* n; EInputActionOrigin x, p; } kOrigins[] = {
        { "A", k_EInputActionOrigin_XBox360_A, k_EInputActionOrigin_PS4_X },
        { "B", k_EInputActionOrigin_XBox360_B, k_EInputActionOrigin_PS4_Circle },
        { "X", k_EInputActionOrigin_XBox360_X, k_EInputActionOrigin_PS4_Square },
        { "Y", k_EInputActionOrigin_XBox360_Y, k_EInputActionOrigin_PS4_Triangle },
        { "START", k_EInputActionOrigin_XBox360_Start, k_EInputActionOrigin_PS4_Options },
        { "BACK", k_EInputActionOrigin_XBox360_Back, k_EInputActionOrigin_PS4_Share },
        { "LSTICK", k_EInputActionOrigin_XBox360_LeftStick_Click, k_EInputActionOrigin_PS4_LeftStick_Click },
        { "RSTICK", k_EInputActionOrigin_XBox360_RightStick_Click, k_EInputActionOrigin_PS4_RightStick_Click },
        { "LBUMPER", k_EInputActionOrigin_XBox360_LeftBumper, k_EInputActionOrigin_PS4_LeftBumper },
        { "RBUMPER", k_EInputActionOrigin_XBox360_RightBumper, k_EInputActionOrigin_PS4_RightBumper },
        { "DUP", k_EInputActionOrigin_XBox360_DPad_North, k_EInputActionOrigin_PS4_DPad_North },
        { "DDOWN", k_EInputActionOrigin_XBox360_DPad_South, k_EInputActionOrigin_PS4_DPad_South },
        { "DLEFT", k_EInputActionOrigin_XBox360_DPad_West, k_EInputActionOrigin_PS4_DPad_West },
        { "DRIGHT", k_EInputActionOrigin_XBox360_DPad_East, k_EInputActionOrigin_PS4_DPad_East },
        { "DLTRIGGER", k_EInputActionOrigin_XBox360_LeftTrigger_Pull, k_EInputActionOrigin_PS4_LeftTrigger_Pull },
        { "DRTRIGGER", k_EInputActionOrigin_XBox360_RightTrigger_Pull, k_EInputActionOrigin_PS4_RightTrigger_Pull },
    };
    for (auto& o : kOrigins)
        if (b == o.n) return sony ? o.p : o.x;
    return k_EInputActionOrigin_None;
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p) s.erase(0, p);
    return s;
}

std::string upper_trim(std::string s)
{
    s = trim(s);
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

float norm_stick(SHORT v, int dead)
{
    if (abs(v) < dead) return 0.f;
    return v < 0 ? (v + dead) / (float)(32768 - dead) : (v - dead) / (float)(32767 - dead);
}

// DualSense/DS4 over Bluetooth (DirectInput): 12 buttons, 6 axes, dpad on
// POV0. Face order is Sony's: 0 square, 1 cross, 2 circle, 3 triangle,
// 4 LB, 5 RB, 6 LT, 7 RT, 8 share, 9 options, 10 L3, 11 R3.
void map_sony(const DIJOYSTATE2& js, const LONG rest[6], StarSteamInput::PadState& out)
{
    static const WORD face[12] = {
        XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
        XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    out.buttons = 0;
    for (int i = 0; i < 12; i++)
        if (js.rgbButtons[i] & 0x80) out.buttons |= face[i];
    auto trig = [](LONG v, LONG r) -> BYTE {
        if (v >= r) return (BYTE)((v - r) * 255 / (65535 - r > 0 ? 65535 - r : 1));
        return (BYTE)((r - v) * 255 / (r > 0 ? r : 1));
    };
    out.lt = trig(js.lZ, rest[2]);
    out.rt = trig(js.lRz, rest[5]);
    if (js.rgbButtons[6] & 0x80) out.lt = 255;
    if (js.rgbButtons[7] & 0x80) out.rt = 255;
    // Sticks stay raw centered here; eval_stick applies the one deadzone.
    auto center = [](LONG v, LONG r) -> SHORT {
        LONG d = v - r;
        if (d > 32767) return 32767;
        if (d < -32768) return -32768;
        return (SHORT)d;
    };
    out.lx = center(js.lX, rest[0]);
    out.ly = center(js.lY, rest[1]);
    out.rx = center(js.lRx, rest[3]);
    out.ry = center(js.lRy, rest[4]);
    DWORD pov = js.rgdwPOV[0];
    if (pov != 0xFFFF) {
        if (pov <= 4500 || pov >= 31500) out.buttons |= XINPUT_GAMEPAD_DPAD_UP;
        if (pov >= 4500 && pov <= 13500) out.buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (pov >= 13500 && pov <= 22500) out.buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (pov >= 22500 && pov <= 31500) out.buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
}
}

uint64_t StarSteamInput::action_handle(const char* n)
{
    uint64_t h = hash_action_name(n);
    register_name(h, n);
    return h;
}

const std::string* StarSteamInput::action_name(uint64_t h) const
{
    auto it = handle_names_.find(h);
    return it == handle_names_.end() ? nullptr : &it->second;
}

void StarSteamInput::poll_pads()
{
    if (g_xinput.ensure()) {
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE s{};
            if (g_xinput.GetState(i, &s) == ERROR_SUCCESS) {
                pads_[i].connected = true;
                pads_[i].sony = false;
                pads_[i].buttons = s.Gamepad.wButtons;
                pads_[i].lt = s.Gamepad.bLeftTrigger;
                pads_[i].rt = s.Gamepad.bRightTrigger;
                pads_[i].lx = s.Gamepad.sThumbLX;
                pads_[i].ly = s.Gamepad.sThumbLY;
                pads_[i].rx = s.Gamepad.sThumbRX;
                pads_[i].ry = s.Gamepad.sThumbRY;
            } else {
                pads_[i].connected = false;
            }
        }
    }
    poll_di();
}

bool StarSteamInput::valid_pad(InputHandle_t h) const
{
    return h >= 1 && h <= 8 && pads_[h - 1].connected;
}

namespace {
struct DiEnumCtx { StarSteamInput* self; };
}

void StarSteamInput::poll_di()
{
    if (!di_) {
        HMODULE m = LoadLibraryA("dinput8.dll");
        if (!m) return;
        typedef HRESULT (WINAPI* DIC)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        DIC f = (DIC)GetProcAddress(m, "DirectInput8Create");
        if (!f || FAILED(f(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&di_, nullptr))) {
            di_ = nullptr;
            return;
        }
    }
    if (!di_enum_done_) {
        di_enum_done_ = true;
        DiEnumCtx ctx{ this };
        di_->EnumDevices(DI8DEVCLASS_GAMECTRL, StarSteamInput::di_enum_cb, &ctx, DIEDFL_ATTACHEDONLY);
    }
    for (int i = 0; i < 4; i++) {
        if (!di_dev_[i] || di_dead_[i]) { pads_[i + 4].connected = false; continue; }
        HRESULT hr = di_dev_[i]->Poll();
        DIJOYSTATE2 js{};
        if (SUCCEEDED(hr)) hr = di_dev_[i]->GetDeviceState(sizeof(js), &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            di_dev_[i]->Acquire();
            hr = di_dev_[i]->GetDeviceState(sizeof(js), &js);
        }
        if (FAILED(hr)) { pads_[i + 4].connected = false; continue; }
        LONG* av[6] = { &js.lX, &js.lY, &js.lZ, &js.lRx, &js.lRy, &js.lRz };
        if (!di_rest_ok_[i]) {
            for (int a = 0; a < 6; a++) di_rest_[i][a] = *av[a];
            di_rest_ok_[i] = true;
        }
        PadState& p = pads_[i + 4];
        p.connected = true;
        p.sony = di_sony_[i];
        p.ps5 = di_ps5_[i];
        map_sony(js, di_rest_[i], p);
    }
}

BOOL CALLBACK StarSteamInput::di_enum_cb(const DIDEVICEINSTANCEA* d, void* v)
{
    StarSteamInput* self = ((DiEnumCtx*)v)->self;
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (!self->di_dev_[i]) { slot = i; break; }
    }
    if (slot < 0) return DIENUM_STOP;
    std::string prod = d->tszProductName ? d->tszProductName : "";
    for (char& c : prod) c = (char)tolower((unsigned char)c);
    bool ps5 = prod.find("dualsense") != std::string::npos;
    bool ps4 = prod.find("wireless controller") != std::string::npos;
    if (!ps5 && !ps4) {
        STAR_LOG("Input: DirectInput pad not mapped, skipped: %s",
            d->tszProductName ? d->tszProductName : "(null)");
        return DIENUM_CONTINUE;
    }
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(self->di_->CreateDevice(d->guidInstance, &dev, nullptr)))
        return DIENUM_CONTINUE;
    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(dev->SetCooperativeLevel(GetDesktopWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        dev->Release();
        return DIENUM_CONTINUE;
    }
    dev->Acquire();
    self->di_dev_[slot] = dev;
    self->di_sony_[slot] = true;
    self->di_ps5_[slot] = ps5;
    STAR_LOG("Input: DirectInput pad mapped: %s (slot %d)",
        d->tszProductName ? d->tszProductName : "(null)", slot + 5);
    return DIENUM_CONTINUE;
}

void StarSteamInput::rumble_di(int idx, unsigned short l, unsigned short r)
{
    if (idx < 0 || idx >= 4 || !di_dev_[idx]) return;
    unsigned mag = l > r ? l : r;
    if (mag == 0) {
        if (di_fx_[idx]) di_fx_[idx]->Stop();
        return;
    }
    if (!di_fx_[idx]) {
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_OBJECTIDS;
    eff.dwGain = 10000;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    DWORD axes[1] = { DIJOFS_X };
    eff.rgdwAxes = axes;
    eff.cbTypeSpecificParams = 0;
        di_dev_[idx]->CreateEffect(GUID_ConstantForce, &eff, &di_fx_[idx], nullptr);
        if (!di_fx_[idx]) return;
    }
    DICONSTANTFORCE cf{};
    cf.lMagnitude = (LONG)(mag * 10000 / 65535);
    DIEFFECT mod{};
    mod.dwSize = sizeof(mod);
    mod.dwFlags = DIEFF_OBJECTIDS;
    mod.cbTypeSpecificParams = sizeof(cf);
    mod.lpvTypeSpecificParams = &cf;
    di_fx_[idx]->SetParameters(&mod, DIEP_TYPESPECIFICPARAMS);
    di_fx_[idx]->Start(1, 0);
}

const char* StarSteamInput::register_name(uint64_t h, const char* n)
{
    if (!h || !n || !*n) return "";
    auto it = handle_names_.find(h);
    if (it == handle_names_.end()) handle_names_[h] = n;
    return handle_names_[h].c_str();
}

// Goldberg-format binding files: STAR/controller/<ACTION_SET>.txt with
// ACTION=BUTTON[,BUTTON] or ACTION=ANALOG=MODE lines.
const StarSteamInput::ActionSetBind* StarSteamInput::bindings_for(InputHandle_t h, const char* action, bool analog)
{
    if (!valid_pad(h) || !action || !*action) return nullptr;
    if (!sets_scanned_) {
        sets_scanned_ = true;
        std::string dir = Settings::get().settings_dir + "\\Controller\\*.txt";
        WIN32_FIND_DATAA fd{};
        HANDLE hf = FindFirstFileA(dir.c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                std::string set = fd.cFileName;
                size_t dot = set.rfind('.');
                if (dot != std::string::npos) set.resize(dot);
                ActionSetBind& b = set_cache_[set];
                std::string path = Settings::get().settings_dir + "\\Controller\\" + fd.cFileName;
                std::ifstream f(path);
                std::string line;
                while (std::getline(f, line)) {
                    std::vector<std::string> parts;
                    size_t p = 0;
                    while (true) {
                        size_t e = line.find('=', p);
                        parts.push_back(line.substr(p, e == std::string::npos ? e : e - p));
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                    if (parts.size() < 2) continue;
                    std::string name = trim(parts[0]);
                    if (name.empty() || name[0] == '#') continue;
                    if (parts.size() == 2) {
                        DigitalBind db;
                        size_t q = 0;
                        while (true) {
                            size_t e = parts[1].find(',', q);
                            std::string btn = upper_trim(parts[1].substr(q, e == std::string::npos ? e : e - q));
                            if (!btn.empty()) db.buttons.push_back(btn);
                            if (e == std::string::npos) break;
                            q = e + 1;
                        }
                        if (!db.buttons.empty()) b.digital[name] = db;
                    } else {
                        AnalogBind ab;
                        ab.source = upper_trim(parts[1]);
                        ab.mode = upper_trim(parts[2]);
                        if (!ab.source.empty()) b.analog[name] = ab;
                    }
                }
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
    }
    // Active set first, then any other loaded set (games that never activate).
    std::string active;
    if (h >= 1 && h <= 8 && active_set_[h - 1]) {
        auto it = handle_names_.find(active_set_[h - 1]);
        if (it != handle_names_.end()) active = it->second;
    }
    if (!active.empty()) {
        auto it = set_cache_.find(active);
        if (it != set_cache_.end()) {
            if (analog) { auto f = it->second.analog.find(action); if (f != it->second.analog.end()) return &it->second; }
            else { auto f = it->second.digital.find(action); if (f != it->second.digital.end()) return &it->second; }
        }
    }
    for (auto& kv : set_cache_) {
        if (kv.first == active) continue;
        if (analog) { auto f = kv.second.analog.find(action); if (f != kv.second.analog.end()) return &kv.second; }
        else { auto f = kv.second.digital.find(action); if (f != kv.second.digital.end()) return &kv.second; }
    }
    return nullptr;
}

bool StarSteamInput::eval_button(int pad, const std::string& btn)
{
    const PadState& g = pads_[pad];
    WORD bit = button_bit(btn);
    if (bit) return (g.buttons & bit) != 0;
    if (btn == "DLTRIGGER") return g.lt > 64;
    if (btn == "DRTRIGGER") return g.rt > 64;
    const SHORT dz = 8000;
    if (btn == "DLJOYUP") return g.ly > dz;
    if (btn == "DLJOYDOWN") return g.ly < -dz;
    if (btn == "DLJOYLEFT") return g.lx < -dz;
    if (btn == "DLJOYRIGHT") return g.lx > dz;
    if (btn == "DRJOYUP") return g.ry > dz;
    if (btn == "DRJOYDOWN") return g.ry < -dz;
    if (btn == "DRJOYLEFT") return g.rx < -dz;
    if (btn == "DRJOYRIGHT") return g.rx > dz;
    return false;
}

void StarSteamInput::eval_stick(int pad, const std::string& src, float& x, float& y)
{
    const PadState& g = pads_[pad];
    x = 0.f; y = 0.f;
    if (src == "LJOY") {
        x = norm_stick(g.lx, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        y = norm_stick(g.ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    } else if (src == "RJOY") {
        x = norm_stick(g.rx, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        y = norm_stick(g.ry, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    } else if (src == "LTRIGGER") {
        x = g.lt / 255.f;
    } else if (src == "RTRIGGER") {
        x = g.rt / 255.f;
    } else if (src == "DPAD") {
        if (g.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) x += 1.f;
        if (g.buttons & XINPUT_GAMEPAD_DPAD_LEFT) x -= 1.f;
        if (g.buttons & XINPUT_GAMEPAD_DPAD_UP) y += 1.f;
        if (g.buttons & XINPUT_GAMEPAD_DPAD_DOWN) y -= 1.f;
    }
}

bool StarSteamInput::Init(bool bExplicitlyCallRunFrame) { STAR_UNREFERENCED(bExplicitlyCallRunFrame); poll_pads(); return true; }
bool StarSteamInput::Shutdown() { return true; }
bool StarSteamInput::SetInputActionManifestFilePath(const char* p) { STAR_UNREFERENCED(p); return true; }
void StarSteamInput::RunFrame(bool b) { STAR_UNREFERENCED(b); poll_pads(); }
bool StarSteamInput::BWaitForData(bool bWaitForever, uint32 unTimeout) { STAR_UNREFERENCED(bWaitForever); STAR_UNREFERENCED(unTimeout); return false; }
bool StarSteamInput::BNewDataAvailable() { poll_pads(); for (auto& p : pads_) if (p.connected) return true; return false; }
int StarSteamInput::GetConnectedControllers(InputHandle_t* handlesOut)
{
    poll_pads();
    int n = 0;
    for (DWORD i = 0; i < 8; i++) {
        if (pads_[i].connected) {
            if (handlesOut) handlesOut[n] = (InputHandle_t)(i + 1);
            n++;
        }
    }
    return n;
}
void StarSteamInput::EnableDeviceCallbacks() {}
void StarSteamInput::EnableActionEventCallbacks(SteamInputActionEventCallbackPointer p) { STAR_UNREFERENCED(p); }
InputActionSetHandle_t StarSteamInput::GetActionSetHandle(const char* n) { return (InputActionSetHandle_t)action_handle(n); }
void StarSteamInput::ActivateActionSet(InputHandle_t h, InputActionSetHandle_t a) { if (h >= 1 && h <= 8) active_set_[h - 1] = a; }
InputActionSetHandle_t StarSteamInput::GetCurrentActionSet(InputHandle_t h) { return (h >= 1 && h <= 8) ? active_set_[h - 1] : 0; }
void StarSteamInput::ActivateActionSetLayer(InputHandle_t h, InputActionSetHandle_t a) { if (h >= 1 && h <= 8) active_set_[h - 1] = a; }
void StarSteamInput::DeactivateActionSetLayer(InputHandle_t h, InputActionSetHandle_t a) { STAR_UNREFERENCED(a); if (h >= 1 && h <= 8) active_set_[h - 1] = 0; }
void StarSteamInput::DeactivateAllActionSetLayers(InputHandle_t h) { if (h >= 1 && h <= 8) active_set_[h - 1] = 0; }
int StarSteamInput::GetActiveActionSetLayers(InputHandle_t h, InputActionSetHandle_t* out) { if (h >= 1 && h <= 8 && active_set_[h - 1] && out) { out[0] = active_set_[h - 1]; return 1; } return 0; }
InputDigitalActionHandle_t StarSteamInput::GetDigitalActionHandle(const char* n) { return (InputDigitalActionHandle_t)action_handle(n); }
InputDigitalActionData_t StarSteamInput::GetDigitalActionData(InputHandle_t h, InputDigitalActionHandle_t d)
{
    InputDigitalActionData_t r{};
    if (!valid_pad(h)) return r;
    const std::string* an = action_name((uint64_t)d);
    if (!an) return r;
    const ActionSetBind* b = bindings_for(h, an->c_str(), false);
    if (!b) return r;
    auto f = b->digital.find(*an);
    if (f == b->digital.end()) return r;
    r.bActive = true;
    for (auto& btn : f->second.buttons) {
        if (eval_button((int)(h - 1), btn)) { r.bState = true; break; }
    }
    return r;
}
int StarSteamInput::GetDigitalActionOrigins(InputHandle_t h, InputActionSetHandle_t a, InputDigitalActionHandle_t d, EInputActionOrigin* o)
{
    STAR_UNREFERENCED(a);
    if (!valid_pad(h) || !o) return 0;
    const std::string* an = action_name((uint64_t)d);
    if (!an) return 0;
    const ActionSetBind* b = bindings_for(h, an->c_str(), false);
    if (!b) return 0;
    auto f = b->digital.find(*an);
    if (f == b->digital.end()) return 0;
    bool sony = pads_[h - 1].sony;
    int n = 0;
    for (auto& btn : f->second.buttons) {
        EInputActionOrigin org = button_origin(btn, sony);
        if (org == k_EInputActionOrigin_None) {
            if (btn.compare(0, 5, "DLJOY") == 0) org = sony ? k_EInputActionOrigin_PS4_LeftStick_Move : k_EInputActionOrigin_XBox360_LeftStick_Move;
            else if (btn.compare(0, 5, "DRJOY") == 0) org = sony ? k_EInputActionOrigin_PS4_RightStick_Move : k_EInputActionOrigin_XBox360_RightStick_Move;
            else if (btn == "DLTRIGGER") org = sony ? k_EInputActionOrigin_PS4_LeftTrigger_Pull : k_EInputActionOrigin_XBox360_LeftTrigger_Pull;
            else if (btn == "DRTRIGGER") org = sony ? k_EInputActionOrigin_PS4_RightTrigger_Pull : k_EInputActionOrigin_XBox360_RightTrigger_Pull;
            else continue;
        }
        if (n < 8) o[n++] = org;
    }
    return n;
}
const char* StarSteamInput::GetStringForDigitalActionName(InputDigitalActionHandle_t h) { auto it = handle_names_.find((uint64_t)h); return it == handle_names_.end() ? "" : it->second.c_str(); }
InputAnalogActionHandle_t StarSteamInput::GetAnalogActionHandle(const char* n) { return (InputAnalogActionHandle_t)action_handle(n); }
InputAnalogActionData_t StarSteamInput::GetAnalogActionData(InputHandle_t h, InputAnalogActionHandle_t a)
{
    InputAnalogActionData_t r{};
    if (!valid_pad(h)) return r;
    const std::string* an = action_name((uint64_t)a);
    if (!an) return r;
    const ActionSetBind* b = bindings_for(h, an->c_str(), true);
    if (!b) return r;
    auto f = b->analog.find(*an);
    if (f == b->analog.end()) return r;
    r.bActive = true;
    eval_stick((int)(h - 1), f->second.source, r.x, r.y);
    return r;
}
int StarSteamInput::GetAnalogActionOrigins(InputHandle_t h, InputActionSetHandle_t a, InputAnalogActionHandle_t an, EInputActionOrigin* o) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(a); STAR_UNREFERENCED(an); STAR_UNREFERENCED(o); return 0; }
const char* StarSteamInput::GetGlyphPNGForActionOrigin(EInputActionOrigin e, ESteamInputGlyphSize s, uint32 f) { STAR_UNREFERENCED(e); STAR_UNREFERENCED(s); STAR_UNREFERENCED(f); return ""; }
const char* StarSteamInput::GetGlyphSVGForActionOrigin(EInputActionOrigin e, uint32 f) { STAR_UNREFERENCED(e); STAR_UNREFERENCED(f); return ""; }
const char* StarSteamInput::GetGlyphForActionOrigin_Legacy(EInputActionOrigin e) { STAR_UNREFERENCED(e); return ""; }
const char* StarSteamInput::GetStringForActionOrigin(EInputActionOrigin e) { STAR_UNREFERENCED(e); return ""; }
const char* StarSteamInput::GetStringForAnalogActionName(InputAnalogActionHandle_t h) { auto it = handle_names_.find((uint64_t)h); return it == handle_names_.end() ? "" : it->second.c_str(); }
void StarSteamInput::StopAnalogActionMomentum(InputHandle_t h, InputAnalogActionHandle_t a) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(a); }
InputMotionData_t StarSteamInput::GetMotionData(InputHandle_t h) { STAR_UNREFERENCED(h); InputMotionData_t r{}; return r; }
void StarSteamInput::TriggerVibration(InputHandle_t h, unsigned short l, unsigned short r)
{
    if (!valid_pad(h)) return;
    int idx = (int)(h - 1);
    if (idx < 4) {
        if (!g_xinput.SetState) return;
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = l;
        v.wRightMotorSpeed = r;
        g_xinput.SetState((DWORD)idx, &v);
    } else {
        rumble_di(idx - 4, l, r);
    }
}
void StarSteamInput::TriggerVibrationExtended(InputHandle_t h, unsigned short l, unsigned short r, unsigned short lt, unsigned short rt)
{
    STAR_UNREFERENCED(lt); STAR_UNREFERENCED(rt);
    TriggerVibration(h, l, r);
}
void StarSteamInput::TriggerSimpleHapticEvent(InputHandle_t h, EControllerHapticLocation loc, uint8 i, char g, uint8 oi, char og) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(loc); STAR_UNREFERENCED(i); STAR_UNREFERENCED(g); STAR_UNREFERENCED(oi); STAR_UNREFERENCED(og); }
void StarSteamInput::SetLEDColor(InputHandle_t h, uint8 r, uint8 g, uint8 b, unsigned int f) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(r); STAR_UNREFERENCED(g); STAR_UNREFERENCED(b); STAR_UNREFERENCED(f); }
void StarSteamInput::Legacy_TriggerHapticPulse(InputHandle_t h, ESteamControllerPad p, unsigned short d) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(p); STAR_UNREFERENCED(d); }
void StarSteamInput::Legacy_TriggerRepeatedHapticPulse(InputHandle_t h, ESteamControllerPad p, unsigned short d, unsigned short o, unsigned short r, unsigned int f) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(p); STAR_UNREFERENCED(d); STAR_UNREFERENCED(o); STAR_UNREFERENCED(r); STAR_UNREFERENCED(f); }
bool StarSteamInput::ShowBindingPanel(InputHandle_t h) { STAR_UNREFERENCED(h); return false; }
ESteamInputType StarSteamInput::GetInputTypeForHandle(InputHandle_t h)
{
    if (!valid_pad(h)) return k_ESteamInputType_Unknown;
    const PadState& p = pads_[h - 1];
    if (p.sony) return p.ps5 ? k_ESteamInputType_PS5Controller : k_ESteamInputType_PS4Controller;
    return k_ESteamInputType_XBox360Controller;
}
InputHandle_t StarSteamInput::GetControllerForGamepadIndex(int n) { poll_pads(); return (n >= 0 && n < 8 && pads_[n].connected) ? (InputHandle_t)(n + 1) : 0; }
int StarSteamInput::GetGamepadIndexForController(InputHandle_t h) { return valid_pad(h) ? (int)(h - 1) : -1; }
const char* StarSteamInput::GetStringForXboxOrigin(EXboxOrigin e)
{
    static const struct { EXboxOrigin o; const char* s; } kNames[] = {
        { k_EXboxOrigin_A, "A" }, { k_EXboxOrigin_B, "B" },
        { k_EXboxOrigin_X, "X" }, { k_EXboxOrigin_Y, "Y" },
        { k_EXboxOrigin_LeftBumper, "Left Bumper" }, { k_EXboxOrigin_RightBumper, "Right Bumper" },
        { k_EXboxOrigin_Menu, "Menu" }, { k_EXboxOrigin_View, "View" },
        { k_EXboxOrigin_LeftTrigger_Pull, "Left Trigger" }, { k_EXboxOrigin_RightTrigger_Pull, "Right Trigger" },
        { k_EXboxOrigin_LeftStick_Move, "Left Stick" }, { k_EXboxOrigin_RightStick_Move, "Right Stick" },
        { k_EXboxOrigin_LeftStick_Click, "Left Stick Click" }, { k_EXboxOrigin_RightStick_Click, "Right Stick Click" },
        { k_EXboxOrigin_DPad_North, "D-Pad Up" }, { k_EXboxOrigin_DPad_East, "D-Pad Right" },
        { k_EXboxOrigin_DPad_South, "D-Pad Down" }, { k_EXboxOrigin_DPad_West, "D-Pad Left" },
    };
    for (auto& k : kNames)
        if (k.o == e) return k.s;
    return "";
}
const char* StarSteamInput::GetGlyphForXboxOrigin(EXboxOrigin e) { STAR_UNREFERENCED(e); return ""; }
EInputActionOrigin StarSteamInput::GetActionOriginFromXboxOrigin(InputHandle_t h, EXboxOrigin e) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(e); return k_EInputActionOrigin_None; }
EInputActionOrigin StarSteamInput::TranslateActionOrigin(ESteamInputType t, EInputActionOrigin e) { STAR_UNREFERENCED(t); STAR_UNREFERENCED(e); return k_EInputActionOrigin_None; }
bool StarSteamInput::GetDeviceBindingRevision(InputHandle_t h, int* maj, int* min) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(maj); STAR_UNREFERENCED(min); return false; }
uint32 StarSteamInput::GetRemotePlaySessionID(InputHandle_t h) { STAR_UNREFERENCED(h); return 0; }
uint16 StarSteamInput::GetSessionInputConfigurationSettings() { return 0; }
void StarSteamInput::SetDualSenseTriggerEffect(InputHandle_t h, const ScePadTriggerEffectParam* p) { STAR_UNREFERENCED(h); STAR_UNREFERENCED(p); }
