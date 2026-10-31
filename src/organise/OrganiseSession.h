#pragma once

#include "geo/GeotagWriter.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

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
    std::optional<double> latitude;    // embedded GPS, decimal degrees
    std::optional<double> longitude;
    bool hasGps() const { return latitude.has_value() && longitude.has_value(); }
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
    // so it can take a moment: safe to call on a worker thread. `progress`
    // reports (0, 0) while finding photos, then (done, total) while reading them.
    static QList<Item> scanFolder(const QString &folder,
                                  const std::function<void(int, int)> &progress = {});

    // A big folder is read a page at a time: listFolder() finds the shots
    // quickly, without reading them -- each one's time is its file's, and
    // they're in that order -- then readMetadata() reads each page's EXIF
    // dates and GPS as it's needed. Both are safe on a worker thread.
    static constexpr int kPageSize = 250;
    static QList<Item> listFolder(const QString &folder);
    static void readMetadata(Item &item);
    static void sortByTaken(QList<Item> &items);
    // How many of `listed` to read first: a page, or enough to reach the
    // last photo the folder's saved progress names (and a page past it).
    static int firstPageSize(const QString &folder, const QList<Item> &listed);

    // `items` read; `pending` listed but not yet read, in order.
    void setItems(const QString &folder, QList<Item> items, QList<Item> pending = {});
    QString folder() const { return m_folder; }
    const QList<Item> &items() const { return m_items; }
    int count() const { return int(m_items.size()); }
    const QList<Item> &pending() const { return m_pending; }
    int pendingCount() const { return int(m_pending.size()); }
    // The front of pending(), now read: put in the order taken after the
    // photos already here (never among them -- names, rejects and the rest
    // are kept by position), with their saved progress. Returns how many.
    int appendItems(QList<Item> batch);

    // The name given at photo `i` (empty unless it starts a run).
    QString nameAt(int i) const { return m_names.value(i); }
    void setName(int i, const QString &name);   // empty clears it
    // Stop the current name from carrying past photo `i`.
    bool isNamingEndAt(int i) const { return m_namingEnds.contains(i); }
    void setNamingEndAt(int i, bool ends);
    // The run photo `i` is in: the index of its named photo, or -1 before
    // the first name.
    int runStart(int i) const;
    QString runName(int i) const;
    // Which run (0, 1, 2...) photo `i` is in, -1 if none: for alternating colours.
    int runNumber(int i) const;

    bool isRejected(int i) const { return m_rejected.contains(i); }
    void setRejected(int i, bool rejected);   // a reject isn't a best shot

    // A best shot of its species: rated 5 stars when organised, so the library
    // takes it as a Best Shot when it's added.
    bool isBest(int i) const { return m_best.contains(i); }
    void setBest(int i, bool best);           // ...and isn't a reject
    QList<int> bestShots() const;             // in order

    // Gives shot `i` a location (Set Location on Map), written straight into
    // its files (geo::GeotagWriter: the JPEG, a RAW's sidecar -- which then
    // moves with it -- and the RAW itself with `intoRaw`), and kept here.
    geo::GeotagWriter::Result setLocation(int i, double latitude, double longitude, bool intoRaw);

    // Shot `i`'s files changed (a rating written): its size and time read
    // again, so its progress still recognises it.
    void refreshFileStats(int i);

    // The names given so far, each once, most recent first.
    QStringList recentNames() const;

    OrganisePlan plan() const;

    // Names, rejects and naming ends, kept in kProgressFile in the folder so
    // a session can be picked up again; a shot is recognised by its name,
    // size and modification time. Shots organised out of the folder keep
    // their entries, so an undone organise brings their state back too.
    bool saveProgress() const;
    void loadProgress();
    void clearProgress();

    // The folder name for a given name: sanitised for the file system.
    static QString folderNameFor(const QString &name);
    // Rates the shot 5 stars, as a best shot: in its JPEG's metadata, and in
    // an XMP sidecar for a RAW (the RAW itself is never written to; a new
    // sidecar's path is put in `createdSidecar`). Files keep their dates.
    static bool rateFiveStars(const Item &item, QString *createdSidecar = nullptr, QString *error = nullptr);
    // A photo's new base name when organising renames them:
    // "Scarlet Robin - Anakie, Victoria, 2025-10-18", or without the place
    // when there isn't one. Safe for the file system.
    static QString renamedBase(const QString &folderName, const QString &place, const QDateTime &taken);

private:
    QJsonObject applyProgress(int from);   // to the photos from `from` on; returns the file's root

    QString m_folder;
    QList<Item> m_items;
    QList<Item> m_pending;
    QHash<int, QString> m_names;
    QSet<int> m_rejected;
    QSet<int> m_best;
    QSet<int> m_namingEnds;
    QStringList m_nameOrder;   // names in the order given (for recentNames())
};

} // namespace pl::organise
