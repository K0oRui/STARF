// Fake XInput provider for controller_smoke.exe (hardware-free testing).
//
// STAR loads "xinput1_4.dll" by name, and the loader searches the application
// directory first, so dropping this DLL next to the test exe shadows the real
// one for the test process only. Pad state comes from a file whose path is in
// FAKE_XINPUT_STATE:
//
//   u32 present_mask; u8 pads[4][16];
//
// Each 16-byte pad: u32 packet; u16 buttons; u8 lt, rt; i16 lx, ly, rx, ry.
// Rumble requests append u32 index + u16 left + u16 right records to the file
// in FAKE_XINPUT_RUMBLE.
#include <windows.h>
#include <cstdint>
#include <cstdio>

struct FakePad {
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
static_assert(sizeof(FakePad) == 16, "pad layout");

static bool env_path(const char* var, char* path)
{
    return GetEnvironmentVariableA(var, path, MAX_PATH) != 0 && path[0] != 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI XInputGetState(DWORD dwUserIndex, void* pState)
{
    if (dwUserIndex >= 4 || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    char path[MAX_PATH] = {};
    if (!env_path("FAKE_XINPUT_STATE", path))
        return ERROR_DEVICE_NOT_CONNECTED;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return ERROR_DEVICE_NOT_CONNECTED;
    uint32_t mask = 0;
    FakePad pads[4] = {};
    bool ok = fread(&mask, sizeof(mask), 1, f) == 1 &&
              fread(pads, sizeof(pads), 1, f) == 1;
    fclose(f);
    if (!ok || !(mask & (1u << dwUserIndex)))
        return ERROR_DEVICE_NOT_CONNECTED;
    memcpy(pState, &pads[dwUserIndex], sizeof(FakePad));
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration)
{
    if (dwUserIndex >= 4)
        return ERROR_DEVICE_NOT_CONNECTED;
    char path[MAX_PATH] = {};
    if (env_path("FAKE_XINPUT_RUMBLE", path)) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "ab") == 0 && f) {
            uint32_t idx = dwUserIndex;
            uint16_t lr[2] = {};
            if (pVibration)
                memcpy(lr, (const uint8_t*)pVibration, sizeof(lr)); // wLeftMotorSpeed, wRightMotorSpeed
            fwrite(&idx, sizeof(idx), 1, f);
            fwrite(lr, sizeof(lr), 1, f);
            fclose(f);
        }
    }
    return ERROR_SUCCESS;
}
