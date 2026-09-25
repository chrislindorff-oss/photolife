#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>

#include <optional>

namespace pl::net {
class INatClient;
}

namespace pl::inat {

// One of the user's iNaturalist observations, paired with whether it looks
// like something already in the local library. Never hidden, only flagged --
// see InatObservationFetcher's class comment for why.
struct Candidate
{
    taxonomy::Observation observation;
    bool likelyDuplicate = false;

    // Photo ids (observation.photos[].id) among this observation's photos
    // that exactly match a capture.inat_photo_id already in the library --
    // i.e. this specific photo was downloaded before, not just "looks like"
    // one that was. Per-photo rather than a single bool: a user can select
    // individual photos from an observation, so some of its photos may be
    // downloaded already while others aren't.
    QSet<qint64> alreadyDownloadedPhotoIds;

    // Filled by LibraryPresence::annotate() (see LibraryPresence.h).
    bool presenceChecked = false;
    bool inAnyTree = false;
    bool inLibrary = false;
};

// Searches one iNaturalist user's observations for every taxon in a
// reference tree project -- and, when the tree has a locality, confined to
// that place too, so a species that also occurs elsewhere doesn't pull in
// observations outside the tree's actual scope -- comparing each against
// locally-catalogued captures
// to flag (never silently exclude) likely duplicates: photos catalogued
// before this feature existed have no stored link to an iNat observation, so
// "already have this" can only ever be a same-taxon-and-close-date(-and-GPS)
// heuristic here -- the user makes the final call in the review grid.
//
// One HTTP page per batch of taxa (batches of 30, the same conservative size
// ReferencePhotoFetcher already uses for a taxon-id list, until a higher safe
// limit for this endpoint is confirmed against the live API), paced by the
// shared, rate-limited HttpClient underneath INatClient.
//
// Drive it from the thread that owns the INatClient / store connection.
class InatObservationFetcher : public QObject
{
    Q_OBJECT

public:
    InatObservationFetcher(net::INatClient &inat, QString connectionName,
                           QObject *parent = nullptr);

    // `placeId` restricts the search to that iNaturalist place (0 = worldwide)
    // -- normally the active reference tree's own locality, so results match
    // both its taxa and its geographic scope, not just the species list.
    void start(const QString &userLogin, const QList<qint64> &taxonIds, qint64 placeId = 0);

    // Every one of the user's observations with photos (optionally within
    // `placeId`), not limited to any reference tree's taxa. progress() then
    // reports observations fetched so far against the API's total.
    void startAll(const QString &userLogin, qint64 placeId = 0);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    // `done`/`total` count taxon batches; `found` is the running number of
    // observations seen so far. Emitted once per HTTP page, not just once per
    // completed batch -- a single batch can span many pages (a prolific
    // observer, or a broad taxon), and `found` is what keeps the UI visibly
    // alive while `done` sits unchanged through all of them.
    void progress(int done, int total, int found);
    void finished(bool ok, const QString &error, QList<Candidate> candidates);

private:
    struct LocalRecord
    {
        qint64 taxonInatId = 0;
        QDate date;
        std::optional<double> latitude;
        std::optional<double> longitude;
    };

    void loadLocalRecords();
    void fetchNextBatch();
    void fetchPage();
    void fetchAllPage();
    void begin(const QString &userLogin, qint64 placeId);
    bool checkCancelled();
    void fail(const QString &error);
    void succeed();
    bool looksLikeDuplicate(const taxonomy::Observation &obs) const;
    QSet<qint64> alreadyDownloadedPhotoIds(const taxonomy::Observation &obs) const;

    net::INatClient &m_inat;
    QString m_connectionName;

    bool m_running = false;
    bool m_cancelled = false;
    QString m_userLogin;
    qint64 m_placeId = 0;
    QList<QList<qint64>> m_batches;
    int m_batchIndex = 0;
    int m_page = 1;
    int m_batchFetchedCount = 0;   // results seen so far for the current batch, across pages
    qint64 m_idAbove = 0;          // startAll(): keyset cursor
    int m_allTotal = 0;            // startAll(): the API's total, from the first page

    QList<LocalRecord> m_localRecords;
    QSet<qint64> m_downloadedPhotoIds;   // capture.inat_photo_id already in the library
    QList<Candidate> m_candidates;

    static constexpr int kBatchSize = 30;
    static constexpr double kDuplicateRadiusKm = 1.0;
};

} // namespace pl::inat

Q_DECLARE_METATYPE(pl::inat::Candidate)
