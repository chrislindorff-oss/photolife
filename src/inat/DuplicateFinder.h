#pragma once

#include "inat/ObservationGrouper.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

namespace pl::taxonomy {
struct Observation;
}

namespace pl::inat {

// An observation of the user's that's already on iNaturalist, to compare
// with one about to be uploaded.
struct ExistingObservation
{
    qint64 id = 0;
    qint64 taxonId = 0;                // 0: not identified
    QList<qint64> taxonAncestorIds;
    QString taxonName;                 // as shown
    QDate date;
    QDateTime time;                    // clock time where it was seen; invalid when not known
    std::optional<double> latitude;
    std::optional<double> longitude;
    QString placeGuess;
    QStringList photoUrls;             // small previews, to compare by eye
    // From the catalogue: library photos already on it (no network needed).
    bool fromLibrary = false;
    QString libraryPhoto;              // one of them, by file name
    QString libraryPath;               // ...its preview file
    QString libraryHash;               // ...and thumbnail key

    static ExistingObservation fromInat(const taxonomy::Observation &observation);
};

// One that looks like the same sighting, and why.
struct DuplicateHit
{
    ExistingObservation existing;
    QString reasons;          // "same species, 25 min apart, 140 m away"
    double km = -1;           // how far apart; -1 when either has no location
    qint64 secondsApart = -1; // -1 when either has no time
};

// Whether a proposed observation is already on iNaturalist, by the
// sighting rather than the photo: the same or a related taxon (a genus or
// subspecies observation counts, as does an unidentified one), the same
// day (or within 12 hours when both have a time), and within 1 km (or
// either has no location). Pure: it compares what it's given.
namespace DuplicateFinder {

constexpr double kRadiusKm = 1.0;
constexpr qint64 kTimeWindowSeconds = 12 * 3600;

// Best first: closest in time, then in distance. The observation the
// photos are already part of isn't a hit.
QList<DuplicateHit> find(const ProposedObservation &proposed, const QList<ExistingObservation> &existing);

// The days to search, a day either side of each observation's, with
// overlapping ranges joined.
QList<QPair<QDate, QDate>> dateRanges(const QList<ProposedObservation> &proposed);

} // namespace DuplicateFinder

} // namespace pl::inat
