#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace pl::organise {

// One shot's files going into a folder (made if missing) inside the download
// folder.
struct ShotMove
{
    QString baseName;     // DSC_1234
    QStringList files;    // absolute paths, all starting with baseName
    QString folderName;   // relative to the download folder
};

// Moves shots into folders inside a download folder -- by renaming, never
// copying -- and can put them all back. Nothing is ever overwritten or
// deleted: a name already taken in the destination gets " (2)", " (3)"...
// after the shot's base name, the same for every file of the shot so they
// still go together. Every move is written to kLogFile as it happens, so an
// interrupted run can still be undone.
class OrganiseMover
{
public:
    static constexpr const char *kLogFile = ".photolife-organise-log.json";

    struct Result
    {
        int moved = 0;          // shots
        QStringList failures;   // "<file>: <why>"
    };

    static Result execute(const QString &folder, const QList<ShotMove> &shots);

    // Whether the folder has a last organise to undo.
    static bool canUndo(const QString &folder);
    // Moves everything the last organise moved back where it was (each file
    // only if it's still where it was put and its old place is free), removes
    // the folders it made if they're empty again, and forgets the log.
    static Result undo(const QString &folder);
};

} // namespace pl::organise
