// Hardware-free smoke test for StarSteamInput (ISteamInput).
//
// A fake xinput1_4.dll (tests/fake_xinput.cpp) sits next to this exe, so the
// loader picks it over the system one and pad state is fully scripted via
// FAKE_XINPUT_STATE / FAKE_XINPUT_RUMBLE files. Covers: no-pad contract,
// connection/index mapping, Goldberg-format bindings (digital + analog +
// triggers), deadzones, origins, rumble capture, and no-crash stubs.
//
// Fixture layout under %TEMP%\star-controller-test-<pid>:
//   STAR\Controller\TestSet.txt   (bindings; settings_dir points at STAR\)
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/settings.h"
#include "steam/steam_input.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

// Logging links from core/logging.cpp; stubbed here so the test links only
// steam_input.cpp (same pattern as async_io_smoke's Settings::get stub).
void STAR_WriteLog(const char*, ...) {}
void STAR_WriteLogLevel(StarLogLevel, const char*, ...) {}
void STAR_WriteTraceLog(const char*, ...) {}

Settings& Settings::get() { static Settings settings; return settings; }

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

// XInput button bits (matches XINPUT_GAMEPAD_* values).
static const uint16_t BTN_A = 0x1000, BTN_X = 0x4000, BTN_Y = 0x8000;

#pragma pack(push, 1)
struct FakePad {
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
#pragma pack(pop)

static std::string g_state_path, g_rumble_path;

static void write_state(uint32_t mask, const FakePad pads[4])
{
    FILE* f = nullptr;
    if (fopen_s(&f, g_state_path.c_str(), "wb") != 0 || !f) {
        fprintf(stderr, "FAIL: cannot write state file\n");
        failures++;
        return;
    }
    fwrite(&mask, sizeof(mask), 1, f);
    fwrite(pads, sizeof(FakePad), 4, f);
    fclose(f);
}

struct RumbleRec { uint32_t idx; uint16_t left, right; };

static std::vector<RumbleRec> read_rumble()
{
    std::vector<RumbleRec> out;
    FILE* f = nullptr;
    if (fopen_s(&f, g_rumble_path.c_str(), "rb") != 0 || !f)
        return out;
    RumbleRec r;
    while (fread(&r, sizeof(r), 1, f) == 1)
        out.push_back(r);
    fclose(f);
    return out;
}



int main()
{
    char tmp[MAX_PATH] = {};
    GetTempPathA(sizeof(tmp), tmp);
    std::string root = std::string(tmp) + "star-controller-test-" + std::to_string(GetCurrentProcessId());
    std::string star_dir = root + "\\STAR";
    std::string ctrl_dir = star_dir + "\\Controller";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(star_dir.c_str(), nullptr);
    CreateDirectoryA(ctrl_dir.c_str(), nullptr);

    {
        FILE* f = nullptr;
        const char* text =
            "# Goldberg-format bindings for controller_smoke\n"
            "JUMP=A\n"
            "RELOAD=X\n"
            "CROUCH=Y\n"
            "FIRE=A,B\n"
            "BOOST=DLTRIGGER\n"
            "MOVE=LJOY=JOYSTICK\n"
            "AIM=RJOY=JOYSTICK\n";
        std::string path = ctrl_dir + "\\TestSet.txt";
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
            fprintf(stderr, "FAIL: cannot write %s\n", path.c_str());
            return 1;
        }
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    g_state_path = root + "\\padstate.bin";
    g_rumble_path = root + "\\rumble.bin";
    SetEnvironmentVariableA("FAKE_XINPUT_STATE", g_state_path.c_str());
    SetEnvironmentVariableA("FAKE_XINPUT_RUMBLE", g_rumble_path.c_str());

    Settings::get().settings_dir = star_dir;
    StarSteamInput& in = StarSteamInput::get();

    // ---- Phase A: no pads connected ----
    {
        FakePad pads[4] = {};
        write_state(0, pads);
        CHECK(in.Init(false) == true);
        CHECK(in.GetControllerForGamepadIndex(0) == 0);
        CHECK(in.GetGamepadIndexForController(1) == -1);
        CHECK(in.GetInputTypeForHandle(1) == k_ESteamInputType_Unknown);
        InputDigitalActionHandle_t jump = in.GetDigitalActionHandle("JUMP");
        InputDigitalActionData_t dd = in.GetDigitalActionData(1, jump);
        CHECK(dd.bActive == false && dd.bState == false);
        InputAnalogActionHandle_t move = in.GetAnalogActionHandle("MOVE");
        InputAnalogActionData_t ad = in.GetAnalogActionData(1, move);
        CHECK(ad.bActive == false && ad.x == 0.0f && ad.y == 0.0f);
        EInputActionOrigin origins[8] = {};
        CHECK(in.GetDigitalActionOrigins(1, 0, jump, origins) == 0);
        in.TriggerVibration(1, 1000, 1000); // disconnected: must not crash
        CHECK(read_rumble().empty());
    }

    // ---- Phase B: scripted pad on XInput slot 0 ----
    {
        FakePad pads[4] = {};
        pads[0].packet = 7;
        pads[0].buttons = (uint16_t)(BTN_A | BTN_X);
        pads[0].lt = 200;
        pads[0].lx = 30000;
        pads[0].ly = -30000;
        pads[0].rx = 1000; // inside deadzone -> reads 0
        write_state(0x1, pads);
        in.RunFrame(false);

        InputHandle_t handles[8] = {};
        int n = in.GetConnectedControllers(handles);
        CHECK(n >= 1 && handles[0] == 1);
        CHECK(in.GetControllerForGamepadIndex(0) == 1);
        CHECK(in.GetGamepadIndexForController(1) == 0);
        CHECK(in.GetGamepadIndexForController(99) == -1);
        CHECK(in.GetInputTypeForHandle(1) == k_ESteamInputType_XBox360Controller);
        CHECK(in.BNewDataAvailable() == true);

        InputActionSetHandle_t set = in.GetActionSetHandle("TestSet");
        CHECK(set != 0);
        in.ActivateActionSet(1, set);
        CHECK(in.GetCurrentActionSet(1) == set);

        const struct { const char* name; bool active, state; } dig[] = {
            {"JUMP", true, true}, // A pressed
            {"RELOAD", true, true}, // X pressed
            {"CROUCH", true, false}, // Y not pressed
            {"FIRE", true, true}, // A,B OR-list
            {"BOOST", true, true}, // LT 200 > 64
            {"MISSING", false, false},
        };
        for (auto& t : dig) {
            InputDigitalActionData_t d =
                in.GetDigitalActionData(1, in.GetDigitalActionHandle(t.name));
            CHECK(d.bActive == t.active && d.bState == t.state);
        }
        CHECK(in.GetDigitalActionData(99, in.GetDigitalActionHandle("JUMP")).bActive == false);
        CHECK(in.GetDigitalActionData(0, in.GetDigitalActionHandle("JUMP")).bActive == false);

        const struct { const char* name; bool active; float xmin, xmax, ymin, ymax; } ana[] = {
            {"MOVE", true, 0.5f, 1.0f, -1.0f, -0.5f},
            {"AIM", true, 0.0f, 0.0f, 0.0f, 0.0f}, // rx=1000 inside deadzone
            {"MISSING", false, 0.0f, 0.0f, 0.0f, 0.0f},
        };
        for (auto& t : ana) {
            InputAnalogActionData_t a =
                in.GetAnalogActionData(1, in.GetAnalogActionHandle(t.name));
            CHECK(a.bActive == t.active && a.x >= t.xmin && a.x <= t.xmax &&
                  a.y >= t.ymin && a.y <= t.ymax);
        }
        CHECK(in.GetAnalogActionData(99, in.GetAnalogActionHandle("MOVE")).bActive == false);

        EInputActionOrigin origins[8] = {};
        int no = in.GetDigitalActionOrigins(1, set, in.GetDigitalActionHandle("JUMP"), origins);
        CHECK(no >= 1 && origins[0] == k_EInputActionOrigin_XBox360_A);
        CHECK(strcmp(in.GetStringForDigitalActionName(in.GetDigitalActionHandle("JUMP")), "JUMP") == 0);
        CHECK(strcmp(in.GetStringForAnalogActionName(in.GetAnalogActionHandle("MOVE")), "MOVE") == 0);

        { FILE* f = nullptr; fopen_s(&f, g_rumble_path.c_str(), "wb"); if (f) fclose(f); }
        in.TriggerVibration(1, 1000, 2000);
        std::vector<RumbleRec> r = read_rumble();
        CHECK(r.size() == 1 && r[0].idx == 0 && r[0].left == 1000 && r[0].right == 2000);
        in.TriggerVibration(99, 3000, 4000); // invalid handle: no-op
        CHECK(read_rumble().size() == 1);

        // Stub surface: must not crash, must report unsupported.
        in.TriggerSimpleHapticEvent(1, k_EControllerHapticLocation_Left, 100, 0, 100, 0);
        in.Legacy_TriggerHapticPulse(1, k_ESteamControllerPad_Left, 100);
        in.Legacy_TriggerRepeatedHapticPulse(1, k_ESteamControllerPad_Left, 100, 50, 3, 0);
        in.SetLEDColor(1, 255, 0, 0, 0);
        in.StopAnalogActionMomentum(1, in.GetAnalogActionHandle("MOVE"));
        CHECK(in.ShowBindingPanel(1) == false);
        CHECK(in.GetDeviceBindingRevision(1, nullptr, nullptr) == false);
        InputMotionData_t m = in.GetMotionData(1);
        CHECK(m.rotQuatX == 0.0f && m.rotQuatY == 0.0f && m.rotQuatZ == 0.0f && m.rotQuatW == 0.0f);
        CHECK(in.TranslateActionOrigin(k_ESteamInputType_XBox360Controller,
                                        k_EInputActionOrigin_XBox360_A) == k_EInputActionOrigin_None);
        CHECK(in.GetSessionInputConfigurationSettings() == 0);
        CHECK(in.GetAnalogActionOrigins(1, set, in.GetAnalogActionHandle("MOVE"), origins) == 0);
    }

    CHECK(in.Shutdown() == true);

    if (failures == 0)
        printf("controller_smoke: all checks passed\n");
    else
        fprintf(stderr, "controller_smoke: %d checks FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
