#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <memory>

struct SDL_Cursor;

namespace NativeCursor {
class Cursor
{
public:
    ~Cursor();

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    SDL_Cursor* sdlCursor() const;
    void* platformCursor() const;

private:
    friend std::unique_ptr<Cursor> create(quint8,
                                          const QByteArray&,
                                          quint16,
                                          quint16,
                                          quint16,
                                          quint16,
                                          quint32,
                                          quint32);

    Cursor(SDL_Cursor* sdlCursor, void* platformCursor);

    SDL_Cursor* m_SdlCursor;
    void* m_PlatformCursor;
};

int protocolVersion();

std::unique_ptr<Cursor> create(quint8 format,
                               const QByteArray& imageData,
                               quint16 sourceWidth,
                               quint16 sourceHeight,
                               quint16 sourceHotspotX,
                               quint16 sourceHotspotY,
                               quint32 scaleX,
                               quint32 scaleY);
}
