#include "util/Csv.h"

namespace pl::util {

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

QString csvField(const QString &value)
{
    QString v = value;
    v.replace(QLatin1String("\r\n"), QLatin1String(" "));
    v.replace(QLatin1Char('\n'), QLatin1Char(' '));
    v.replace(QLatin1Char('\r'), QLatin1Char(' '));
    const bool quote = v.contains(QLatin1Char(',')) || v.contains(QLatin1Char('"'))
                       || (!v.isEmpty() && (v.front().isSpace() || v.back().isSpace()));
    if (!quote)
        return v;
    v.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + v + QLatin1Char('"');
}

} // namespace pl::util
