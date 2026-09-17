#pragma once
#include "core/star_common.h"
#include "steam/isteaminput.h"
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

class StarSteamInput : public ISteamInput {
public:
    static StarSteamInput& get();

    bool Init(bool bExplicitlyCallRunFrame) override;
    bool Shutdown() override;
    bool SetInputActionManifestFilePath(const char* pchInputActionManifestAbsolutePath) override;
    void RunFrame(bool bReservedValue) override;
    bool BWaitForData(bool bWaitForever, uint32 unTimeout) override;
    bool BNewDataAvailable() override;
    int GetConnectedControllers(InputHandle_t* handlesOut) override;
    void EnableDeviceCallbacks() override;
    void EnableActionEventCallbacks(SteamInputActionEventCallbackPointer pCallback) override;
    InputActionSetHandle_t GetActionSetHandle(const char* pszActionSetName) override;
    void ActivateActionSet(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle) override;
    InputActionSetHandle_t GetCurrentActionSet(InputHandle_t inputHandle) override;
    void ActivateActionSetLayer(InputHandle_t inputHandle, InputActionSetHandle_t actionSetLayerHandle) override;
    void DeactivateActionSetLayer(InputHandle_t inputHandle, InputActionSetHandle_t actionSetLayerHandle) override;
    void DeactivateAllActionSetLayers(InputHandle_t inputHandle) override;
    int GetActiveActionSetLayers(InputHandle_t inputHandle, InputActionSetHandle_t* handlesOut) override;
    InputDigitalActionHandle_t GetDigitalActionHandle(const char* pszActionName) override;
    InputDigitalActionData_t GetDigitalActionData(InputHandle_t inputHandle, InputDigitalActionHandle_t digitalActionHandle) override;
    int GetDigitalActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputDigitalActionHandle_t digitalActionHandle, EInputActionOrigin* originsOut) override;
    const char* GetStringForDigitalActionName(InputDigitalActionHandle_t eActionHandle) override;
    InputAnalogActionHandle_t GetAnalogActionHandle(const char* pszActionName) override;
    InputAnalogActionData_t GetAnalogActionData(InputHandle_t inputHandle, InputAnalogActionHandle_t analogActionHandle) override;
    int GetAnalogActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle, InputAnalogActionHandle_t analogActionHandle, EInputActionOrigin* originsOut) override;
    const char* GetGlyphPNGForActionOrigin(EInputActionOrigin eOrigin, ESteamInputGlyphSize eSize, uint32 unFlags) override;
    const char* GetGlyphSVGForActionOrigin(EInputActionOrigin eOrigin, uint32 unFlags) override;
    const char* GetGlyphForActionOrigin_Legacy(EInputActionOrigin eOrigin) override;
    const char* GetStringForActionOrigin(EInputActionOrigin eOrigin) override;
    const char* GetStringForAnalogActionName(InputAnalogActionHandle_t eActionHandle) override;
    void StopAnalogActionMomentum(InputHandle_t inputHandle, InputAnalogActionHandle_t eAction) override;
    InputMotionData_t GetMotionData(InputHandle_t inputHandle) override;
    void TriggerVibration(InputHandle_t inputHandle, unsigned short usLeftSpeed, unsigned short usRightSpeed) override;
    void TriggerVibrationExtended(InputHandle_t inputHandle, unsigned short usLeftSpeed, unsigned short usRightSpeed, unsigned short usLeftTriggerSpeed, unsigned short usRightTriggerSpeed) override;
    void TriggerSimpleHapticEvent(InputHandle_t inputHandle, EControllerHapticLocation eHapticLocation, uint8 nIntensity, char nGainDB, uint8 nOtherIntensity, char nOtherGainDB) override;
    void SetLEDColor(InputHandle_t inputHandle, uint8 nColorR, uint8 nColorG, uint8 nColorB, unsigned int nFlags) override;
    void Legacy_TriggerHapticPulse(InputHandle_t inputHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec) override;
    void Legacy_TriggerRepeatedHapticPulse(InputHandle_t inputHandle, ESteamControllerPad eTargetPad, unsigned short usDurationMicroSec, unsigned short usOffMicroSec, unsigned short unRepeat, unsigned int nFlags) override;
    bool ShowBindingPanel(InputHandle_t inputHandle) override;
    ESteamInputType GetInputTypeForHandle(InputHandle_t inputHandle) override;
    InputHandle_t GetControllerForGamepadIndex(int nIndex) override;
    int GetGamepadIndexForController(InputHandle_t ulinputHandle) override;
    const char* GetStringForXboxOrigin(EXboxOrigin eOrigin) override;
    const char* GetGlyphForXboxOrigin(EXboxOrigin eOrigin) override;
    EInputActionOrigin GetActionOriginFromXboxOrigin(InputHandle_t inputHandle, EXboxOrigin eOrigin) override;
    EInputActionOrigin TranslateActionOrigin(ESteamInputType eDestinationInputType, EInputActionOrigin eSourceOrigin) override;
    bool GetDeviceBindingRevision(InputHandle_t inputHandle, int* pMajor, int* pMinor) override;
    uint32 GetRemotePlaySessionID(InputHandle_t inputHandle) override;
    uint16 GetSessionInputConfigurationSettings() override;
    void SetDualSenseTriggerEffect(InputHandle_t inputHandle, const ScePadTriggerEffectParam* pParam) override;

    // Unified pad state, shared with the free mapping helper in the .cpp.
    struct PadState {
        bool connected = false;
        bool sony = false;
        bool ps5 = false;
        WORD buttons = 0;
        BYTE lt = 0, rt = 0;
        SHORT lx = 0, ly = 0, rx = 0, ry = 0;
    };
    static BOOL CALLBACK di_enum_cb(const DIDEVICEINSTANCEA* d, void* ctx);

private:
    StarSteamInput() = default;

    // XInput bridge: real pads surface as one Steam handle each (index+1).
    // Action bindings come from STAR/controller/<ACTION_SET>.txt files
    // (Goldberg format: ACTION=BUTTON[,BUTTON] or ACTION=ANALOG=MODE).
    struct DigitalBind { std::vector<std::string> buttons; };
    struct AnalogBind { std::string source; std::string mode; };
    struct ActionSetBind {
        std::map<std::string, DigitalBind> digital;
        std::map<std::string, AnalogBind> analog;
    };
    // Unified pads: slots 0-3 XInput, 4-7 DirectInput (Sony BT pads).
    // Buttons use XINPUT_GAMEPAD_* bits whatever the source, so binding
    // evaluation never cares where the pad came from.
    PadState pads_[8]{};
    InputActionSetHandle_t active_set_[8]{};
    std::map<uint64_t, std::string> handle_names_; // action/set handle -> name
    std::map<std::string, ActionSetBind> set_cache_;
    bool sets_scanned_ = false;
    void poll_pads();
    bool valid_pad(InputHandle_t h) const;
    uint64_t action_handle(const char* n);
    const std::string* action_name(uint64_t h) const;
    const char* register_name(uint64_t h, const char* n);
    const ActionSetBind* bindings_for(InputHandle_t h, const char* action, bool analog);
    bool eval_button(int pad, const std::string& btn);
    void eval_stick(int pad, const std::string& src, float& x, float& y);
    // DirectInput (non-XInput pads: DualSense/DS4 over Bluetooth).
    // Only Sony layouts are claimed; anything else is logged and skipped.
    IDirectInput8A* di_ = nullptr;
    IDirectInputDevice8A* di_dev_[4] = {};
    bool di_sony_[4]{}, di_ps5_[4]{}, di_dead_[4]{};
    LONG di_rest_[4][6]{};
    bool di_rest_ok_[4]{};
    LPDIRECTINPUTEFFECT di_fx_[4]{};
    bool di_enum_done_ = false;
    void poll_di();
    void rumble_di(int idx, unsigned short l, unsigned short r);
};
