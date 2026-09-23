#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "../src/overlay/backends/gdi_resources.h"

int main()
{
    // Warm up GDI before checking that resize/reset/destruction release handles.
    { star_gdi::DibSurface warmup; assert(warmup.create(1, 1)); }
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    {
        star_gdi::DibSurface pixels;
        for (int size = 1; size <= 32; ++size) {
            assert(pixels.create(size, size + 1));
            DIBSECTION dib{};
            assert(GetObject(GetCurrentObject(pixels.dc(), OBJ_BITMAP), sizeof(dib), &dib));
            assert(dib.dsBm.bmWidth == size && dib.dsBm.bmHeight == size + 1);
            assert(dib.dsBm.bmBitsPixel == 32);
            assert(dib.dsBm.bmBits == pixels.bits());
            assert(SetPixelV(pixels.dc(), 0, 0, RGB(10, 20, 30)));
            GdiFlush();
            const auto* first = static_cast<const unsigned char*>(pixels.bits());
            assert(first[0] == 30 && first[1] == 20 && first[2] == 10);
        }
        assert(!pixels.create(0, 10));
        assert(!pixels.dc() && !pixels.bits());
        assert(!pixels.create(16385, 1));
        assert(pixels.create(2, 2));
        pixels.reset();
        pixels.reset();
        assert(!pixels.dc() && !pixels.bits());
        assert(pixels.create(4, 4));
    }
    GdiFlush();
    assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before);
}
