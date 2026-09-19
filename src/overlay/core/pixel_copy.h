#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

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
