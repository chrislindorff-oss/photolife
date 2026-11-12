#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDateTime>
#include <QHash>
#include <QPair>
#include <QList>
#include <QPointF>
#include <QString>

#include <optional>

namespace pl::inat {

// One library photo that may go to iNaturalist.
struct UploadPhoto
{
    int captureId = 0;
    QString path;          // its preview file: a JPEG, or a RAW (its embedded preview is sent)
    QString contentHash;   // its thumbnail's key
    QString label;         // its file name
    QDateTime taken;       // invalid when it has no date
    bool hasTime = false;  // `taken` has a time of day, not just a date
    std::optional<double> latitude;
    std::optional<double> longitude;
    taxonomy::Taxon taxon;      // inatId 0: no species yet
    qint64 observationId = 0;   // already part of this observation on iNaturalist
    qint64 photoId = 0;         // ...as this photo
    // Its sighting is on iNaturalist, as `observationId`, but this photo
    // isn't and won't be sent: the user said it's the same as one there
    // (stored as a negative inat_photo_id), or it's a copy of a photo that is
    // (`copyOf` names that one; nothing is stored).
    bool linked = false;
    QString copyOf;
    // Notes on the subject written into the photo when it was organised
    // (scan::readXmpNotes): "sex", "lifeStage", "alive", "flowers", "notes".
    QHash<QString, QString> notes;

    bool done() const { return photoId > 0 || linked; }   // nothing to send
};

// The photos that go up as one observation, and what's sent with them.
struct ProposedObservation
{
    enum Problem { NoSpecies = 1, NoLocation = 2, NoDate = 4 };

    // In the order they're sent: the first is the one iNaturalist shows.
    // Those already sent come first.
    QList<UploadPhoto> photos;
    taxonomy::Taxon taxon;
    bool taxonFromVision = false;   // picked from iNaturalist's suggestions
    std::optional<QDateTime> when;  // set by hand, when no photo has a date
    QString note;                   // sent as its description
    // Annotations, by name -- "sex", "lifeStage", "alive", "flowers" -- in
    // iNaturalist's terms ("Male", "Adult"...); sent once it's made.
    QHash<QString, QString> annotations;
    qint64 observationId = 0;       // made on iNaturalist already (some or all photos sent)
    QString uuid;                   // the one it's made with, kept so a retry can look it up
    bool uuidSent = false;          // a create was tried with `uuid`, so look before making another
    // Looked for among the user's iNaturalist observations since its photos
    // or species last changed.
    bool checked = false;

    // Where and when: the first photo with a location, the earliest date (or `when`).
    std::optional<QPointF> where() const;   // x = longitude, y = latitude
    QDateTime observedAt() const;
    bool hasTime() const;
    int problems() const;   // Problem flags
    int remaining() const;  // photos still to send
    bool uploaded() const { return observationId > 0 && remaining() == 0; }
};

// iNaturalist's ids for annotations given by name (see
// ProposedObservation::annotations): (controlled attribute, value) pairs,
// those it doesn't know left out.
QList<QPair<int, int>> annotationIds(const QHash<QString, QString> &annotations);
// "Male · Adult · Alive": for showing.
QString annotationsText(const QHash<QString, QString> &annotations);

// Proposes observations from photos, and lets the user change them. No
// network or catalogue: everything it needs is in the photos.
namespace ObservationGrouper {

// Photos without a species group only this close together (a burst):
// unsorted photos taken half an hour apart are often of different things.
constexpr int kNoSpeciesGapSeconds = 120;

// Photos already part of an observation stay with it. The rest are sorted
// by time, and each species' photos are split where more than `gapSeconds`
// (kNoSpeciesGapSeconds for photos without one) pass between one and the
// next -- and, with `maxKm` above 0, where one is further than that from
// the last of its group with a location (one without a location goes by
// time alone). Undated photos each start their own.
QList<ProposedObservation> group(QList<UploadPhoto> photos, int gapSeconds, double maxKm = 0);

// The species the photos of `groups` are of, without repeats (a "no
// species" one too, when there is one) -- more than one means a merge must
// ask which to use.
QList<taxonomy::Taxon> speciesOf(const QList<ProposedObservation> &groups);

// Whether `groups` can become one: not when two of them are already
// separate observations on iNaturalist.
bool canMerge(const QList<ProposedObservation> &groups);

// `groups` as one observation of `taxon`, keeping the one that's already
// on iNaturalist, if any, and the notes.
ProposedObservation merge(const QList<ProposedObservation> &groups, const taxonomy::Taxon &taxon);

// `group` as two, the second starting at photo `at`. Only for one that
// isn't on iNaturalist yet, with `at` inside it (0 < at < size).
bool canSplit(const ProposedObservation &group, int at);
QList<ProposedObservation> splitAt(const ProposedObservation &group, int at);

// Moves photo `from` of `groups[fromGroup]` to position `to` of
// `groups[toGroup]` (the same group to reorder it). A photo that's already
// part of an observation on iNaturalist can't move, and nothing goes ahead
// of the photos already sent. A photo moved to another observation takes
// its species (one moved to an observation that's on iNaturalist is added
// to it); a group left empty is removed. False when nothing moved.
bool canMove(const QList<ProposedObservation> &groups, int fromGroup, int from, int toGroup);
bool movePhoto(QList<ProposedObservation> &groups, int fromGroup, int from, int toGroup, int to);

} // namespace ObservationGrouper

} // namespace pl::inat
