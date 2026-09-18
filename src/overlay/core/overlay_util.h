#pragma once
// Shared overlay drawing/persistence helpers. Previously file-local statics in
// overlay.cpp; extracted so the split overlay translation units can share them.

#include "imgui.h"
#include <cstdint>
#include <string>

// Panel palette (byte triples consumed by col()/v4()).
#define P_BG0    0x1a,0x1a,0x1a
#define P_BG1    0x22,0x22,0x22
#define P_BG2    0x2a,0x2a,0x2a
#define P_SEP    0x35,0x35,0x35
#define P_TXT    0xf2,0xf2,0xf2
#define P_MUT    0x80,0x80,0x80
#define P_LGT    0x9e,0x9e,0x9e
#define P_DIM    0x4a,0x4a,0x4a
#define P_GRN    0x4c,0xb8,0x4c

float clamp01(float v);
float easeOut(float t);

// 5-point star (procedural trophy for the all-complete toast).
void draw_star(ImDrawList* dl, ImVec2 c, float r_out, float r_in, ImU32 col);

// Unlock timestamp as compact "dd.mm HH:MM" (fits under the row button).
std::string fmt_unlock_time(uint32_t t);

// Word-wrap text into at most two lines fitting max_w (measured at the given
// font size). Overlong remainder is ellipsized onto line 2. Returns 0/1/2.
int wrap_two_lines(ImFont* font, float size, const char* text, float max_w,
                   std::string& l1, std::string& l2);

ImU32 col(uint8_t r, uint8_t g, uint8_t b, float a = 1.f);
ImVec4 v4(uint8_t r, uint8_t g, uint8_t b, float a = 1.f);

// Persist one key in overlay.star (keeps art header/comments, replaces or appends).
void save_overlay_key(const std::string& key, const std::string& value);
