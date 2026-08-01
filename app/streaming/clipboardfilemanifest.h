#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>

struct ClipboardFileEntry
{
    quint8 type;
    QString relativePath;
    QString localPath;
    quint64 size;
    quint64 modifiedTimeMs;
};

namespace ClipboardFileManifest
{
    bool encode(const QVector<ClipboardFileEntry>& entries,
                QByteArray& manifest);

    bool build(const QList<QUrl>& urls,
               QVector<ClipboardFileEntry>& entries,
               QByteArray& manifest,
               const std::function<bool()>& isCurrent);
}
