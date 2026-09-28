#pragma once

#include <QString>
#include <QStringList>

namespace pl::util {

// Splits one CSV line into fields, honouring double-quoted fields with ""
// escapes. Lines are split before this is called, so a quoted field cannot
// span lines.
QStringList splitCsvLine(const QString &line);

// One field as it should be written to a CSV line: quoted (with "" escapes)
// when it holds a comma, quote or leading/trailing space; line breaks become
// spaces, since splitCsvLine() can't read a field across lines.
QString csvField(const QString &value);

} // namespace pl::util
