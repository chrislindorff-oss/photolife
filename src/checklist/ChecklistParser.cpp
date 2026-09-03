#include "checklist/ChecklistParser.h"

#include <QIODevice>
#include <QRegularExpression>
#include <QStringList>

namespace pl::checklist {
namespace {

// Splits one CSV line into fields, honouring double-quoted fields with ""
// escapes. Only used for lines that actually contain a quote; the common case
// (no quotes) takes the fast path in parse().
QStringList splitCsvLine(const QString &line)
{
    QStringList fields;
    QString field;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            fields << field;
            field.clear();
        } else {
            field += c;
        }
    }
    fields << field;
    return fields;
}

bool looksLikeHeader(const QString &name, const QString &status)
{
    const QString n = name.toLower();
    const QString s = status.toLower();
    return (n == QLatin1String("name") || n == QLatin1String("scientific name")
            || n == QLatin1String("taxon") || n == QLatin1String("species"))
           && (s.isEmpty() || s.contains(QLatin1String("status")));
}

} // namespace

QList<ChecklistEntry> parseChecklistCsv(const QByteArray &bytes)
{
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);   // strip BOM

    QList<ChecklistEntry> entries;
    bool firstRow = true;

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    for (const QString &rawLine : lines) {
        const QString line = rawLine;
        if (line.trimmed().isEmpty())
            continue;

        QString name;
        QString status;
        if (line.contains(QLatin1Char('"'))) {
            const QStringList fields = splitCsvLine(line);
            name = fields.value(0).trimmed();
            status = fields.value(1).trimmed();
        } else {
            const int comma = line.indexOf(QLatin1Char(','));
            if (comma < 0) {
                name = line.trimmed();
            } else {
                name = line.left(comma).trimmed();
                // second field runs to the next comma (extra columns ignored)
                const int next = line.indexOf(QLatin1Char(','), comma + 1);
                status = (next < 0 ? line.mid(comma + 1) : line.mid(comma + 1, next - comma - 1))
                             .trimmed();
            }
        }

        if (firstRow) {
            firstRow = false;
            if (looksLikeHeader(name, status))
                continue;
        }

        if (name.isEmpty())
            continue;

        ChecklistEntry e;
        e.name = name;
        e.status = status;
        e.statusFolded = status.toLower();
        entries.append(e);
    }

    return entries;
}

QList<ChecklistEntry> parseChecklistCsv(QIODevice &device)
{
    return parseChecklistCsv(device.readAll());
}

} // namespace pl::checklist
