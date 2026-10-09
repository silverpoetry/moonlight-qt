#include "nativecursor_win.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace {
bool readMsbBit(const std::uint8_t* plane, int pitch, int x, int y)
{
    return (plane[y * pitch + x / 8] & (0x80u >> (x % 8))) != 0;
}

void setMsbBit(std::vector<std::uint8_t>& plane, int pitch, int x, int y, bool value)
{
    const auto mask = static_cast<std::uint8_t>(0x80u >> (x % 8));
    auto& byte = plane[static_cast<std::size_t>(y * pitch + x / 8)];
    if (value) {
        byte |= mask;
    }
    else {
        byte &= static_cast<std::uint8_t>(~mask);
    }
}
}

void* WindowsNativeCursor::createMonochromeCursor(const std::uint8_t* data,
                                                   const std::uint8_t* mask,
                                                   int width,
                                                   int height,
                                                   int hotspotX,
                                                   int hotspotY)
{
    if (data == nullptr || mask == nullptr ||
            width <= 0 || height <= 0 ||
            hotspotX < 0 || hotspotX >= width ||
            hotspotY < 0 || hotspotY >= height) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // CreateIconIndirect() expects a double-height monochrome bitmap. Its top
    // half is the AND plane and its bottom half is the XOR plane. Converting
    // from SDL's data/mask truth table preserves all four pixel operations.
    const int bitmapWidth = (width + 15) & ~15;
    const int bitmapPitch = bitmapWidth / 8;
    const int sourcePitch = (width + 7) / 8;
    const auto planeSize = static_cast<std::size_t>(bitmapPitch * height);
    std::vector<std::uint8_t> windowsMask(planeSize * 2, 0);
    std::fill_n(windowsMask.begin(), planeSize, 0xFF);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool dataBit = readMsbBit(data, sourcePitch, x, y);
            const bool maskBit = readMsbBit(mask, sourcePitch, x, y);
            setMsbBit(windowsMask, bitmapPitch, x, y, !maskBit);
            setMsbBit(windowsMask, bitmapPitch, x, y + height, dataBit != maskBit);
        }
    }

    HBITMAP maskBitmap = CreateBitmap(bitmapWidth,
                                      height * 2,
                                      1,
                                      1,
                                      windowsMask.data());
    if (maskBitmap == nullptr) {
        return nullptr;
    }

    ICONINFO iconInfo = {};
    iconInfo.fIcon = FALSE;
    iconInfo.xHotspot = static_cast<DWORD>(hotspotX);
    iconInfo.yHotspot = static_cast<DWORD>(hotspotY);
    iconInfo.hbmMask = maskBitmap;
    HCURSOR cursor = static_cast<HCURSOR>(CreateIconIndirect(&iconInfo));
    DeleteObject(maskBitmap);
    if (cursor == nullptr) {
        return nullptr;
    }

    HCURSOR exactSizeCursor = static_cast<HCURSOR>(CopyImage(cursor,
                                                             IMAGE_CURSOR,
                                                             bitmapWidth,
                                                             height,
                                                             0));
    DestroyCursor(cursor);
    return exactSizeCursor;
}

void WindowsNativeCursor::destroyCursor(void* cursor)
{
    if (cursor != nullptr) {
        DestroyCursor(static_cast<HCURSOR>(cursor));
    }
}
