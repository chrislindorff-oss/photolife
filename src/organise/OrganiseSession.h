#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace pl::organise {

// One shot in a download folder: its RAW and/or JPEG renditions, and any
// sidecars (DSC_1234.xmp, DSC_1234.NEF.xmp, ...), which always move together.
struct Item
{
    QString baseName;       // DSC_1234
    QStringList files;      // absolute paths: renditions, then sidecars
    QStringList kinds;      // the renditions' extensions, upper-case ("NEF", "JPG")
    QString previewPath;    // the JPEG if there is one, else the RAW
    QDateTime taken;        // EXIF date taken, else the file's modification time
    bool takenFromExif = false;
    qint64 size = 0;        // of the first rendition: with mtime, recognises the shot again
    qint64 mtime = 0;
};

// A run of photos going into one folder.
struct FolderMove
{
    QString folderName;      // as it will be created (sanitised)
    QList<int> items;        // indexes into OrganiseSession::items()
};

// What committing would do.
struct OrganisePlan
{
    QList<FolderMove> folders;   // named runs, merged by name, first-named first
    QList<int> rejects;
    int unnamed = 0;             // photos before the first name: left where they are
    int photos() const;          // photos moving into named folders
};

// Sorting a download folder: the shots in it (top level only), in the order
// they were taken; the names given to the first photo of each taxon -- every
// photo up to the next named one belongs to that name -- and the rejects.
// No widgets, no catalogue: just the folder's files.
class OrganiseSession
{
public:
    static constexpr const char *kProgressFile = ".photolife-organise.json";
    static constexpr const char *kRejectsFolder = "Rejects";

    // The shots directly in `folder` (not its subfolders), sorted by when
    // they were taken, file name breaking ties. Reads each shot's EXIF date,
    // so it can take a moment: safe to call on a worker thread.
    static QList<Item> scanFolder(const QString &folder);

    void setItems(const QString &folder, QList<Item> items);
    QString folder() const { return m_folder; }
    const QList<Item> &items() const { return m_items; }
    int count() const { return int(m_items.size()); }

    // The name given at photo `i` (empty unless it starts a run).
    QString nameAt(int i) const { return m_names.value(i); }
    void setName(int i, const QString &name);   // empty clears it
    // The run photo `i` is in: the index of its named photo, or -1 before
    // the first name.
    int runStart(int i) const;
    QString runName(int i) const;
    // Which run (0, 1, 2...) photo `i` is in, -1 if none: for alternating colours.
    int runNumber(int i) const;

    bool isRejected(int i) const { return m_rejected.contains(i); }
    void setRejected(int i, bool rejected);

    // The names given so far, each once, most recent first.
    QStringList recentNames() const;

    OrganisePlan plan() const;

    // Names and rejects, kept in kProgressFile in the folder so a session
    // can be picked up again; a shot is recognised by its name, size and
    // modification time. Shots organised out of the folder keep theirs, so
    // an undone organise comes back named.
    bool saveProgress() const;
    void loadProgress();
    void clearProgress();

    // The folder name for a given name: sanitised for the file system.
    static QString folderNameFor(const QString &name);

private:
    QString m_folder;
    QList<Item> m_items;
    QHash<int, QString> m_names;
    QSet<int> m_rejected;
    QStringList m_nameOrder;   // names in the order given (for recentNames())
};

} // namespace pl::organise
