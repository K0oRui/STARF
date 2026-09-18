#pragma once
#include "imgui.h"
#include <cstdint>

// Owns accent color, UI scale, and the loaded fonts, plus the one-time ImGui
// style setup. Resolved from settings whenever the overlay (re)initializes.
class OverlayStyle {
public:
    void setup();
    void resolve_accent();

    ImU32  acc(float a) const {
        return IM_COL32(r_, g_, b_, (int)(a * 255.f + .5f));
    }
    ImVec4 vacc(float a) const {
        return { r_ / 255.f, g_ / 255.f, b_ / 255.f, a };
    }

    float   scale() const { return scale_; }
    ImFont* small_font() const { return small_; }
    ImFont* title_font() const { return title_; }

private:
    float   scale_ = 1.0f;
    uint8_t r_ = 0x4f, g_ = 0xa3, b_ = 0xff;
    ImFont* small_ = nullptr;
    ImFont* title_ = nullptr;
};
