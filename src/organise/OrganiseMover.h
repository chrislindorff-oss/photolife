#pragma once

#include <QDateTime>
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
    QString newBaseName;  // renamed to this (each file keeps what follows the
                          // base name: ".NEF", ".NEF.xmp"); empty keeps it
};

// Moves shots into folders inside a download folder -- by renaming, never
// copying -- optionally giving them new names, and can put them all back
// under their old ones. Nothing is ever overwritten or deleted: a name
// already taken in the destination (by a file of any extension) gets " (2)",
// " (3)"... after the shot's base name, the same for every file of the shot
// so they still go together. Every move is written to kLogFile as it happens, so an
// interrupted run can still be undone. Each organise is kept there as its
// own entry, and undo() takes back the most recent one -- all of it, every
// folder -- so earlier organises can be undone in turn.
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

    // What undo() would put back: the most recent organise in the folder.
    struct UndoPreview
    {
        QDateTime when;
        struct Folder
        {
            QString name;    // relative to the download folder
            int shots = 0;   // photos (RAW+JPEG+sidecars count once)
            int files = 0;
        };
        QList<Folder> folders;   // in the order they were organised
        int shots = 0;
        int files = 0;
        int missing = 0;         // files no longer where they were put: left alone
        int earlier = 0;         // older organises that can be undone after it
    };
    // nullopt-like: an empty `folders` when there's nothing to undo.
    static UndoPreview previewUndo(const QString &folder);

    // Whether the folder has an organise to undo.
    static bool canUndo(const QString &folder);
    // Moves everything the most recent organise moved back where it was (each
    // file only if it's still where it was put and its old place is free),
    // removes the folders it made if they're empty again, and drops it from
    // the log.
    static Result undo(const QString &folder);
};

} // namespace pl::organise
