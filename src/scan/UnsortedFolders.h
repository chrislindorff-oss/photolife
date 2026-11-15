#pragma once

#include "scan/ScanTypes.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace pl::scan {

// Unsorted folders: folders (with their subfolders) of unculled runs of
// shots, marked by the user. PhotoLife catalogues one photo per run in them
// -- the run's representative, an ordinary capture that's matched as usual,
// so its species counts -- and only remembers the rest (unsorted_photo),
// with the run they belong to. Rescans skip the remembered files and
// matching handles one entry per run. Unmarking a folder forgets them, and
// the next scan catalogues every photo again. Files are never touched.

// A run: photos of the same subject (`group`: their folder and name) taken
// one after another, at most `gapSeconds` apart; photos with no time go by
// their day (or, with no date at all, are one run per group).
struct RunItem
{
    QString group;
    QDateTime taken;   // valid when the time of day is known
    QDate day;
};
constexpr int kRunGapSeconds = 10;
// Indices into `items`, each run in time order.
QList<QList<int>> groupRuns(const QList<RunItem> &items, int gapSeconds = kRunGapSeconds);

namespace Unsorted {

// The folders marked Unsorted.
QStringList folders(const QString &connectionName);
// Whether `path` (a folder) is one of `folders` or inside one.
bool contains(const QStringList &folders, const QString &path);

// A photo's group for runs: its folder and name -- a camera's file name
// (DSC_0001) counting as no name, so its shots run together.
QString groupKey(const QString &dirPath, const QString &baseName);

// When a file was taken, read cheaply (a RAW's own metadata, a JPEG's EXIF);
// invalid when it doesn't say.
QDateTime fileTime(const DiscoveredFile &file);

struct ThinResult
{
    bool ok = false;
    QString error;
    int photos = 0;    // catalogued photos in the folder before
    int runs = 0;
    int kept = 0;      // still catalogued: one per run, and any you've decided on
    int removed = 0;   // now only remembered
};

// Marks `folder` Unsorted and thins what's catalogued there: one photo per
// run is kept; the others' catalogue entries are removed and remembered.
// A photo you've decided something about -- named or rejected it, a best
// shot, a cover photo, on iNaturalist, marked on or not on iNaturalist --
// always stays catalogued.
ThinResult mark(const QString &connectionName, const QString &folder);
// No longer Unsorted: forgets the remembered photos (the next scan
// catalogues them all again).
bool unmark(const QString &connectionName, const QString &folder);

struct FolderStats
{
    QString path;
    int catalogued = 0;   // representatives (and kept photos)
    int remembered = 0;   // the rest of the runs
};
QList<FolderStats> stats(const QString &connectionName);

// The taxa (iNat ids) whose every named photo is in an Unsorted folder:
// photographed, but no sorted photo of them yet.
QSet<qint64> unsortedOnlyTaxa(const QString &connectionName);

} // namespace Unsorted

} // namespace pl::scan
