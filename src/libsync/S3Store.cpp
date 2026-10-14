#include "libsync/S3Store.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace pl::libsync {

namespace {

const QByteArray kEmptyPayloadHash =
    QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex();

QString stripQuotes(QString etag)
{
    if (etag.size() >= 2 && etag.startsWith(QLatin1Char('"')) && etag.endsWith(QLatin1Char('"')))
        etag = etag.mid(1, etag.size() - 2);
    return etag;
}

// A single-part upload's ETag is the MD5 of its content; a multipart one
// ("<hex>-<parts>") isn't.
QString md5FromEtag(const QString &etag)
{
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-fA-F]{32}$"));
    return hex32.match(etag).hasMatch() ? etag.toLower() : QString();
}

// S3's <Error><Code/><Message/></Error> body, as one line.
QString s3ErrorText(const QByteArray &body)
{
    QXmlStreamReader xml(body);
    QString code;
    QString message;
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("Code"))
            code = xml.readElementText();
        else if (xml.name() == QLatin1String("Message"))
            message = xml.readElementText();
    }
    if (code.isEmpty() && message.isEmpty())
        return {};
    return message.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, message);
}

} // namespace

S3Store::S3Store(S3Config config) : m_config(std::move(config))
{
    m_config.endpoint = m_config.endpoint.trimmed();
    m_config.bucket = m_config.bucket.trimmed();
    m_config.accessKey = m_config.accessKey.trimmed();
    if (m_config.region.trimmed().isEmpty())
        m_config.region = QStringLiteral("auto");
}

QString S3Store::rootPrefix() const
{
    QString prefix = m_config.prefix.trimmed();
    while (prefix.startsWith(QLatin1Char('/')))
        prefix.remove(0, 1);
    while (prefix.endsWith(QLatin1Char('/')))
        prefix.chop(1);
    return prefix.isEmpty() ? QString() : prefix + QLatin1Char('/');
}

QString S3Store::libraryPrefix() const
{
    return rootPrefix() + QStringLiteral("library/");
}

QString S3Store::thumbsPrefix() const
{
    return rootPrefix() + QStringLiteral("thumbs/");
}

QString S3Store::objectKey(const QString &relPath) const
{
    return libraryPrefix() + relPath;
}

S3Store::Target S3Store::target(const QString &key, const SigV4Query &query) const
{
    QUrl endpoint(m_config.endpoint);
    if (endpoint.scheme().isEmpty())
        endpoint = QUrl(QStringLiteral("https://") + m_config.endpoint);

    Target t;
    t.host = endpoint.host().toUtf8();
    const int port = endpoint.port();
    const bool defaultPort = port == -1
                             || (endpoint.scheme() == QLatin1String("https") && port == 443)
                             || (endpoint.scheme() == QLatin1String("http") && port == 80);
    if (!defaultPort)
        t.host += ':' + QByteArray::number(port);

    t.encodedPath = '/' + sigV4Encode(m_config.bucket);
    if (!key.isEmpty())
        t.encodedPath += '/' + sigV4Encode(key, /*keepSlash=*/true);

    QByteArray encoded = endpoint.scheme().toUtf8() + "://" + t.host + t.encodedPath;
    if (!query.isEmpty())
        encoded += '?' + sigV4CanonicalQuery(query);
    t.url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    return t;
}

BlockingHttp::Response S3Store::call(const QByteArray &method, const QString &key,
                                     const SigV4Query &query, SigV4Headers headers,
                                     QIODevice *body, const QByteArray &payloadHash,
                                     QIODevice *sink, const ByteProgress &progress)
{
    const Target t = target(key, query);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    headers.append({"host", t.host});
    headers.append({"x-amz-date", sigV4AmzDate(now)});
    headers.append({"x-amz-content-sha256", payloadHash});

    const SigV4Credentials credentials{m_config.accessKey, m_config.secretKey, m_config.region};
    const QByteArray authorization =
        sigV4Authorization(credentials, method, t.encodedPath, query, headers, payloadHash, now);

    QNetworkRequest request(t.url);
    for (const auto &[name, value] : headers) {
        if (name != "host")
            request.setRawHeader(name, value);
    }
    request.setRawHeader("Authorization", authorization);
    if (body)
        request.setHeader(QNetworkRequest::ContentLengthHeader, body->size());

    return m_http.send(request, method, body, {}, sink, progress,
                       [this] { return cancelled(); });
}

bool S3Store::fail(const QString &what, const BlockingHttp::Response &response)
{
    QString detail = response.error;
    if (detail.isEmpty())
        detail = s3ErrorText(response.body);
    if (detail.isEmpty())
        detail = QStringLiteral("HTTP %1").arg(response.status);
    m_error = QStringLiteral("%1: %2").arg(what, detail);
    return false;
}

bool S3Store::testConnection()
{
    if (!m_config.isComplete()) {
        m_error = QStringLiteral("The endpoint, bucket, access key and secret are all needed");
        return false;
    }
    const SigV4Query query{{QStringLiteral("list-type"), QStringLiteral("2")},
                           {QStringLiteral("max-keys"), QStringLiteral("1")},
                           {QStringLiteral("prefix"), libraryPrefix()}};
    const auto response = call("GET", {}, query, {}, nullptr, kEmptyPayloadHash);
    if (!response.ok())
        return fail(QStringLiteral("Couldn't open the bucket"), response);
    return true;
}

std::optional<QList<S3Store::Listed>> S3Store::listUnder(const QString &prefix)
{
    QList<Listed> entries;
    QString continuation;
    do {
        SigV4Query query{{QStringLiteral("list-type"), QStringLiteral("2")},
                         {QStringLiteral("max-keys"), QStringLiteral("1000")},
                         {QStringLiteral("prefix"), prefix}};
        if (!continuation.isEmpty())
            query.append({QStringLiteral("continuation-token"), continuation});
        const auto response = call("GET", {}, query, {}, nullptr, kEmptyPayloadHash);
        if (!response.ok()) {
            fail(QStringLiteral("Couldn't list what's online"), response);
            return std::nullopt;
        }

        continuation.clear();
        bool truncated = false;
        QXmlStreamReader xml(response.body);
        Listed entry;
        bool inContents = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                const auto name = xml.name();
                if (name == QLatin1String("Contents")) {
                    inContents = true;
                    entry = Listed();
                } else if (inContents && name == QLatin1String("Key")) {
                    entry.key = xml.readElementText();
                } else if (inContents && name == QLatin1String("Size")) {
                    entry.size = xml.readElementText().toLongLong();
                } else if (inContents && name == QLatin1String("ETag")) {
                    entry.etag = stripQuotes(xml.readElementText());
                } else if (!inContents && name == QLatin1String("IsTruncated")) {
                    truncated = xml.readElementText() == QLatin1String("true");
                } else if (!inContents && name == QLatin1String("NextContinuationToken")) {
                    continuation = xml.readElementText();
                }
            } else if (xml.isEndElement() && xml.name() == QLatin1String("Contents")) {
                inContents = false;
                if (entry.key.startsWith(prefix) && !entry.key.endsWith(QLatin1Char('/')))
                    entries.append(entry);
            }
        }
        if (xml.hasError()) {
            m_error = QStringLiteral("Couldn't read the bucket listing: %1").arg(xml.errorString());
            return std::nullopt;
        }
        if (!truncated)
            continuation.clear();
        if (cancelled()) {
            m_error = QStringLiteral("Cancelled");
            return std::nullopt;
        }
    } while (!continuation.isEmpty());
    return entries;
}

std::optional<QList<RemoteEntry>> S3Store::list()
{
    const QString prefix = libraryPrefix();
    const auto listed = listUnder(prefix);
    if (!listed)
        return std::nullopt;
    QList<RemoteEntry> out;
    out.reserve(listed->size());
    for (const Listed &l : *listed) {
        RemoteEntry e;
        e.remoteId = l.key;
        e.relPath = l.key.mid(prefix.size());
        e.size = l.size;
        e.version = l.etag;
        e.md5 = md5FromEtag(l.etag);
        out.append(e);
    }
    return out;
}

std::optional<QSet<QString>> S3Store::listThumbnails()
{
    const QString prefix = thumbsPrefix();
    const auto listed = listUnder(prefix);
    if (!listed)
        return std::nullopt;
    QSet<QString> out;
    for (const Listed &l : *listed) {
        QString name = l.key.mid(prefix.size());
        if (name.endsWith(QLatin1String(".jpg")))
            name.chop(4);
        out.insert(name);
    }
    return out;
}

bool S3Store::putThumbnail(const QString &name, const QByteArray &jpeg)
{
    QBuffer body;
    body.setData(jpeg);
    body.open(QIODevice::ReadOnly);
    const QByteArray hash = QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex();
    const SigV4Headers headers{{"content-type", "image/jpeg"}};
    const auto response = call("PUT", thumbsPrefix() + name + QStringLiteral(".jpg"), {}, headers,
                               &body, hash);
    if (!response.ok())
        return fail(QStringLiteral("Couldn't upload a thumbnail"), response);
    return true;
}

std::optional<QByteArray> S3Store::getThumbnail(const QString &name)
{
    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    const auto response = call("GET", thumbsPrefix() + name + QStringLiteral(".jpg"), {}, {},
                               nullptr, kEmptyPayloadHash, &sink);
    if (!response.ok()) {
        fail(QStringLiteral("Couldn't download a thumbnail"), response);
        return std::nullopt;
    }
    return sink.data();
}

bool S3Store::describe(RemoteEntry &entry)
{
    const QString key = entry.remoteId.isEmpty() ? objectKey(entry.relPath) : entry.remoteId;
    const auto response = call("HEAD", key, {}, {}, nullptr, kEmptyPayloadHash);
    if (!response.ok())
        return fail(QStringLiteral("Couldn't look up %1").arg(entry.relPath), response);
    entry.version = stripQuotes(QString::fromLatin1(response.header("ETag")));
    if (entry.md5.isEmpty())
        entry.md5 = md5FromEtag(entry.version);
    const QByteArray sha = response.header("x-amz-meta-sha256");
    if (!sha.isEmpty())
        entry.sha256 = QString::fromLatin1(sha).toLower();
    const QByteArray md5 = response.header("x-amz-meta-md5");
    if (!md5.isEmpty())
        entry.md5 = QString::fromLatin1(md5).toLower();
    const QByteArray by = response.header("x-amz-meta-uploaded-by");
    if (!by.isEmpty())
        entry.uploadedBy = QString::fromUtf8(QByteArray::fromPercentEncoding(by));
    return true;
}

bool S3Store::download(const RemoteEntry &entry, const QString &localPath,
                       const ByteProgress &progress)
{
    QFile file(localPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_error = QStringLiteral("Couldn't write %1: %2").arg(localPath, file.errorString());
        return false;
    }
    const QString key = entry.remoteId.isEmpty() ? objectKey(entry.relPath) : entry.remoteId;
    const auto response = call("GET", key, {}, {}, nullptr, kEmptyPayloadHash, &file, progress);
    file.close();
    if (!response.ok())
        return fail(QStringLiteral("Couldn't download %1").arg(entry.relPath), response);
    return true;
}

std::optional<RemoteEntry> S3Store::upload(const QString &localPath, const QString &relPath,
                                           const QString &md5, const QString &sha256,
                                           const QString &uploadedBy,
                                           const ByteProgress &progress)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("Couldn't read %1: %2").arg(localPath, file.errorString());
        return std::nullopt;
    }
    const QString key = objectKey(relPath);
    const SigV4Headers headers{
        {"content-type", "application/octet-stream"},
        {"content-md5", QByteArray::fromHex(md5.toLatin1()).toBase64()},
        {"x-amz-meta-sha256", sha256.toLatin1()},
        {"x-amz-meta-uploaded-by", uploadedBy.toUtf8().toPercentEncoding(" ")},
    };
    const auto response = call("PUT", key, {}, headers, &file, sha256.toLatin1(), nullptr, progress);
    if (!response.ok()) {
        fail(QStringLiteral("Couldn't upload %1").arg(relPath), response);
        return std::nullopt;
    }

    RemoteEntry entry;
    entry.relPath = relPath;
    entry.remoteId = key;
    entry.size = file.size();
    entry.version = stripQuotes(QString::fromLatin1(response.header("ETag")));
    entry.md5 = md5.toLower();
    entry.sha256 = sha256.toLower();
    entry.uploadedBy = uploadedBy;
    return entry;
}

} // namespace pl::libsync
