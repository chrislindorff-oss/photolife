#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>

namespace pl::libsync {

// AWS Signature Version 4, as S3-compatible stores (Cloudflare R2, Backblaze
// B2, MinIO, AWS) require on every request. Pure functions, so they can be
// checked against AWS's published examples.
struct SigV4Credentials
{
    QString accessKey;
    QString secretKey;
    QString region;                       // "auto" for R2
    QString service = QStringLiteral("s3");
};

using SigV4Headers = QList<QPair<QByteArray, QByteArray>>;
using SigV4Query = QList<QPair<QString, QString>>;

// RFC 3986 percent-encoding as SigV4 wants it: everything but A-Z a-z 0-9
// - . _ ~ is encoded (upper-case hex); '/' is kept when `keepSlash`.
QByteArray sigV4Encode(const QString &value, bool keepSlash = false);

// The canonical (sorted, encoded) query string -- also what goes on the URL.
QByteArray sigV4CanonicalQuery(const SigV4Query &query);

// "20150830T123600Z" for `utc`.
QByteArray sigV4AmzDate(const QDateTime &utc);

// The Authorization header value for a request. `encodedPath` is the
// already-encoded path exactly as it goes on the URL; `headers` are every
// header to sign (they must include host and x-amz-date, and for S3
// x-amz-content-sha256); `payloadHash` is the hex SHA-256 of the body (or
// "UNSIGNED-PAYLOAD").
QByteArray sigV4Authorization(const SigV4Credentials &credentials, const QByteArray &method,
                              const QByteArray &encodedPath, const SigV4Query &query,
                              const SigV4Headers &headers, const QByteArray &payloadHash,
                              const QDateTime &utc);

} // namespace pl::libsync
