#pragma once

#include <QList>
#include <QString>

class QByteArray;
class QIODevice;

namespace pl::checklist {

// One row of a regional checklist CSV: a name and (optionally) a conservation
// status. The raw status text is kept as-is; statusFolded is lower-cased for
// comparison.
struct ChecklistEntry
{
    QString name;
    QString status;
    QString statusFolded;

    bool operator==(const ChecklistEntry &) const = default;
};

// Parses a "<name>,<status>" checklist CSV. Tolerates a UTF-8 BOM, CRLF or LF
// line endings, quoted fields, a header row, blank lines, and extra columns
// (only the first two are used). Rows with an empty name are dropped.
QList<ChecklistEntry> parseChecklistCsv(const QByteArray &bytes);
QList<ChecklistEntry> parseChecklistCsv(QIODevice &device);

} // namespace pl::checklist
