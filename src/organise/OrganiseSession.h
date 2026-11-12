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

// What was noted about a run's subject (Notes for This Run…), in
// iNaturalist's own annotation terms so an upload can carry them across;
// empty fields mean "not noted".
struct RunNotes
{
    QString sex;          // "Female", "Male", "Cannot Be Determined"
    QString lifeStage;    // "Adult", "Juvenile", "Egg", "Larva", "Pupa", "Nymph", "Teneral", "Subimago"
    QString alive;        // "Alive", "Dead", "Cannot Be Determined"
    QString flowers;      // "Flowers", "Fruits or Seeds", "Flower Buds", "No Flowers or Fruits"
    QString text;         // anything else
    bool isEmpty() const
    {
        return sex.isEmpty() && lifeStage.isEmpty() && alive.isEmpty() && flowers.isEmpty() && text.trimmed().isEmpty();
    }
    // "Female · Adult · Alive · on a fence post": for captions and descriptions.
    QString summary() const;
    bool operator==(const RunNotes &o) const = default;
};

// A run of photos going into one folder.
struct FolderMove
{
    QString folderName;      // as it will be created (sanitised)
    QList<int> items;        // indexes into OrganiseSession::items()
};

// A burst: photos taken in quick succession, first and last index.
struct Burst
{
    int first = 0;
    int last = 0;
    int size() const { return last - first + 1; }
    bool contains(int i) const { return i >= first && i <= last; }
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
    // Identify Runs: where new runs probably start among the photos no name
    // covers yet -- the first of each such stretch, and wherever more than
    // `gapSeconds` passed since the photo before (rejects aside). Named runs
    // are never split.
    QList<int> proposedRunStarts(int gapSeconds) const;

    // "Not sure" of the name: kept on the run's named photo. Organising can
    // mark such folders "cf." (see uncertainName()).
    bool isUnsure(int i) const { return m_unsure.contains(runStart(i)); }
    void setUnsure(int i, bool unsure);   // the run photo `i` is in; nothing before the first name
    int unsureCount() const { return int(m_unsure.size()); }
    // The next named photo marked not sure after `i` (wrapping), -1 if none.
    int nextUnsure(int i) const;

    // Notes on a run's subject, kept on its named photo.
    RunNotes notesAt(int i) const { return m_notes.value(runStart(i)); }
    void setNotes(int i, const RunNotes &notes);   // the run photo `i` is in

    // Bursts: runs of photos each taken within `gapSeconds` of the one
    // before, two or more long, in order.
    QList<Burst> bursts(int gapSeconds = kBurstGapSeconds) const;
    static constexpr int kBurstGapSeconds = 2;

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

    // Takes shot `i`'s location out of its files again (Remove Location):
    // geo::GeotagWriter::clearShot(). What's still there afterwards (inside a
    // RAW that wasn't to be changed) is kept here, and reported.
    geo::GeotagWriter::Result clearLocation(int i, bool intoRaw);

    // Shot `i`'s files changed (a rating written): its size and time read
    // again, so its progress still recognises it.
    void refreshFileStats(int i);

    // The names given so far, each once, most recent first.
    QStringList recentNames() const;

    // `markUnsure`: runs marked not sure go into "cf." folders (uncertainName()).
    OrganisePlan plan(bool markUnsure = false) const;

    // Names, rejects and naming ends, kept in kProgressFile in the folder so
    // a session can be picked up again; a shot is recognised by its name,
    // size and modification time. Shots organised out of the folder keep
    // their entries, so an undone organise brings their state back too.
    bool saveProgress() const;
    void loadProgress();
    void clearProgress();

    // The folder's GPS track (GPX Track…): the .gpx file photos are geotagged
    // from, and the correction added to the photos' times before they're
    // matched to it (a camera clock that was off, or set to another time
    // zone). Kept with the progress; the file itself is only ever read.
    QString gpxPath() const { return m_gpxPath; }   // absolute; empty when none
    int gpxOffsetSeconds() const { return m_gpxOffsetSeconds; }
    void setGpx(const QString &path, int offsetSeconds);   // an empty path removes it
    // Whether the track is the user's choice (GPX Track…), not one found.
    bool gpxChosen() const { return m_gpxChosen; }
    // A track found for the folder elsewhere (the tracks folder): used, not
    // kept -- it's looked for again next time. Ignored once one is chosen.
    void useFoundGpx(const QString &path);
    // The .gpx files directly in `folder`, by name.
    static QStringList gpxFilesIn(const QString &folder);

    // A name marked uncertain: "Pterostylis cf. nana" for a binomial,
    // "cf. Scarlet Robin" otherwise.
    static QString uncertainName(const QString &name);

    // The folder name for a given name: sanitised for the file system.
    static QString folderNameFor(const QString &name);
    // Rates the shot 5 stars, as a best shot: in its JPEG's metadata, and in
    // an XMP sidecar for a RAW (the RAW itself is never written to; a new
    // sidecar's path is put in `createdSidecar`). Files keep their dates.
    static bool rateFiveStars(const Item &item, QString *createdSidecar = nullptr, QString *error = nullptr);
    // Writes a run's notes into a shot's files as rateFiveStars() writes the
    // rating: in a JPEG's XMP, and an XMP sidecar for a RAW (never the RAW).
    static bool writeNotes(const Item &item, const RunNotes &notes, QString *createdSidecar = nullptr,
                           QString *error = nullptr);
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
    QSet<int> m_unsure;              // named photos whose name is uncertain
    QHash<int, RunNotes> m_notes;    // by named photo
    QStringList m_nameOrder;   // names in the order given (for recentNames())
    QString m_gpxPath;
    int m_gpxOffsetSeconds = 0;
    bool m_gpxChosen = false;  // the user's choice (even "none"), not one just found in the folder
};

} // namespace pl::organise
