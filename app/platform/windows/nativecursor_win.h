#pragma once

#include <cstdint>

namespace WindowsNativeCursor {
void* createMonochromeCursor(const std::uint8_t* data,
                             const std::uint8_t* mask,
                             int width,
                             int height,
                             int hotspotX,
                             int hotspotY);

void destroyCursor(void* cursor);
}
