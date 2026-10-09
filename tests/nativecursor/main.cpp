#include "platform/windows/nativecursor_win.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
constexpr int CURSOR_WIDTH = 16;
constexpr int CURSOR_HEIGHT = 16;

std::vector<std::uint8_t> renderCursor(void* cursor, std::uint32_t background)
{
    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = CURSOR_WIDTH;
    bitmapInfo.bmiHeader.biHeight = -CURSOR_HEIGHT;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr,
                                      &bitmapInfo,
                                      DIB_RGB_COLORS,
                                      &pixels,
                                      nullptr,
                                      0);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(pixels),
                CURSOR_WIDTH * CURSOR_HEIGHT,
                background);
    DrawIconEx(dc,
               0,
               0,
               static_cast<HCURSOR>(cursor),
               CURSOR_WIDTH,
               CURSOR_HEIGHT,
               0,
               nullptr,
               DI_NORMAL);
    GdiFlush();

    std::vector<std::uint8_t> result(CURSOR_WIDTH * CURSOR_HEIGHT * 4);
    std::copy_n(static_cast<std::uint8_t*>(pixels), result.size(), result.begin());
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return result;
}

bool isLight(const std::vector<std::uint8_t>& image, int x)
{
    const auto offset = static_cast<std::size_t>(x) * 4;
    return image[offset] + image[offset + 1] + image[offset + 2] >= 3 * 128;
}
}

int main()
{
    constexpr int pitch = CURSOR_WIDTH / 8;
    constexpr int planeSize = pitch * CURSOR_HEIGHT;
    std::array<std::uint8_t, planeSize> data {};
    std::array<std::uint8_t, planeSize> mask {};

    // Pixel order: transparent, black, white, inverted.
    data[0] = 0x50;
    mask[0] = 0x60;
    void* cursor = WindowsNativeCursor::createMonochromeCursor(data.data(),
                                                               mask.data(),
                                                               CURSOR_WIDTH,
                                                               CURSOR_HEIGHT,
                                                               0,
                                                               0);
    if (cursor == nullptr) {
        std::cerr << "Failed to create test cursor: " << GetLastError() << '\n';
        return 1;
    }

    const auto onBlack = renderCursor(cursor, 0xFF000000u);
    const auto onWhite = renderCursor(cursor, 0xFFFFFFFFu);
    WindowsNativeCursor::destroyCursor(cursor);

    const std::array<std::array<bool, 2>, 4> expected {{
        {{false, true}},
        {{false, false}},
        {{true, true}},
        {{true, false}},
    }};
    for (int x = 0; x < static_cast<int>(expected.size()); ++x) {
        const std::array<bool, 2> actual {{isLight(onBlack, x), isLight(onWhite, x)}};
        if (actual != expected[x]) {
            std::cerr << "Unexpected cursor operation at x=" << x
                      << ": black=" << actual[0]
                      << " white=" << actual[1] << '\n';
            return 2;
        }
    }

    return 0;
}
