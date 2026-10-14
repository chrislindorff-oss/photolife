#include "libsync/SigV4.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <algorithm>

namespace pl::libsync {

namespace {

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// Header values are trimmed and runs of spaces collapsed to one.
QByteArray canonicalValue(const QByteArray &value)
{
    return value.simplified();
}

} // namespace

QByteArray sigV4Encode(const QString &value, bool keepSlash)
{
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray utf8 = value.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() * 3);
    for (const char ch : utf8) {
        const auto c = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
                                || c == '~';
        if (unreserved || (keepSlash && c == '/')) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

QByteArray sigV4CanonicalQuery(const SigV4Query &query)
{
    QList<QPair<QByteArray, QByteArray>> encoded;
    encoded.reserve(query.size());
    for (const auto &[key, value] : query)
        encoded.append({sigV4Encode(key), sigV4Encode(value)});
    std::sort(encoded.begin(), encoded.end());
    QByteArray out;
    for (const auto &[key, value] : encoded) {
        if (!out.isEmpty())
            out += '&';
        out += key + '=' + value;
    }
    return out;
}

QByteArray sigV4AmzDate(const QDateTime &utc)
{
    return utc.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

QByteArray sigV4Authorization(const SigV4Credentials &credentials, const QByteArray &method,
                              const QByteArray &encodedPath, const SigV4Query &query,
                              const SigV4Headers &headers, const QByteArray &payloadHash,
                              const QDateTime &utc)
{
    QList<QPair<QByteArray, QByteArray>> canonical;
    canonical.reserve(headers.size());
    for (const auto &[name, value] : headers)
        canonical.append({name.toLower().trimmed(), canonicalValue(value)});
    std::sort(canonical.begin(), canonical.end());

    QByteArray canonicalHeaders;
    QByteArray signedHeaders;
    for (const auto &[name, value] : canonical) {
        canonicalHeaders += name + ':' + value + '\n';
        if (!signedHeaders.isEmpty())
            signedHeaders += ';';
        signedHeaders += name;
    }

    const QByteArray canonicalRequest = method + '\n' + encodedPath + '\n'
                                        + sigV4CanonicalQuery(query) + '\n' + canonicalHeaders
                                        + '\n' + signedHeaders + '\n' + payloadHash;

    const QByteArray amzDate = sigV4AmzDate(utc);
    const QByteArray date = amzDate.left(8);
    const QByteArray scope = date + '/' + credentials.region.toUtf8() + '/'
                             + credentials.service.toUtf8() + "/aws4_request";
    const QByteArray stringToSign = "AWS4-HMAC-SHA256\n" + amzDate + '\n' + scope + '\n'
                                    + sha256Hex(canonicalRequest);

    const QByteArray kDate = hmac("AWS4" + credentials.secretKey.toUtf8(), date);
    const QByteArray kRegion = hmac(kDate, credentials.region.toUtf8());
    const QByteArray kService = hmac(kRegion, credentials.service.toUtf8());
    const QByteArray kSigning = hmac(kService, "aws4_request");
    const QByteArray signature = hmac(kSigning, stringToSign).toHex();

    return "AWS4-HMAC-SHA256 Credential=" + credentials.accessKey.toUtf8() + '/' + scope
           + ", SignedHeaders=" + signedHeaders + ", Signature=" + signature;
}

} // namespace pl::libsync
