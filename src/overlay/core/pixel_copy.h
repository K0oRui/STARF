#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

// Straight-alpha -> premultiplied-alpha, in place. The renderers output
// straight alpha, but UpdateLayeredWindow (ULW_ALPHA) and GDI AlphaBlend
// both expect premultiplied sources; feeding straight pixels makes every
// translucent texel too bright (washed toasts, glowing text fringes).
// Opaque pixels are untouched, so fully-opaque UIs are bit-identical.
inline void premultiply_inplace(uint8_t* bits, size_t width, size_t height)
{
    for (size_t i = 0, n = width * height; i < n; ++i, bits += 4) {
        unsigned a = bits[3];
        if (a == 255 || a == 0) {
            if (a == 0) bits[0] = bits[1] = bits[2] = 0;
            continue;
        }
        bits[0] = (uint8_t)(bits[0] * a / 255);
        bits[1] = (uint8_t)(bits[1] * a / 255);
        bits[2] = (uint8_t)(bits[2] * a / 255);
    }
}
// Copies non-overlapping 32-bit pixel buffers, preserving alpha and row padding.
inline void copy_pixels32(void* destination, size_t destination_pitch,
                          const void* source, size_t source_pitch,
                          size_t width, size_t height, bool swap_red_blue)
{
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);
    for (size_t y = 0; y < height; ++y) {
        auto* row = dst + y * destination_pitch;
        std::memcpy(row, src + y * source_pitch, width * 4);
        if (swap_red_blue)
            for (size_t x = 0; x < width; ++x)
                std::swap(row[x * 4], row[x * 4 + 2]);
    }
}
