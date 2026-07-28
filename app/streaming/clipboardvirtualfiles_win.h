#pragma once

#include <QByteArray>

#include <functional>

namespace ClipboardVirtualFiles {
    using ManifestCallback = std::function<QByteArray()>;
    using ReadCallback =
            std::function<QByteArray(quint32 fileIndex,
                                     quint64 offset,
                                     quint32 length)>;

    bool setRemoteFiles(const QByteArray& markerMime,
                        const QByteArray& markerData,
                        ManifestCallback manifestCallback,
                        ReadCallback readCallback);

    bool hasMarker(const QByteArray& markerMime);

    bool hasRemoteFiles(const QByteArray& markerMime);
}
