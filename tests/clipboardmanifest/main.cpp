#include "clipboardfilemanifest.h"

#include <Limelight.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>

namespace
{
    const char CanonicalManifestHex[] =
#include "../../moonlight-common-c/moonlight-common-c/tests/fixtures/ClipboardManifestV1.inc"
    ;

    bool verifyCanonicalEncoding()
    {
        constexpr quint64 ModifiedTimeMs = 1720000000000ULL;
        const QVector<ClipboardFileEntry> entries {
            {
                LI_CLIPBOARD_FILE_TYPE_DIRECTORY,
                QStringLiteral("folder"),
                QString(),
                0,
                ModifiedTimeMs,
            },
            {
                LI_CLIPBOARD_FILE_TYPE_REGULAR,
                QStringLiteral("folder/one.txt"),
                QString(),
                10,
                ModifiedTimeMs,
            },
            {
                LI_CLIPBOARD_FILE_TYPE_REGULAR,
                QStringLiteral("two.bin"),
                QString(),
                5,
                ModifiedTimeMs,
            },
        };

        QByteArray encoded;
        if (!ClipboardFileManifest::encode(entries, encoded)) {
            qCritical() << "Qt manifest encoder rejected canonical entries";
            return false;
        }

        const QByteArray canonical = QByteArray::fromHex(
                    QByteArray(CanonicalManifestHex));
        if (encoded != canonical) {
            qCritical() << "Qt manifest differs from common-c fixture";
            return false;
        }
        return true;
    }

    bool verifyInvalidManifestIsNotPublished()
    {
        const QVector<ClipboardFileEntry> unsafeEntries {
            {
                LI_CLIPBOARD_FILE_TYPE_REGULAR,
                QStringLiteral("../secret.txt"),
                QString(),
                1,
                0,
            },
        };
        QByteArray sentinel("unchanged");
        if (ClipboardFileManifest::encode(unsafeEntries, sentinel)) {
            qCritical() << "Qt manifest encoder accepted an unsafe path";
            return false;
        }
        if (sentinel != QByteArrayLiteral("unchanged")) {
            qCritical() << "Failed encoding published a partial manifest";
            return false;
        }
        return true;
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return verifyCanonicalEncoding() && verifyInvalidManifestIsNotPublished() ?
                0 : 1;
}
