#pragma once

#include <QDate>
#include <QList>
#include <QString>

#include <optional>

namespace pl::coverage {

// A reference tree's photographic life list: its taxa in the order they were
// first photographed. A species counts once, dated by the earliest photo of
// it or of any subspecies, variety or form below it; those (and hybrids) can
// be listed as entries of their own too.

struct LifeListNode
{
    qint64 inatId = 0;
    qint64 parentInatId = 0;   // 0: none in the tree
    QString rank;
    QString name;
    QString commonName;
};

// One matched photo of a tree taxon.
struct LifeListSighting
{
    qint64 taxonInatId = 0;
    int captureId = 0;
    QString capturedOn;   // ISO date, maybe with a time; empty when unknown
};

struct LifeListEntry
{
    int number = 0;   // 1, 2, 3...; 0 when it has no usable date (listed last)
    qint64 inatId = 0;
    QString name;
    QString commonName;
    QString rank;
    bool belowSpecies = false;   // a subspecies, variety, form or hybrid
    QString firstOn;             // the first photo's captured_on; empty when undated
    int captureId = 0;           // the first photo (when undated, its lowest-numbered one)
    int photoCount = 0;          // photos counted toward it
    // Filled by loadLifeList():
    QString previewPath;
    QString previewHash;
    QString locality;            // "Town, State, Country", or the place typed in the file name
    // The first photo's camera, from its EXIF (capture_camera): read on
    // demand, so cameraChecked is false until it has been.
    QString camera;
    std::optional<double> focalLength;   // millimetres
    bool cameraChecked = false;
};

// True for a captured_on to date by: known, and not after `today` (a file
// name read as 2912 isn't one).
bool isUsableLifeListDate(const QString &capturedOn, const QDate &today);

// Every entry, species and below, in order: dated ones by first photo (the
// time breaks a same-day tie, then the capture), then the undated by name.
// Not numbered: numberedLifeList() does that for what's shown.
QList<LifeListEntry> buildLifeList(const QList<LifeListNode> &nodes, const QList<LifeListSighting> &sightings,
                                   const QDate &today);

// The entries shown -- species only unless `includeBelow` -- numbered 1, 2, 3...
// (the undated keep 0).
QList<LifeListEntry> numberedLifeList(const QList<LifeListEntry> &all, bool includeBelow);

// buildLifeList() over a project's tree and its auto/confirmed matches, with
// each entry's first photo's preview and place filled in. Read-only.
QList<LifeListEntry> loadLifeList(const QString &connectionName, int projectId,
                                  const QDate &today = QDate::currentDate());

} // namespace pl::coverage
