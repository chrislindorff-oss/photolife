#include "inat/InatToken.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace pl::inat {

QString extractApiToken(const QString &text)
{
    // A JWT: three base64url parts, the first two JSON (so "eyJ..."), found
    // wherever it is in the text: Firefox shows the page's JSON as a table,
    // and Ctrl+A, Ctrl+C there copies `api_token<tab>"eyJ..."`.
    static const QRegularExpression jwt(QStringLiteral(R"((?<![\w-])eyJ[\w-]+\.eyJ[\w-]+\.[\w-]+(?![\w-]))"));
    return jwt.match(text).captured(0);
}

std::optional<QDateTime> apiTokenExpiry(const QString &token)
{
    const QStringList parts = token.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return std::nullopt;
    const QByteArray payload = QByteArray::fromBase64(
        parts.at(1).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonValue exp = QJsonDocument::fromJson(payload).object().value(QStringLiteral("exp"));
    if (!exp.isDouble())
        return std::nullopt;
    return QDateTime::fromSecsSinceEpoch(qint64(exp.toDouble())).toUTC();
}

} // namespace pl::inat
