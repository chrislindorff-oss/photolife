#pragma once

#include "collection/FileNamePattern.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pl::scan {

// Renaming library photos on the drive, as the library's Rename Files on
// Drive does: works out every photo's new name before anything is touched,
// so they can be shown, and renames a photo's folder. The files themselves
// are renamed one photo at a time by CaptureRenamer::renameTo().
class LibraryRenamer
{
public:
    enum class Mode {
        NamePart,   // a new name in place of the name part; the rest kept
        Pattern,    // the whole name built from parts, as Export does
    };

    // In NamePart mode, what the name part is: the name typed, or each
    // photo's own identified species in one of these forms.
    enum class NameForm {
        Typed,
        Common,             // Flame Robin
        Scientific,         // Petroica phoenicea
        CommonScientific,   // Flame Robin (Petroica phoenicea)
        ScientificCommon,   // Petroica phoenicea (Flame Robin)
    };
    // A species' name in `form`; the scientific name alone when it has no
    // common one. Empty for NameForm::Typed or no species.
    static QString speciesName(const QString &scientific, const QString &common, NameForm form);

    struct Photo
    {
        qint64 captureId = 0;
        int folderId = 0;
        QString folder;         // its folder's path
        QString oldStem;        // its base name now
        QString nameText;       // the name part, as the scanner read it
        QString newStem;        // after plan(): what it will be called
        bool onDisk = true;     // all its files are on this computer
        QString suffix;         // its preview file's extension, for showing
        QString scientificName; // of its identified species, if any
        QString commonName;
        collection::NameFields fields;
    };

    // The photos, with what their names can be built from, in the order
    // they're named: by folder, then when they were taken.
    static QList<Photo> load(const QString &connectionName, const QList<qint64> &captureIds);

    // Sets each photo's newStem, as CaptureRenamer::renameTo() will name it
    // taking them in order: a name already taken in the folder (by another
    // file, or a photo named before it) gets the next free "(n)". A photo not
    // on this computer keeps its name, as does one without a species when
    // `form` names photos by theirs.
    static void plan(QList<Photo> &photos, Mode mode, const QString &name,
                     const collection::FileNamePattern &pattern, const QString &author,
                     NameForm form = NameForm::Typed);

    struct FolderResult
    {
        bool renamed = false;
        QString newPath;
        QString error;   // why not, when it wasn't
    };
    // Renames folder `folderId` to `newName` in place (it stays in the same
    // parent) and updates the catalogue: the folder, the folders inside it
    // and every file's path. Refuses a folder that is, or holds, one of the
    // library's `roots`, a name already taken, and a folder with photos kept
    // online only. The folder is renamed back if the catalogue can't be updated.
    static FolderResult renameFolder(const QString &connectionName, int folderId, const QString &newName,
                                     const QStringList &roots);

    // `name` made safe for a file or folder name; empty when nothing's left.
    static QString cleanName(const QString &name);
};

} // namespace pl::scan
