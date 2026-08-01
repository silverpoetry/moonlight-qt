#include "clipboardfilemanifest.h"

#include <Limelight.h>

#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFileInfo>
#include <QSet>

#include <utility>

namespace
{
    bool encodeEntries(
            const QVector<ClipboardFileEntry>& entries,
            QByteArray& manifest,
            const std::function<bool()>& isCurrent);

    bool appendEntry(const QString& localPath,
                     const QString& relativePath,
                     int depth,
                     QVector<ClipboardFileEntry>& entries,
                     quint64& totalFileBytes,
                     const std::function<bool()>& isCurrent)
    {
        if (!isCurrent() ||
                depth > 128 ||
                entries.size() >=
                    static_cast<int>(LI_CLIPBOARD_MAX_FILE_ENTRIES)) {
            return false;
        }

        const QFileInfo info(localPath);
        if (!info.exists() ||
                info.isSymLink() ||
                (!info.isFile() && !info.isDir())) {
            return false;
        }

        const QByteArray path = relativePath.toUtf8();
        ClipboardFileEntry entry {
            static_cast<quint8>(
                info.isDir() ?
                    LI_CLIPBOARD_FILE_TYPE_DIRECTORY :
                    LI_CLIPBOARD_FILE_TYPE_REGULAR),
            relativePath,
            info.absoluteFilePath(),
            info.isFile() ? static_cast<quint64>(info.size()) : 0,
            info.lastModified().isValid() ?
                static_cast<quint64>(qMax<qint64>(
                    0, info.lastModified().toMSecsSinceEpoch())) :
                0,
        };
        LI_CLIPBOARD_FILE_MANIFEST_ENTRY protocolEntry {
            entry.type,
            static_cast<quint32>(path.size()),
            entry.size,
            entry.modifiedTimeMs,
            reinterpret_cast<const uint8_t*>(path.constData()),
        };
        QByteArray validationBuffer(
                    LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE + path.size(),
                    Qt::Uninitialized);
        size_t encodedLength = 0;
        if (!LiEncodeClipboardFileManifestEntry(
                    reinterpret_cast<uint8_t*>(validationBuffer.data()),
                    static_cast<size_t>(validationBuffer.size()),
                    &protocolEntry,
                    &encodedLength)) {
            return false;
        }

        if (entry.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
            if (entry.size > LI_CLIPBOARD_MAX_FILE_BYTES ||
                    totalFileBytes >
                        LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES - entry.size) {
                return false;
            }
            totalFileBytes += entry.size;
        }
        entries.append(std::move(entry));

        if (!info.isDir()) {
            return true;
        }

        QDirIterator children(
                    info.absoluteFilePath(),
                    QDir::AllEntries |
                    QDir::Hidden |
                    QDir::System |
                    QDir::NoDotAndDotDot,
                    QDirIterator::NoIteratorFlags);
        while (children.hasNext()) {
            children.next();
            const QFileInfo child = children.fileInfo();
            if (!appendEntry(
                        child.absoluteFilePath(),
                        relativePath + "/" + child.fileName(),
                        depth + 1,
                        entries,
                        totalFileBytes,
                        isCurrent)) {
                return false;
            }
        }
        return true;
    }
}

bool ClipboardFileManifest::encode(
        const QVector<ClipboardFileEntry>& entries,
        QByteArray& manifest)
{
    return encodeEntries(entries, manifest, []() { return true; });
}

namespace
{
bool encodeEntries(
        const QVector<ClipboardFileEntry>& entries,
        QByteArray& manifest,
        const std::function<bool()>& isCurrent)
{
    if (!isCurrent() ||
            entries.isEmpty() ||
            entries.size() >
                static_cast<qsizetype>(LI_CLIPBOARD_MAX_FILE_ENTRIES)) {
        return false;
    }

    quint64 totalFileBytes = 0;
    quint32 fileCount = 0;
    qsizetype manifestSize = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
    for (const ClipboardFileEntry& entry : entries) {
        if (!isCurrent()) {
            return false;
        }
        const QByteArray path = entry.relativePath.toUtf8();
        if (manifestSize >
                    static_cast<qsizetype>(
                        LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES) -
                    LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE -
                    path.size()) {
            return false;
        }
        manifestSize += LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE +
                path.size();

        if (entry.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
            if (entry.size > LI_CLIPBOARD_MAX_FILE_BYTES ||
                    totalFileBytes >
                        LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES - entry.size) {
                return false;
            }
            totalFileBytes += entry.size;
            fileCount++;
        }
    }

    QByteArray encoded(manifestSize, Qt::Uninitialized);
    LI_CLIPBOARD_FILE_MANIFEST_HEADER header {
        static_cast<quint32>(entries.size()),
        fileCount,
        totalFileBytes,
    };
    if (!LiEncodeClipboardFileManifestHeader(
                reinterpret_cast<uint8_t*>(encoded.data()),
                static_cast<size_t>(encoded.size()),
                &header)) {
        return false;
    }

    size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
    for (const ClipboardFileEntry& entry : entries) {
        if (!isCurrent()) {
            return false;
        }
        const QByteArray path = entry.relativePath.toUtf8();
        LI_CLIPBOARD_FILE_MANIFEST_ENTRY protocolEntry {
            entry.type,
            static_cast<quint32>(path.size()),
            entry.size,
            entry.modifiedTimeMs,
            reinterpret_cast<const uint8_t*>(path.constData()),
        };
        size_t encodedLength = 0;
        if (!LiEncodeClipboardFileManifestEntry(
                    reinterpret_cast<uint8_t*>(encoded.data()) + offset,
                    static_cast<size_t>(encoded.size()) - offset,
                    &protocolEntry,
                    &encodedLength)) {
            return false;
        }
        offset += encodedLength;
    }

    if (offset != static_cast<size_t>(encoded.size()) ||
            !LiIsValidClipboardFileManifest(
                reinterpret_cast<const uint8_t*>(encoded.constData()),
                static_cast<size_t>(encoded.size()))) {
        return false;
    }

    manifest = std::move(encoded);
    return true;
}
}

bool ClipboardFileManifest::build(
        const QList<QUrl>& urls,
        QVector<ClipboardFileEntry>& entries,
        QByteArray& manifest,
        const std::function<bool()>& isCurrent)
{
    if (urls.isEmpty() || !isCurrent()) {
        return false;
    }

    QVector<ClipboardFileEntry> builtEntries;
    QSet<QString> topLevelNames;
    quint64 totalFileBytes = 0;
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            return false;
        }

        const QFileInfo info(url.toLocalFile());
        const QString name = info.fileName();
        const QString foldedName = name.toCaseFolded();
        if (name.isEmpty() || topLevelNames.contains(foldedName)) {
            return false;
        }
        topLevelNames.insert(foldedName);
        if (!appendEntry(
                    info.absoluteFilePath(),
                    name,
                    0,
                    builtEntries,
                    totalFileBytes,
                    isCurrent)) {
            return false;
        }
    }

    QByteArray encoded;
    if (!isCurrent() ||
            !encodeEntries(builtEntries, encoded, isCurrent)) {
        return false;
    }

    entries = std::move(builtEntries);
    manifest = std::move(encoded);
    return true;
}
