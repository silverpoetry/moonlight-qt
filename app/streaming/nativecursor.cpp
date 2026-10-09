#include "nativecursor.h"

#include <Limelight.h>
#include <SDL.h>

#include <QImage>

#include <algorithm>
#include <vector>

#ifdef Q_OS_WIN32
#include "platform/windows/nativecursor_win.h"
#endif

namespace {
int scaleDimension(quint16 value, quint32 scale)
{
    return qMax(1, static_cast<int>((static_cast<quint64>(value) * scale + 0x8000) >> 16));
}

int scaleHotspot(quint16 value, quint32 scale, int dimension)
{
    return qMin(dimension - 1,
                qMax(0, static_cast<int>((static_cast<quint64>(value) * scale + 0x8000) >> 16)));
}

SDL_Cursor* createBgraCursor(const QByteArray& imageData,
                             quint16 sourceWidth,
                             quint16 sourceHeight,
                             quint16 sourceHotspotX,
                             quint16 sourceHotspotY,
                             quint32 scaleX,
                             quint32 scaleY)
{
    const quint64 requiredBytes = static_cast<quint64>(sourceWidth) * sourceHeight * 4;
    if (sourceWidth == 0 || sourceHeight == 0 ||
            static_cast<quint64>(imageData.size()) < requiredBytes) {
        SDL_SetError("Invalid BGRA native cursor payload");
        return nullptr;
    }

    QImage sourceImage(reinterpret_cast<const uchar*>(imageData.constData()),
                       sourceWidth,
                       sourceHeight,
                       sourceWidth * 4,
                       QImage::Format_ARGB32);
    const int scaledWidth = scaleDimension(sourceWidth, scaleX);
    const int scaledHeight = scaleDimension(sourceHeight, scaleY);
    QImage scaledImage = sourceImage.scaled(scaledWidth,
                                            scaledHeight,
                                            Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_ARGB32);

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
                scaledImage.bits(),
                scaledImage.width(),
                scaledImage.height(),
                32,
                scaledImage.bytesPerLine(),
                SDL_PIXELFORMAT_BGRA32);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_Cursor* cursor = SDL_CreateColorCursor(
                surface,
                scaleHotspot(sourceHotspotX, scaleX, scaledWidth),
                scaleHotspot(sourceHotspotY, scaleY, scaledHeight));
    SDL_FreeSurface(surface);
    return cursor;
}

bool readMsbBit(const unsigned char* plane, int pitch, int x, int y)
{
    return (plane[y * pitch + x / 8] & (0x80u >> (x % 8))) != 0;
}

void setMsbBit(std::vector<Uint8>& plane, int pitch, int x, int y, bool value = true)
{
    const auto mask = static_cast<Uint8>(0x80u >> (x % 8));
    auto& byte = plane[static_cast<std::size_t>(y * pitch + x / 8)];
    if (value) {
        byte |= mask;
    }
    else {
        byte &= static_cast<Uint8>(~mask);
    }
}

struct ScaledMonochromeCursor
{
    int width;
    int height;
    int hotspotX;
    int hotspotY;
    std::vector<Uint8> data;
    std::vector<Uint8> mask;
};

bool scaleMonochromeCursor(const QByteArray& imageData,
                           quint16 sourceWidth,
                           quint16 sourceHeight,
                           quint16 sourceHotspotX,
                           quint16 sourceHotspotY,
                           quint32 scaleX,
                           quint32 scaleY,
                           ScaledMonochromeCursor& result)
{
    if (sourceWidth == 0 || sourceHeight == 0) {
        SDL_SetError("Invalid monochrome native cursor dimensions");
        return false;
    }

    const int sourcePitch = (sourceWidth + 7) / 8;
    const quint64 sourcePlaneSize = static_cast<quint64>(sourcePitch) * sourceHeight;
    if (static_cast<quint64>(imageData.size()) < sourcePlaneSize * 2) {
        SDL_SetError("Invalid monochrome native cursor payload");
        return false;
    }

    result.width = scaleDimension(sourceWidth, scaleX);
    result.height = scaleDimension(sourceHeight, scaleY);
    result.hotspotX = scaleHotspot(sourceHotspotX, scaleX, result.width);
    result.hotspotY = scaleHotspot(sourceHotspotY, scaleY, result.height);
    const int resultPitch = (result.width + 7) / 8;
    result.data.assign(static_cast<std::size_t>(resultPitch * result.height), 0);
    result.mask.assign(result.data.size(), 0);

    const auto* sourceData = reinterpret_cast<const unsigned char*>(imageData.constData());
    const auto* sourceMask = sourceData + sourcePlaneSize;
    for (int y = 0; y < result.height; ++y) {
        const int sourceY = qMin(static_cast<int>(sourceHeight) - 1,
                                 static_cast<int>((static_cast<qint64>(y) * sourceHeight) / result.height));
        for (int x = 0; x < result.width; ++x) {
            const int sourceX = qMin(static_cast<int>(sourceWidth) - 1,
                                     static_cast<int>((static_cast<qint64>(x) * sourceWidth) / result.width));
            setMsbBit(result.data, resultPitch, x, y,
                      readMsbBit(sourceData, sourcePitch, sourceX, sourceY));
            setMsbBit(result.mask, resultPitch, x, y,
                      readMsbBit(sourceMask, sourcePitch, sourceX, sourceY));
        }
    }

    return true;
}

#ifndef Q_OS_WIN32
SDL_Cursor* createSdlMonochromeCursor(const ScaledMonochromeCursor& source)
{
    const int cursorWidth = (source.width + 7) & ~7;
    if (cursorWidth == source.width) {
        return SDL_CreateCursor(source.data.data(),
                                source.mask.data(),
                                cursorWidth,
                                source.height,
                                source.hotspotX,
                                source.hotspotY);
    }

    const int sourcePitch = (source.width + 7) / 8;
    const int cursorPitch = cursorWidth / 8;
    std::vector<Uint8> data(static_cast<std::size_t>(cursorPitch * source.height), 0);
    std::vector<Uint8> mask(data.size(), 0);
    for (int y = 0; y < source.height; ++y) {
        std::copy_n(source.data.begin() + y * sourcePitch,
                    sourcePitch,
                    data.begin() + y * cursorPitch);
        std::copy_n(source.mask.begin() + y * sourcePitch,
                    sourcePitch,
                    mask.begin() + y * cursorPitch);
    }
    return SDL_CreateCursor(data.data(),
                            mask.data(),
                            cursorWidth,
                            source.height,
                            source.hotspotX,
                            source.hotspotY);
}
#endif
}

NativeCursor::Cursor::Cursor(SDL_Cursor* sdlCursor, void* platformCursor)
    : m_SdlCursor(sdlCursor),
      m_PlatformCursor(platformCursor)
{
}

NativeCursor::Cursor::~Cursor()
{
    if (m_SdlCursor != nullptr) {
        SDL_FreeCursor(m_SdlCursor);
    }
#ifdef Q_OS_WIN32
    if (m_PlatformCursor != nullptr) {
        WindowsNativeCursor::destroyCursor(m_PlatformCursor);
    }
#endif
}

SDL_Cursor* NativeCursor::Cursor::sdlCursor() const
{
    return m_SdlCursor;
}

void* NativeCursor::Cursor::platformCursor() const
{
    return m_PlatformCursor;
}

int NativeCursor::protocolVersion()
{
#ifdef Q_OS_WIN32
    return LI_NATIVE_CURSOR_VERSION_MONOCHROME;
#else
    return LI_NATIVE_CURSOR_VERSION_BGRA;
#endif
}

std::unique_ptr<NativeCursor::Cursor> NativeCursor::create(quint8 format,
                                                           const QByteArray& imageData,
                                                           quint16 sourceWidth,
                                                           quint16 sourceHeight,
                                                           quint16 sourceHotspotX,
                                                           quint16 sourceHotspotY,
                                                           quint32 scaleX,
                                                           quint32 scaleY)
{
    scaleX = scaleX == 0 ? 1u << 16 : scaleX;
    scaleY = scaleY == 0 ? 1u << 16 : scaleY;

    if (format == LI_NATIVE_CURSOR_FORMAT_BGRA) {
        SDL_Cursor* cursor = createBgraCursor(imageData,
                                             sourceWidth,
                                             sourceHeight,
                                             sourceHotspotX,
                                             sourceHotspotY,
                                             scaleX,
                                             scaleY);
        return cursor != nullptr ? std::unique_ptr<Cursor>(new Cursor(cursor, nullptr)) : nullptr;
    }

    if (format == LI_NATIVE_CURSOR_FORMAT_MONOCHROME) {
        ScaledMonochromeCursor scaledCursor;
        if (!scaleMonochromeCursor(imageData,
                                   sourceWidth,
                                   sourceHeight,
                                   sourceHotspotX,
                                   sourceHotspotY,
                                   scaleX,
                                   scaleY,
                                   scaledCursor)) {
            return nullptr;
        }
#ifdef Q_OS_WIN32
        void* cursor = WindowsNativeCursor::createMonochromeCursor(
                    scaledCursor.data.data(),
                    scaledCursor.mask.data(),
                    scaledCursor.width,
                    scaledCursor.height,
                    scaledCursor.hotspotX,
                    scaledCursor.hotspotY);
        if (cursor == nullptr) {
            SDL_SetError("Failed to create Windows native cursor");
        }
        return cursor != nullptr ? std::unique_ptr<Cursor>(new Cursor(nullptr, cursor)) : nullptr;
#else
        SDL_Cursor* cursor = createSdlMonochromeCursor(scaledCursor);
        return cursor != nullptr ? std::unique_ptr<Cursor>(new Cursor(cursor, nullptr)) : nullptr;
#endif
    }

    SDL_SetError("Unsupported native cursor format: %u", format);
    return nullptr;
}
