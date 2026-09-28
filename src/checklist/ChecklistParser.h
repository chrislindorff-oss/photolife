#pragma once

#include <QList>
#include <QString>

class QByteArray;
class QIODevice;

namespace pl::checklist {

// One row of a regional checklist CSV: a name, (optionally) a conservation
// status, and (optionally) whether the taxon is native or introduced in the
// checklist's region. The raw status text is kept as-is; statusFolded is
// lower-cased for comparison.
struct ChecklistEntry
{
    QString name;
    QString status;
    QString statusFolded;
    QString origin;   // "introduced" | "native" | "" (not given)

    bool operator==(const ChecklistEntry &) const = default;
};

// Parses a "<name>,<status>" checklist CSV. Tolerates a UTF-8 BOM, CRLF or LF
// line endings, quoted fields, a header row, blank lines, and extra columns.
// Rows with an empty name are dropped.
//
// Origin is read, when present, from: a header-named column ("Origin",
// "Native/Introduced", "Establishment" ...; a header also lets the name and
// status columns sit anywhere), else a third column holding a recognisable
// value ("*", "introduced", "naturalised", "exotic", "native" ...), else a
// "*" before the name (the VicFlora/VBA convention for introduced taxa,
// stripped from the name).
QList<ChecklistEntry> parseChecklistCsv(const QByteArray &bytes);
QList<ChecklistEntry> parseChecklistCsv(QIODevice &device);

// "introduced" / "native" for a recognised origin value ("*", "exotic",
// "naturalised", "indigenous" ...), else "". Shared with the reference-tree
// file reader (treefile/TreeFile).
QString originFromValue(const QString &raw);

// Header recognisers, given a trimmed, lower-cased header cell: a name column
// ("name", "scientific name", "taxon", "species" ...) and an origin column
// ("origin", "native/introduced", "establishment" ...).
bool isNameHeader(const QString &h);
bool isOriginHeader(const QString &h);

} // namespace pl::checklist
