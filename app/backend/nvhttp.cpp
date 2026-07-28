#include "nvcomputer.h"
#include <Limelight.h>

#include <QDebug>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QtEndian>
#include <QNetworkProxy>
#include <QUrlQuery>

#include <limits>

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000
#define QUIT_TIMEOUT_MS 30000
#define CLIPBOARD_BLOB_TIMEOUT_MS 30000
#define CLIPBOARD_FILE_TIMEOUT_MS 60000

namespace {
    constexpr int ClipboardMaxBlobBytes = 32 * 1024 * 1024;
    constexpr int ClipboardMaxFileChunkBytes =
            LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES;

    bool isCanonicalUuid(const QString& id)
    {
        const QString normalized = id.trimmed().toLower();
        const QUuid uuid(normalized);
        return !uuid.isNull() &&
                uuid.toString(QUuid::WithoutBraces)
                    .compare(normalized, Qt::CaseInsensitive) == 0;
    }
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid, QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this)),
    m_ServerCert(serverCert),
    m_UseTrueUid(useTrueUid)
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(computer->preferredAddress(), computer->activeHttpsPort, computer->serverCert, !computer->isNvidiaServerSoftware, nam)
{
}

void NvHTTP::setServerCert(QSslCertificate serverCert)
{
    m_ServerCert = serverCert;
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

void NvHTTP::setTrueUid(bool useTrueUid)
{
    m_UseTrueUid = useTrueUid;
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

QSslCertificate NvHTTP::serverCert()
{
    return m_ServerCert;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    QString serverInfo;

    // Check if we have a pinned cert and HTTPS port for this host yet
    if (!m_ServerCert.isNull() && httpsPort() != 0)
    {
        try
        {
            // Always try HTTPS first, since it properly reports
            // pairing status (and a few other attributes).
            serverInfo = openConnectionToString(m_BaseUrlHttps,
                                                "serverinfo",
                                                nullptr,
                                                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                logLevel);
            // Throws if the request failed
            verifyResponseStatus(serverInfo);
        }
        catch (const GfeHttpResponseException& e)
        {
            if (e.getStatusCode() == 401)
            {
                // Certificate validation error, fallback to HTTP
                serverInfo = openConnectionToString(m_BaseUrlHttp,
                                                    "serverinfo",
                                                    nullptr,
                                                    fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                    logLevel);
                verifyResponseStatus(serverInfo);
            }
            else
            {
                // Rethrow real errors
                throw e;
            }
        }
    }
    else
    {
        // Only use HTTP prior to pairing or fetching HTTPS port
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        verifyResponseStatus(serverInfo);

        // Populate the HTTPS port
        uint16_t httpsPort = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (httpsPort == 0) {
            httpsPort = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(httpsPort);

        // If we just needed to determine the HTTPS port, we'll try again over
        // HTTPS now that we have the port number
        if (!m_ServerCert.isNull()) {
            return getServerInfo(logLevel, fastFail);
        }
    }

    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 bool isGfe,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool sops,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString& rtspSessionUrl)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   // Using an FPS value over 60 causes SOPS to default to 720p60,
                                   // so force it to 0 to ensure the correct resolution is set. We
                                   // used to use 60 here but that locked the frame rate to 60 FPS
                                   // on GFE 3.20.3. We don't need this hack for Sunshine.
                                   QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps)+
                                   "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                                   "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                                   "&rikeyid="+QString::number(riKeyId)+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   LiGetLaunchUrlQueryParameters(),
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

void
NvHTTP::quitApp()
{
    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   "cancel",
                                   nullptr,
                                   QUIT_TIMEOUT_MS);

    qInfo() << "Quit response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    // Newer GFE versions will just return success even if quitting fails
    // if we're not the original requester.
    if (getCurrentGame(getServerInfo(NvHTTP::NVLL_ERROR)) != 0) {
        // Generate a synthetic GfeResponseException letting the caller know
        // that they can't kill someone else's stream.
        throw GfeHttpResponseException(599, "");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (!modes.isEmpty()) {
                if (XML_NAME_EQUALS(name, "Width")) {
                    modes.last().width = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "Height")) {
                    modes.last().height = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "RefreshRate")) {
                    modes.last().refreshRate = xmlReader.readElementText().toInt();
                }
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    throw std::runtime_error("Invalid applist XML");
                }
                apps.append(NvApp());
            }
            else if (!apps.isEmpty()) {
                if (XML_NAME_EQUALS(name, "AppTitle")) {
                    apps.last().name = xmlReader.readElementText();
                }
                else if (XML_NAME_EQUALS(name, "ID")) {
                    apps.last().id = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "IsHdrSupported")) {
                    apps.last().hdrSupported = xmlReader.readElementText() == "1";
                }
                else if (XML_NAME_EQUALS(name, "IsAppCollectorGame")) {
                    apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
                }
            }
        }
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (XML_NAME_EQUALS(xmlReader.name(), "root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected for unpaired PCs when we fetch serverinfo over HTTPS
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

NvHTTP::ClipboardBlobUploadResult
NvHTTP::uploadClipboardBlob(const QByteArray& mimeType,
                            const QByteArray& content,
                            quint64 originId)
{
    if ((mimeType != "image/png" && mimeType != "text/plain") ||
            content.isEmpty() ||
            content.size() > ClipboardMaxBlobBytes ||
            originId == 0) {
        throw std::invalid_argument("Invalid clipboard blob upload");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/blobs");
    url.setQuery(QString());

    QNetworkRequest request = createRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    request.setRawHeader("X-Clipboard-Mime", mimeType);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(originId)).toLatin1());
    request.setRawHeader("X-Clipboard-Idempotency-Key",
                         QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1());

    QNetworkReply* reply = executeRequest(request,
                                          &content,
                                          "clipboard blob upload",
                                          CLIPBOARD_BLOB_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          64 * 1024);
    const QByteArray response = reply->readAll();
    delete reply;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Malformed clipboard blob upload response");
    }

    const QJsonObject object = document.object();
    const QString id = object.value("id").toString().trimmed().toLower();
    const QUuid uuid(id);
    const auto sizeValue = object.value("size");
    const double sizeNumber = sizeValue.isDouble() ?
                sizeValue.toDouble(-1) :
                -1;
    const qint64 size = sizeNumber >= 0 ?
                static_cast<qint64>(sizeNumber) :
                -1;
    const QByteArray digestHex = object.value("sha256").toString().toLatin1().toLower();
    const QByteArray digest = QByteArray::fromHex(digestHex);
    const QByteArray expectedDigest =
            QCryptographicHash::hash(content, QCryptographicHash::Sha256);

    if (uuid.isNull() ||
            uuid.toString(QUuid::WithoutBraces).compare(id, Qt::CaseInsensitive) != 0 ||
            static_cast<double>(size) != sizeNumber ||
            size != content.size() ||
            digestHex.size() != 64 ||
            digest.toHex() != digestHex ||
            digest != expectedDigest) {
        throw std::runtime_error("Invalid clipboard blob upload response");
    }

    return {
        id,
        static_cast<quint32>(size),
        digest,
    };
}

QByteArray
NvHTTP::downloadClipboardBlob(const QString& id,
                              const QByteArray& expectedMimeType,
                              quint64 requestOriginId,
                              quint32 expectedSize,
                              const QByteArray& expectedSha256)
{
    const QString normalizedId = id.trimmed().toLower();
    const QUuid uuid(normalizedId);
    if (uuid.isNull() ||
            uuid.toString(QUuid::WithoutBraces).compare(normalizedId, Qt::CaseInsensitive) != 0 ||
            (expectedMimeType != "image/png" && expectedMimeType != "text/plain") ||
            requestOriginId == 0 ||
            expectedSize == 0 ||
            expectedSize > static_cast<quint32>(ClipboardMaxBlobBytes) ||
            expectedSha256.size() != 32) {
        throw std::invalid_argument("Invalid clipboard blob download");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/blobs/" + normalizedId);
    url.setQuery(QString());

    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(requestOriginId)).toLatin1());

    QNetworkReply* reply = executeRequest(request,
                                          nullptr,
                                          "clipboard blob download",
                                          CLIPBOARD_BLOB_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          expectedSize);

    const QByteArray responseMimeType =
            reply->rawHeader("Content-Type").split(';').value(0).trimmed().toLower();
    const QByteArray responseDigestHex =
            reply->rawHeader("X-Clipboard-SHA256").trimmed().toLower();
    const QVariant contentLength = reply->header(QNetworkRequest::ContentLengthHeader);
    const QByteArray content = reply->readAll();
    delete reply;

    if (responseMimeType != expectedMimeType ||
            responseDigestHex.size() != 64 ||
            QByteArray::fromHex(responseDigestHex) != expectedSha256 ||
            (contentLength.isValid() &&
             contentLength.toLongLong() != static_cast<qint64>(expectedSize)) ||
            content.size() != static_cast<int>(expectedSize) ||
            QCryptographicHash::hash(content, QCryptographicHash::Sha256) != expectedSha256) {
        throw std::runtime_error("Clipboard blob integrity check failed");
    }

    return content;
}

NvHTTP::ClipboardFileOfferResult
NvHTTP::registerClipboardFileSource(quint64 originId)
{
    if (originId == 0) {
        throw std::invalid_argument("Invalid clipboard file offer");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files");
    url.setQuery(QString());

    QNetworkRequest request = createRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/vnd.moonlight.file-offer"));
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(originId)).toLatin1());
    request.setRawHeader("X-Clipboard-Idempotency-Key",
                         QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1());

    const QByteArray emptyContent;
    QNetworkReply* reply = executeRequest(request,
                                          &emptyContent,
                                          "clipboard file source registration",
                                          CLIPBOARD_FILE_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          64 * 1024,
                                          true);
    const QByteArray response = reply->readAll();
    delete reply;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error(
                    "Malformed clipboard file source response");
    }

    const QJsonObject object = document.object();
    const QString id = object.value("id").toString().trimmed().toLower();
    if (!isCanonicalUuid(id)) {
        throw std::runtime_error(
                    "Invalid clipboard file source response");
    }

    return {id};
}

NvHTTP::ClipboardFileRequest
NvHTTP::pollClipboardFileRequest(
        const QString& id,
        quint64 originId,
        const std::function<bool()>& cancelRequested)
{
    const QString normalizedId = id.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) || originId == 0) {
        throw std::invalid_argument("Invalid clipboard file request poll");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId +
                "/requests");
    QUrlQuery query;
    query.addQueryItem("wait", QString::number(25));
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(originId)).toLatin1());

    QNetworkReply* reply = executeRequest(request,
                                          nullptr,
                                          "clipboard file request poll",
                                          30 * 1000,
                                          NvLogLevel::NVLL_ERROR,
                                          64 * 1024,
                                          true,
                                          cancelRequested);
    const QByteArray response = reply->readAll();
    delete reply;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Malformed clipboard file request");
    }
    const QJsonValue requestValue = document.object().value("request");
    if (requestValue.isNull()) {
        return {
            false,
            {},
            ClipboardFileRequestKind::Chunk,
            0,
            0,
            0,
        };
    }
    if (!requestValue.isObject()) {
        throw std::runtime_error("Invalid clipboard file request");
    }

    const QJsonObject object = requestValue.toObject();
    const QString requestId = object.value("id").toString().trimmed().toLower();
    const QString type = object.value("type").toString().trimmed().toLower();
    const double fileIndexNumber = object.value("file_index").toDouble(-1);
    const double offsetNumber = object.value("offset").toDouble(-1);
    const double lengthNumber = object.value("length").toDouble(-1);
    const bool manifestRequest = type == QStringLiteral("manifest");
    const bool chunkRequest = type == QStringLiteral("chunk");
    if (!isCanonicalUuid(requestId) ||
            (!manifestRequest && !chunkRequest) ||
            fileIndexNumber < 0 ||
            fileIndexNumber > std::numeric_limits<quint32>::max() ||
            static_cast<double>(static_cast<quint32>(fileIndexNumber)) !=
                fileIndexNumber ||
            offsetNumber < 0 ||
            offsetNumber > static_cast<double>(
                std::numeric_limits<quint64>::max()) ||
            static_cast<double>(static_cast<quint64>(offsetNumber)) !=
                offsetNumber ||
            lengthNumber < 0 ||
            lengthNumber > ClipboardMaxFileChunkBytes ||
            static_cast<double>(static_cast<quint32>(lengthNumber)) !=
                lengthNumber ||
            (manifestRequest &&
             (fileIndexNumber != 0 ||
              offsetNumber != 0 ||
              lengthNumber != 0)) ||
            (chunkRequest && lengthNumber == 0)) {
        throw std::runtime_error("Invalid clipboard file request fields");
    }
    return {
        true,
        requestId,
        manifestRequest ?
            ClipboardFileRequestKind::Manifest :
            ClipboardFileRequestKind::Chunk,
        static_cast<quint32>(fileIndexNumber),
        static_cast<quint64>(offsetNumber),
        static_cast<quint32>(lengthNumber),
    };
}

void
NvHTTP::fulfillClipboardFileRequest(const QString& id,
                                    const QString& requestId,
                                    ClipboardFileRequestKind kind,
                                    const QByteArray& content,
                                    quint64 originId)
{
    const QString normalizedId = id.trimmed().toLower();
    const QString normalizedRequestId = requestId.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) ||
            !isCanonicalUuid(normalizedRequestId) ||
            content.isEmpty() ||
            content.size() > ClipboardMaxFileChunkBytes ||
            (kind == ClipboardFileRequestKind::Manifest &&
             (content.size() >
                  static_cast<int>(
                      LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES) ||
              !LiIsValidClipboardFileManifest(
                  reinterpret_cast<const uint8_t*>(
                      content.constData()),
                  static_cast<size_t>(content.size())))) ||
            originId == 0) {
        throw std::invalid_argument("Invalid clipboard file response");
    }
    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId +
                "/requests/" + normalizedRequestId);
    url.setQuery(QString());
    QNetworkRequest request = createRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      kind == ClipboardFileRequestKind::Manifest ?
                          QByteArrayLiteral(
                              "application/vnd.moonlight.file-manifest") :
                          QByteArrayLiteral("application/octet-stream"));
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(originId)).toLatin1());
    request.setRawHeader(
                "X-Clipboard-SHA256",
                QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    QNetworkReply* reply = executeRequest(request,
                                          &content,
                                          "clipboard file response",
                                          CLIPBOARD_FILE_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          64 * 1024,
                                          true);
    delete reply;
}

void
NvHTTP::rejectClipboardFileRequest(const QString& id,
                                   const QString& requestId,
                                   quint64 originId)
{
    const QString normalizedId = id.trimmed().toLower();
    const QString normalizedRequestId = requestId.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) ||
            !isCanonicalUuid(normalizedRequestId) ||
            originId == 0) {
        throw std::invalid_argument(
                    "Invalid clipboard file rejection");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId +
                "/requests/" + normalizedRequestId);
    url.setQuery(QString());
    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(
                             static_cast<qulonglong>(originId))
                             .toLatin1());
    request.setRawHeader("X-Clipboard-Error",
                         QByteArrayLiteral("source_unavailable"));

    const QByteArray emptyContent;
    QNetworkReply* reply = executeRequest(
                request,
                &emptyContent,
                "clipboard file rejection",
                CLIPBOARD_FILE_TIMEOUT_MS,
                NvLogLevel::NVLL_ERROR,
                64 * 1024,
                true);
    delete reply;
}

void
NvHTTP::releaseClipboardFileSource(const QString& id,
                                   quint64 originId)
{
    const QString normalizedId = id.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) || originId == 0) {
        throw std::invalid_argument(
                    "Invalid clipboard file source release");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId +
                "/release");
    url.setQuery(QString());
    QNetworkRequest request = createRequest(url);
    request.setRawHeader(
                "X-Clipboard-Origin",
                QString::number(
                    static_cast<qulonglong>(originId)).toLatin1());

    const QByteArray emptyContent;
    QNetworkReply* reply = executeRequest(
                request,
                &emptyContent,
                "clipboard file source release",
                REQUEST_TIMEOUT_MS,
                NvLogLevel::NVLL_ERROR,
                64 * 1024,
                true);
    delete reply;
}

QByteArray
NvHTTP::downloadClipboardFileManifest(const QString& id,
                                      quint64 requestOriginId)
{
    const QString normalizedId = id.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) ||
            requestOriginId == 0) {
        throw std::invalid_argument("Invalid clipboard file manifest download");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId + "/manifest");
    url.setQuery(QString());
    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(requestOriginId)).toLatin1());

    QNetworkReply* reply = executeRequest(request,
                                          nullptr,
                                          "clipboard file manifest download",
                                          CLIPBOARD_FILE_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES);
    const QByteArray contentType =
            reply->rawHeader("Content-Type").split(';').value(0).trimmed().toLower();
    const QByteArray digestHex =
            reply->rawHeader("X-Clipboard-SHA256").trimmed().toLower();
    const QVariant contentLength = reply->header(QNetworkRequest::ContentLengthHeader);
    const QByteArray manifest = reply->readAll();
    delete reply;

    const QByteArray digest = QByteArray::fromHex(digestHex);
    if (contentType != "application/vnd.moonlight.file-manifest" ||
            digestHex.size() != 64 ||
            (contentLength.isValid() &&
             contentLength.toLongLong() != manifest.size()) ||
            manifest.isEmpty() ||
            manifest.size() >
                static_cast<int>(LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES) ||
            digest.size() != LI_CLIPBOARD_SHA256_BYTES ||
            digest.toHex() != digestHex ||
            QCryptographicHash::hash(
                manifest,
                QCryptographicHash::Sha256) != digest ||
            !LiIsValidClipboardFileManifest(
                reinterpret_cast<const uint8_t*>(manifest.constData()),
                static_cast<size_t>(manifest.size()))) {
        throw std::runtime_error("Clipboard file manifest integrity check failed");
    }
    return manifest;
}

QByteArray
NvHTTP::downloadClipboardFileChunk(const QString& id,
                                   quint32 fileIndex,
                                   quint64 offset,
                                   quint32 length,
                                   quint64 requestOriginId)
{
    const QString normalizedId = id.trimmed().toLower();
    if (!isCanonicalUuid(normalizedId) ||
            requestOriginId == 0 ||
            length == 0 ||
            length > static_cast<quint32>(ClipboardMaxFileChunkBytes)) {
        throw std::invalid_argument("Invalid clipboard file chunk download");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/api/v2/clipboard/files/" + normalizedId +
                "/" + QString::number(fileIndex));
    QUrlQuery query;
    query.addQueryItem("offset", QString::number(static_cast<qulonglong>(offset)));
    query.addQueryItem("length", QString::number(length));
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Clipboard-Origin",
                         QString::number(static_cast<qulonglong>(requestOriginId)).toLatin1());
    QNetworkReply* reply = executeRequest(request,
                                          nullptr,
                                          "clipboard file chunk download",
                                          CLIPBOARD_FILE_TIMEOUT_MS,
                                          NvLogLevel::NVLL_ERROR,
                                          length);
    const QByteArray contentType =
            reply->rawHeader("Content-Type").split(';').value(0).trimmed().toLower();
    const QByteArray digestHex =
            reply->rawHeader("X-Clipboard-SHA256").trimmed().toLower();
    const QByteArray offsetHeader =
            reply->rawHeader("X-Clipboard-Offset").trimmed();
    const QVariant contentLength = reply->header(QNetworkRequest::ContentLengthHeader);
    const QByteArray content = reply->readAll();
    delete reply;

    bool offsetOk = false;
    const quint64 responseOffset = offsetHeader.toULongLong(&offsetOk);
    const QByteArray digest = QByteArray::fromHex(digestHex);
    if (contentType != "application/octet-stream" ||
            digestHex.size() != 64 ||
            digest.toHex() != digestHex ||
            !offsetOk ||
            responseOffset != offset ||
            (contentLength.isValid() &&
             contentLength.toLongLong() != static_cast<qint64>(length)) ||
            content.size() != static_cast<int>(length) ||
            QCryptographicHash::hash(content, QCryptographicHash::Sha256) != digest) {
        throw std::runtime_error("Clipboard file chunk integrity check failed");
    }
    return content;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    return QByteArray::fromHex(getXmlString(xml, tagName).toUtf8());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return QString();
}

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    bool ignoreErrors = true;

    if (m_ServerCert.isNull()) {
        // We should never make an HTTPS request without a cert
        Q_ASSERT(!m_ServerCert.isNull());
        return;
    }

    for (const QSslError& error : errors) {
        if (m_ServerCert != error.certificate()) {
            ignoreErrors = false;
            break;
        }
    }

    if (ignoreErrors) {
        reply->ignoreSslErrors(errors);
    }
}

QNetworkRequest NvHTTP::createRequest(const QUrl& url)
{
    QNetworkRequest request(url);

    // Authenticate with the same client identity used for all paired host
    // requests and pin the server certificate in handleSslErrors().
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Sunshine and GFE expect these management requests to be short-lived.
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    return request;
}

QNetworkReply*
NvHTTP::executeRequest(QNetworkRequest request,
                       const QByteArray* uploadData,
                       QString command,
                       int timeoutMs,
                       NvLogLevel logLevel,
                       qint64 maximumResponseBytes,
                       bool reuseConnection,
                       const std::function<bool()>& cancelRequested)
{
    const QUrl url = request.url();
    if (reuseConnection) {
        request.setRawHeader("Connection", "keep-alive");
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
        request.setAttribute(
                    QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute,
                    60);
#endif
    }
    auto sslErrorsConnection =
            connect(m_Nam, &QNetworkAccessManager::sslErrors,
                    this, &NvHTTP::handleSslErrors);
    QNetworkReply* reply = uploadData == nullptr ?
                m_Nam->get(request) :
                m_Nam->post(request, *uploadData);
    if (maximumResponseBytes > 0) {
        reply->setProperty("moonlightResponseTooLarge", false);
        connect(reply, &QNetworkReply::readyRead, reply,
                [reply, maximumResponseBytes]() {
            if (reply->bytesAvailable() > maximumResponseBytes) {
                reply->setProperty("moonlightResponseTooLarge", true);
                reply->abort();
            }
        });
    }

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &loop, &QEventLoop::quit);
    bool canceledByCaller = false;
    QTimer cancelTimer;
    if (cancelRequested) {
        cancelTimer.setInterval(50);
        connect(&cancelTimer, &QTimer::timeout, &loop,
                [&canceledByCaller, &cancelRequested, reply]() {
            if (cancelRequested()) {
                canceledByCaller = true;
                reply->abort();
            }
        });
        cancelTimer.start();
    }
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (!reply->isFinished()) {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    if (!reuseConnection) {
        m_Nam->clearAccessCache();
    }
#endif
    disconnect(sslErrorsConnection);

    if (reply->property("moonlightResponseTooLarge").toBool()) {
        delete reply;
        throw std::runtime_error("HTTP response exceeded the size limit");
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (!canceledByCaller && logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (canceledByCaller) {
            delete reply;
            throw std::runtime_error("Request canceled");
        }
        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            GfeHttpResponseException exception(401, "Server certificate mismatch");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError,
                                              "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    return reply;
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a placeholder UID for GFE allow them to quit games for each other.
    url.setQuery("uniqueid=" + (m_UseTrueUid ? IdentityManager::get()->getUniqueId() : "0123456789ABCDEF") +
                 "&uuid=" + QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request = createRequest(url);
    return executeRequest(request, nullptr, command, timeoutMs, logLevel);
}
