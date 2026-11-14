#pragma once

#include "inat/DuplicateFinder.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace pl::taxonomy {
struct Observation;
}

namespace pl::inat {

// "Already on iNaturalist?": whether library photos are already on the
// user's iNaturalist account -- posted from the iNaturalist app or website,
// or before PhotoLife -- judged by species, time and place against the
// details of the user's observations (cached in inat_my_observation), with
// a confidence. High ones are marked on iNaturalist by the check; Medium
// and Low wait for the user. Decisions live in capture_inat_status.

enum class PresenceConfidence { None, Low, Medium, High };

QString confidenceKey(PresenceConfidence confidence);   // "high" | "medium" | "low" | ""
PresenceConfidence confidenceFromKey(const QString &key);

// A library photo to look for.
struct PresencePhoto
{
    int captureId = 0;
    qint64 taxonInatId = 0;
    QList<qint64> taxonAncestorIds;   // root first, without the taxon
    QDateTime taken;                  // valid when the time of day is known
    QDate day;                        // valid when at least the day is known
    std::optional<double> latitude;
    std::optional<double> longitude;
};

// Its best match among the user's observations.
struct PresenceHit
{
    qint64 observationId = 0;
    PresenceConfidence confidence = PresenceConfidence::None;
    QString reasons;          // "same species, 25 min apart, 140 m away"
    double km = -1;           // -1: a location missing
    qint64 secondsApart = -1; // -1: a time missing
    bool found() const { return confidence != PresenceConfidence::None; }
};

// Pure: compares photos with the observations it's given.
//  High:   the same species; both timed and at most an hour apart (or the
//          photo has only a day, and it's the same day); within 200 m.
//  Medium: the same or a related taxon, the same day (within 12 hours when
//          both are timed), within 1 km; or High's time and place with an
//          observation that has no species; or the same species at that
//          time where either has no location.
//  Low:    a photo with no date: the same species within 200 m, any date;
//          or a related or unidentified observation at the time, with a
//          location missing.
class PresenceMatcher
{
public:
    static constexpr double kNearKm = 0.2;
    static constexpr double kRadiusKm = 1.0;
    static constexpr qint64 kCloseSeconds = 3600;
    static constexpr qint64 kWindowSeconds = 12 * 3600;

    explicit PresenceMatcher(QList<ExistingObservation> observations);

    PresenceHit best(const PresencePhoto &photo) const;

private:
    PresenceHit compare(const PresencePhoto &photo, const ExistingObservation &obs) const;

    QList<ExistingObservation> m_observations;
    QHash<QDate, QList<int>> m_byDay;
    QHash<qint64, QList<int>> m_byTaxon;
};

// What the catalogue holds: the cache of observations, the photos to look
// for, and the decisions. All on the caller's connection and thread.
namespace PresenceStore {

// --- the cache of the user's observations ----------------------------------
qint64 newestCachedId(const QString &connectionName, const QString &login);
int cachedCount(const QString &connectionName, const QString &login);
bool cacheObservations(const QString &connectionName, const QString &login,
                       const QList<taxonomy::Observation> &observations);
bool clearCache(const QString &connectionName, const QString &login);
QList<ExistingObservation> cachedObservations(const QString &connectionName, const QString &login);

// --- the photos and decisions ----------------------------------------------
// Named photos not known to be on iNaturalist: no observation linked and no
// on_inat / not_on_inat decision (a "maybe" is looked at again).
QList<PresencePhoto> photosToCheck(const QString &connectionName);

// On iNaturalist: linked to `observationId` when > 0 (as Upload links a
// photo whose sighting is there, so it's never sent), else just marked.
bool markOnInat(const QString &connectionName, const QList<int> &captureIds, qint64 observationId,
                PresenceConfidence confidence, const QString &reasons, const QString &decidedBy,
                const QString &checkRun = {});
bool markNotOnInat(const QString &connectionName, const QList<int> &captureIds);
bool markMaybe(const QString &connectionName, int captureId, const PresenceHit &hit, const QString &checkRun);
// Forgets the decisions (and the links they made -- never a real upload).
bool clearStatus(const QString &connectionName, const QList<int> &captureIds);
// Undoes everything one check run decided.
int undoRun(const QString &connectionName, const QString &checkRun);
QString lastRun(const QString &connectionName);

// A decision, for the review list.
struct Item
{
    int captureId = 0;
    QString status;               // on_inat | not_on_inat | maybe
    PresenceConfidence confidence = PresenceConfidence::None;
    QString decidedBy;            // check | user
    QString reasons;
    qint64 observationId = 0;
    QString photoName;            // the photo's name, as shown
    QString photoPath;            // its preview file
    QString photoHash;
    QString photoTaken;
    QString observationTaxon;     // what it is on iNaturalist
    QString observationOn;
    QString observationPlace;
    QString observationPreviewUrl;
};
// `status` "maybe", or "auto" for on_inat decided by a check.
QList<Item> items(const QString &connectionName, const QString &status);

} // namespace PresenceStore

} // namespace pl::inat
