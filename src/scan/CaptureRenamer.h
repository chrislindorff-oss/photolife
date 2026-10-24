#pragma once

#include <QString>
#include <QStringList>

namespace pl::scan {

// Renames a catalogued photo's files in the library -- its RAW and JPEG
// renditions and any editing sidecars (.xmp, .dop...) -- to a new name, and
// updates the catalogue to match, so nothing is lost (matches, best shots and
// coordinates are kept by the capture, not the file name). Only the name part
// of a library-style file name changes: "Wasp Gall 1 - Sailors Tk, Steiglitz
// 14-6-2020 (1).JPG" given "Apiomorpha" becomes "Apiomorpha - Sailors Tk,
// Steiglitz 14-6-2020 (1).JPG". Nothing is ever overwritten: a name already
// taken in the folder gets the next free "(n)". If any file can't be renamed,
// those already renamed are put back. Never deletes anything.
class CaptureRenamer
{
public:
    explicit CaptureRenamer(QString connectionName);

    // The new stem for `baseName` (parsed name `nameText`, as the scanner
    // read it) given the name `name`: `name` in place of the name part, the
    // rest kept. Safe for the file system.
    static QString renamedStem(const QString &baseName, const QString &nameText, const QString &name);

    struct Result
    {
        bool renamed = false;
        QString newBaseName;
        QStringList files;   // new paths
        QString error;       // why not, when it wasn't
    };
    // Renames capture `captureId` for `name`. Photos whose files aren't all
    // on this computer (kept online) are left as they are.
    Result rename(qint64 captureId, const QString &name);

private:
    QString m_connectionName;
};

} // namespace pl::scan
