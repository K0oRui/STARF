#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include "../src/overlay/core/pixel_copy.h"

int main()
{
    // Two rows, two pixels per row, and four bytes of source padding.
    const std::array<uint8_t, 24> source{
        1, 2, 3, 0, 4, 5, 6, 255, 90, 90, 90, 90,
        7, 8, 9, 42, 10, 11, 12, 128, 90, 90, 90, 90
    };
    const std::array<uint8_t, 16> rgba{1, 2, 3, 0, 4, 5, 6, 255, 7, 8, 9, 42, 10, 11, 12, 128};
    const std::array<uint8_t, 16> bgra{3, 2, 1, 0, 6, 5, 4, 255, 9, 8, 7, 42, 12, 11, 10, 128};
    std::array<uint8_t, 16> packed{};
    copy_pixels32(packed.data(), 8, source.data(), 12, 2, 2, false);
    assert(packed == rgba);
    copy_pixels32(packed.data(), 8, source.data(), 12, 2, 2, true);
    assert(packed == bgra);

    // Upload into padded destination rows without overwriting padding.
    std::array<uint8_t, 24> uploaded;
    uploaded.fill(90);
    copy_pixels32(uploaded.data(), 12, packed.data(), 8, 2, 2, true);
    assert(uploaded == source);
    copy_pixels32(uploaded.data(), 12, rgba.data(), 8, 2, 2, false);
    assert(uploaded == source);
}
