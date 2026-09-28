#include "checklist/ChecklistParser.h"

#include "util/Csv.h"

#include <QIODevice>
#include <QRegularExpression>
#include <QStringList>

#include <optional>

namespace pl::checklist {

QString originFromValue(const QString &raw)
{
    const QString v = raw.trimmed().toLower();
    if (v.isEmpty())
        return {};
    if (v == QLatin1String("*") || v.startsWith(QLatin1String("introduc"))
        || v.startsWith(QLatin1String("exotic")) || v.startsWith(QLatin1String("naturali"))
        || v.startsWith(QLatin1String("alien")) || v.startsWith(QLatin1String("non-native"))
        || v == QLatin1String("weed") || v == QLatin1String("i"))
        return QStringLiteral("introduced");
    if (v.startsWith(QLatin1String("native")) || v.startsWith(QLatin1String("indigenous"))
        || v.startsWith(QLatin1String("endemic")) || v == QLatin1String("n"))
        return QStringLiteral("native");
    return {};
}

bool isNameHeader(const QString &h)
{
    return h == QLatin1String("name") || h == QLatin1String("scientific name")
           || h == QLatin1String("scientific_name") || h == QLatin1String("taxon")
           || h == QLatin1String("taxon name") || h == QLatin1String("species");
}

bool isOriginHeader(const QString &h)
{
    return h.contains(QLatin1String("origin")) || h.contains(QLatin1String("nativ"))
           || h.contains(QLatin1String("introduc")) || h.contains(QLatin1String("establishment"));
}

namespace {

// Column positions, from a header row or the plain "<name>,<status>" layout.
struct Columns
{
    int name = 0;
    int status = 1;
    int origin = -1;          // -1: none named -- try the third column's values
    bool fromHeader = false;
};

std::optional<Columns> headerColumns(const QStringList &fields)
{
    Columns c;
    c.name = c.status = -1;
    for (int i = 0; i < fields.size(); ++i) {
        const QString h = fields.at(i).trimmed().toLower();
        if (c.name < 0 && isNameHeader(h))
            c.name = i;
        else if (c.status < 0 && h.contains(QLatin1String("status")))
            c.status = i;
        else if (c.origin < 0 && isOriginHeader(h))
            c.origin = i;
    }
    if (c.name < 0)
        return std::nullopt;
    c.fromHeader = true;
    return c;
}

} // namespace

QList<ChecklistEntry> parseChecklistCsv(const QByteArray &bytes)
{
    QString text = QString::fromUtf8(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);   // strip BOM

    QList<ChecklistEntry> entries;
    bool firstRow = true;
    Columns cols;

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty())
            continue;

        const QStringList fields = line.contains(QLatin1Char('"'))
                                       ? util::splitCsvLine(line)
                                       : line.split(QLatin1Char(','));

        if (firstRow) {
            firstRow = false;
            if (const auto header = headerColumns(fields)) {
                cols = *header;
                continue;
            }
        }

        QString name = fields.value(cols.name).trimmed();
        const QString status = cols.status >= 0 ? fields.value(cols.status).trimmed() : QString();
        QString origin;
        if (cols.origin >= 0)
            origin = originFromValue(fields.value(cols.origin));
        else if (!cols.fromHeader)
            origin = originFromValue(fields.value(2));
        if (name.startsWith(QLatin1Char('*'))) {
            name = name.mid(1).trimmed();
            if (origin.isEmpty())
                origin = QStringLiteral("introduced");
        }

        if (name.isEmpty())
            continue;

        ChecklistEntry e;
        e.name = name;
        e.status = status;
        e.statusFolded = status.toLower();
        e.origin = origin;
        entries.append(e);
    }

    return entries;
}

QList<ChecklistEntry> parseChecklistCsv(QIODevice &device)
{
    return parseChecklistCsv(device.readAll());
}

} // namespace pl::checklist
