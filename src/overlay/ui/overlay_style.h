#pragma once
#include "imgui.h"
#include "overlay/core/overlay_util.h"
#include <cstdint>

// Owns accent color, UI scale, and the loaded fonts, plus the one-time ImGui
// style setup. Resolved from settings whenever the overlay (re)initializes.
class OverlayStyle {
public:
    void setup();
    void resolve_accent();

    ImU32  acc(float a) const { return col(r_, g_, b_, a); }
    ImVec4 vacc(float a) const { return v4(r_, g_, b_, a); }

    float   scale() const { return scale_; }
    ImFont* small_font() const { return small_; }
    ImFont* title_font() const { return title_; }

private:
    float   scale_ = 1.0f;
    uint8_t r_ = 0x4f, g_ = 0xa3, b_ = 0xff; // == kOverlayAccents[0] (blue); see overlay_util.h
    ImFont* small_ = nullptr;
    ImFont* title_ = nullptr;
};
