#pragma once

#include <QByteArray>

#include <functional>

namespace ClipboardVirtualFiles {
    using ReadCallback =
            std::function<QByteArray(quint32 fileIndex,
                                     quint64 offset,
                                     quint32 length)>;

    bool setRemoteFiles(const QByteArray& manifest,
                        const QByteArray& markerMime,
                        const QByteArray& markerData,
                        ReadCallback readCallback);

    bool hasMarker(const QByteArray& markerMime);

    bool hasRemoteFiles(const QByteArray& markerMime);
}
