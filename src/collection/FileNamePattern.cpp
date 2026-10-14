#include "collection/FileNamePattern.h"

#include "util/PathSanitize.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>

namespace pl::collection {
namespace {

struct TokenInfo
{
    NameToken token;
    const char *key;     // stable identifier stored in JSON
    const char *label;   // shown in the naming dialog
};

constexpr TokenInfo kTokens[] = {
    {NameToken::ScientificName, "scientificName", QT_TRANSLATE_NOOP("FileNamePattern", "Scientific name")},
    {NameToken::CommonName, "commonName", QT_TRANSLATE_NOOP("FileNamePattern", "Common name")},
    {NameToken::DateTaken, "date", QT_TRANSLATE_NOOP("FileNamePattern", "Date taken")},
    {NameToken::TimeTaken, "time", QT_TRANSLATE_NOOP("FileNamePattern", "Time taken")},
    {NameToken::Year, "year", QT_TRANSLATE_NOOP("FileNamePattern", "Year taken")},
    {NameToken::Town, "town", QT_TRANSLATE_NOOP("FileNamePattern", "Town")},
    {NameToken::State, "state", QT_TRANSLATE_NOOP("FileNamePattern", "State")},
    {NameToken::Country, "country", QT_TRANSLATE_NOOP("FileNamePattern", "Country")},
    {NameToken::Author, "author", QT_TRANSLATE_NOOP("FileNamePattern", "Author")},
    {NameToken::OriginalName, "originalName", QT_TRANSLATE_NOOP("FileNamePattern", "Original filename")},
    {NameToken::Sequence, "sequence", QT_TRANSLATE_NOOP("FileNamePattern", "Sequence number")},
    {NameToken::Text, "text", QT_TRANSLATE_NOOP("FileNamePattern", "Custom text")},
};

const TokenInfo *infoFor(NameToken token)
{
    for (const TokenInfo &info : kTokens) {
        if (info.token == token)
            return &info;
    }
    return nullptr;
}

const TokenInfo *infoFor(const QString &key)
{
    for (const TokenInfo &info : kTokens) {
        if (key == QLatin1String(info.key))
            return &info;
    }
    return nullptr;
}

QString rawValue(const NamePart &part, const FileNamePattern &pattern, const NameFields &f)
{
    switch (part.token) {
    case NameToken::ScientificName: return f.scientificName;
    case NameToken::CommonName: return f.commonName;
    case NameToken::DateTaken:
        return f.captured.isValid() ? f.captured.toString(pattern.dateFormat) : QString();
    case NameToken::TimeTaken:
        return f.captured.isValid() ? f.captured.toString(QStringLiteral("HHmmss")) : QString();
    case NameToken::Year:
        return f.captured.isValid() ? QString::number(f.captured.date().year()) : QString();
    case NameToken::Town: return f.town;
    case NameToken::State: return f.state;
    case NameToken::Country: return f.country;
    case NameToken::Author: return f.author;
    case NameToken::OriginalName: return f.originalStem;
    case NameToken::Sequence:
        return f.sequence > 0 ? QStringLiteral("%1").arg(f.sequence, 3, 10, QLatin1Char('0'))
                              : QString();
    case NameToken::Text: return part.text;
    }
    return {};
}

} // namespace

bool FileNamePattern::uses(NameToken token) const
{
    for (const NamePart &part : parts) {
        if (part.token == token)
            return true;
    }
    return false;
}

QString FileNamePattern::render(const NameFields &fields) const
{
    const QString stem = renderParts(fields);
    return stem.isEmpty() ? fields.originalStem : stem;
}

QString FileNamePattern::renderParts(const NameFields &fields) const
{
    const bool replaceSpaces = spacesToSeparator && !separator.isEmpty()
                               && separator != QStringLiteral(" ");
    QStringList pieces;
    for (const NamePart &part : parts) {
        QString value = util::sanitizeFilenameComponent(rawValue(part, *this, fields));
        if (value.isEmpty())
            continue;
        if (replaceSpaces)
            value.replace(QLatin1Char(' '), separator);
        pieces << value;
    }

    // The separator itself must be filename-safe too; sanitizing it alone
    // would collapse it to nothing, so strip illegal characters directly.
    QString sep = separator;
    sep.remove(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")));

    QString stem = pieces.join(sep).left(kMaxStemLength).trimmed();
    // Windows won't accept a name ending in a dot.
    while (stem.endsWith(QLatin1Char('.')))
        stem.chop(1);
    return stem;
}

QString FileNamePattern::describe() const
{
    if (parts.isEmpty())
        return QCoreApplication::translate("FileNamePattern", "Original filename");
    QStringList labels;
    for (const NamePart &part : parts) {
        labels << (part.token == NameToken::Text ? QStringLiteral("\"%1\"").arg(part.text)
                                                 : tokenLabel(part.token));
    }
    const QString sep = separator.isEmpty() ? QStringLiteral(" + ")
                                            : QStringLiteral(" %1 ").arg(separator);
    return labels.join(sep);
}

QJsonObject FileNamePattern::toJson() const
{
    QJsonArray array;
    for (const NamePart &part : parts) {
        QJsonObject obj;
        obj.insert(QStringLiteral("token"), QLatin1String(infoFor(part.token)->key));
        if (part.token == NameToken::Text)
            obj.insert(QStringLiteral("text"), part.text);
        array.append(obj);
    }
    QJsonObject json;
    json.insert(QStringLiteral("parts"), array);
    json.insert(QStringLiteral("separator"), separator);
    json.insert(QStringLiteral("dateFormat"), dateFormat);
    json.insert(QStringLiteral("spacesToSeparator"), spacesToSeparator);
    return json;
}

FileNamePattern FileNamePattern::fromJson(const QJsonObject &json)
{
    FileNamePattern pattern;
    for (const QJsonValue &value : json.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject obj = value.toObject();
        const TokenInfo *info = infoFor(obj.value(QStringLiteral("token")).toString());
        if (!info)
            continue;   // a token from a newer version; skip rather than fail
        pattern.parts.append({info->token, obj.value(QStringLiteral("text")).toString()});
    }
    if (json.contains(QStringLiteral("separator")))
        pattern.separator = json.value(QStringLiteral("separator")).toString();
    const QString dateFormat = json.value(QStringLiteral("dateFormat")).toString();
    if (!dateFormat.isEmpty())
        pattern.dateFormat = dateFormat;
    pattern.spacesToSeparator = json.value(QStringLiteral("spacesToSeparator")).toBool();
    return pattern;
}

QList<NameToken> FileNamePattern::availableTokens()
{
    QList<NameToken> tokens;
    for (const TokenInfo &info : kTokens)
        tokens << info.token;
    return tokens;
}

QString FileNamePattern::tokenLabel(NameToken token)
{
    const TokenInfo *info = infoFor(token);
    return info ? QCoreApplication::translate("FileNamePattern", info->label) : QString();
}

LocalityParts splitLocality(const QString &locality)
{
    QStringList pieces;
    for (const QString &piece : locality.split(QLatin1Char(','))) {
        if (!piece.trimmed().isEmpty())
            pieces << piece.trimmed();
    }
    LocalityParts parts;
    if (pieces.isEmpty())
        return parts;
    parts.country = pieces.takeLast();
    if (!pieces.isEmpty())
        parts.state = pieces.takeLast();
    if (!pieces.isEmpty())
        parts.town = pieces.join(QStringLiteral(", "));
    return parts;
}

QDateTime parseCapturedOn(const QString &text)
{
    if (text.isEmpty())
        return {};
    QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
    if (!dt.isValid())
        dt = QDateTime(QDate::fromString(text.left(10), Qt::ISODate), QTime(0, 0));
    return dt;
}

} // namespace pl::collection
