#pragma once
// Shared overlay drawing/persistence helpers. Previously file-local statics in
// overlay.cpp; extracted so the split overlay translation units can share them.

#include "imgui.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

// Panel palette (byte triples consumed by col()/v4()).
// Single source of truth for every overlay color — use these, never raw
// hex or IM_COL32 literals, so the overlay stays constant.
#define P_BG0    0x1a,0x1a,0x1a // window / toast / HUD pill
#define P_BG1    0x22,0x22,0x22 // child / popup / input field
#define P_BG2    0x2a,0x2a,0x2a // button / frame / thumb placeholder
#define P_SEP    0x35,0x35,0x35 // border / separator / progress track
#define P_TXT    0xf2,0xf2,0xf2 // primary text / scrim label
#define P_MUT    0x80,0x80,0x80 // secondary text / section headers
#define P_DIM    0x4a,0x4a,0x4a // hints / scrollbar grab / hidden bars
// Neutral interaction states (were raw 0x30/0x32/0x3a literals).
#define P_HOV    0x30,0x30,0x30 // button/frame hover
#define P_ACT    0x3a,0x3a,0x3a // button active
// Destructive (unified: 0x44,0x20,0x20 outlier -> P_DNG_ACT).
#define P_DNG    0x38,0x1a,0x1a // delete / reset button
#define P_DNG_HOV 0x55,0x28,0x28 // delete / reset hover
#define P_DNG_ACT 0x66,0x30,0x30 // delete / reset active
// Specials (were raw IM_COL32 literals).
#define P_GLD    0xff,0xcd,0x46 // summary toast gold (255,205,70)
#define P_WHT    0xff,0xff,0xff // icon tint / viewer image / scrim label
#define P_BLK    0x00,0x00,0x00 // dim layer / thumb scrim
// Shape: corner rounding in px (values preserved; names group by role).
// ImGui style roundings in OverlayStyle::setup() use these too.
#define R_CARD   5.f // toast card + toast icon outline
#define R_ICON   4.f // toast/row icons + placeholders + swatch/row outlines + HUD pill + shot thumbs/scrim
#define R_SMALL  3.f // panel progress + row icons + hidden bar + corner outline
// Line: draw-list outline thickness (toast/corner/row/swatch outlines).
#define T_LINE   1.5f
// Alpha: opacity multipliers. Animation-driven `a` and `panel_anim_`
// compose at call sites (e.g. col(P_SEP, a * A_TRACK)).
#define A_BG       0.97f // window bg (style + SetNextWindowBgAlpha)
#define A_POPUP    0.98f // popup bg
#define A_TRACK    0.4f  // progress track bg, toast + panel (x fade alpha on toast)
#define A_BORDER   0.6f  // popup border
// Accent ramp: translucent button states up to full-strength marks.
#define A_ACC_BG   0.22f // accent-tinted button bg
#define A_ACC_HOV  0.3f  // accent-tinted button hover
#define A_ACC_ACT  0.35f // accent-tinted button active
#define A_ACC_LINE 0.6f  // toast/row icon outlines
#define A_ACC_EMPH 0.7f  // toast fill + corner active outline
// Accent table: single source of truth shared by resolve_accent() and the
// DISPLAY swatch row. resolve_accent() falls back to entry 0 (blue).
struct OverlayAccent { const char* name; uint8_t r, g, b; };
inline constexpr OverlayAccent kOverlayAccents[] = {
    { "blue",   0x4f, 0xa3, 0xff },
    { "red",    0xff, 0x5a, 0x5a },
    { "green",  0x4c, 0xb8, 0x4c },
    { "purple", 0xb0, 0x7f, 0xff },
    { "orange", 0xff, 0xa0, 0x3c },
    { "yellow", 0xff, 0xd4, 0x4d },
};

float clamp01(float v);
float easeOut(float t);
float easeIn(float t);

// Real-time seconds (steady clock), independent of the game's present rate.
// Double precision: float loses ~1ms resolution after ~1h uptime, which
// makes high-FPS titles (classic DX7/DX8, uncapped) compute dt==0 every
// frame and fall back to a fixed step, expiring toasts many times too fast.
inline double now_seconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 5-point star (procedural trophy for the all-complete toast).
void draw_star(ImDrawList* dl, ImVec2 c, float r_out, float r_in, ImU32 col);

// Clipped text: PushClipRect/AddText/PopClipRect in one call. Pass l2 +
// line_h for a second line sharing the same clip rect.
inline void clip_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImVec2 max,
                      ImU32 color, const char* l1, const char* l2 = nullptr, float line_h = 0.f) {
    dl->PushClipRect(pos, max, true);
    dl->AddText(font, size, pos, color, l1);
    if (l2) dl->AddText(font, size, {pos.x, pos.y + line_h}, color, l2);
    dl->PopClipRect();
}

// File name without directories: "C:\a\b.png" -> "b.png".
inline std::string file_name(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// Unlock timestamp as "Sep 22, 2:35 PM" (12-hour, fits under row button).
std::string fmt_unlock_time(uint32_t t);

// Word-wrap text into at most two lines fitting max_w (measured at the given
// font size). Overlong remainder is ellipsized onto line 2. Returns 0/1/2.
int wrap_two_lines(ImFont* font, float size, const char* text, float max_w,
                   std::string& l1, std::string& l2);

ImU32 col(uint8_t r, uint8_t g, uint8_t b, float a = 1.f);
ImVec4 v4(uint8_t r, uint8_t g, uint8_t b, float a = 1.f);

// Edge strips confined to a rounded card: full-bleed bars whose outer ends
// follow the card's own corner arcs. ImDrawList can't clip rounded shapes,
// and a thin strip can't carry the card radius itself (it pinches), so the
// ends are built from the card's arcs. All shapes stay inside the convex
// card. Requires h <= r and card W/H > 2r. Angles are radians:
// 0 = +x, PI/2 = +y (down), PI = -x, 3PI/2 = up.
constexpr float kPi = 3.14159265f;
inline void card_bottom_wedge(ImDrawList* dl, ImVec2 c0, ImVec2 c1, float r, float h, ImU32 col, bool right)
{
    float eang = acosf((r - h) / r);
    float cx = right ? c1.x - r : c0.x + r;
    float a0 = right ? 0.5f * kPi - eang : 0.5f * kPi;
    float a1 = right ? 0.5f * kPi : 0.5f * kPi + eang;
    dl->PathClear();
    dl->PathArcTo({cx, c1.y - r}, r, a0, a1);
    dl->PathLineTo({cx, c1.y - h});
    dl->PathFillConvex(col);
}

// Persist one key in overlay.star (keeps art header/comments, replaces or appends).
void save_overlay_key(const std::string& key, const std::string& value);

// Tiny-framebuffer auto-migrate: below this size hook-drawn text has too
// few pixels and upscales to mush, so the overlay moves to the external
// window (native resolution) instead. Threshold is 640x480: the classics
// (640x480, 800x600) stay on hooks, only smaller frames (544x416, 512x384,
// 320x240, ...) migrate. Explicit mode=hook never migrates (see
// switch_to_external), and mode=external never uses hooks at all.
constexpr int kTinyFrameW = 640, kTinyFrameH = 480;
