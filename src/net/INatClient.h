#pragma once

#include "net/HttpClient.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QList>
#include <QTime>
#include <QObject>
#include <QPointF>
#include <QString>

#include <functional>
#include <optional>

class QUrl;

namespace pl::net {

// The result of one API call: either a value or an error string.
template <typename T>
struct Outcome
{
    T value{};
    QString error;
    int status = 0;   // the HTTP status, where a call needs to tell 401 from 403
    bool ok() const { return error.isEmpty(); }
};

// One entry from observations/species_counts: a taxon recorded in the region,
// with its observation count and the chain of ancestor iNat ids from its
// `ancestry` string (root first).
struct SpeciesCount
{
    int count = 0;
    pl::taxonomy::Taxon taxon;
    QList<qint64> ancestorIds;
};

struct SpeciesCountsPage
{
    int totalResults = 0;
    int page = 0;
    int perPage = 0;
    QList<SpeciesCount> results;
};

// A taxon plus the immediate context the API returns alongside it.
struct TaxonDetail
{
    pl::taxonomy::Taxon taxon;
    QList<pl::taxonomy::Taxon> ancestors;   // root first
    QList<pl::taxonomy::Taxon> children;
};

// Which of an observer's observations userSpeciesCounts() counts.
enum class ObservationQuality
{
    Any,          // every observation, casual included
    Verifiable,   // research grade + needs ID
    Research,     // research grade only
};

struct ObservationPage
{
    int totalResults = 0;
    QList<pl::taxonomy::Observation> results;
};

// One taxon iNaturalist's computer vision suggests for a photo.
struct VisionSuggestion
{
    pl::taxonomy::Taxon taxon;
    double score = 0;         // combined_score, 0-100: the vision, weighted by what's seen nearby
    double visionScore = 0;   // vision_score, 0-100: the photo alone
    bool seenNearby = false;  // frequency_score > 0: recorded near the photo's location
};

struct VisionResult
{
    QList<VisionSuggestion> suggestions;     // best first
    std::optional<VisionSuggestion> commonAncestor;   // a coarser taxon they share, if any
};

// A new observation of the user's own, for createObservation().
struct NewObservation
{
    QString uuid;               // made here, so after a failure findObservationByUuid() can tell
                                // whether it was made after all
    qint64 taxonId = 0;
    QString speciesGuess;       // the name the user knows it by
    QDate date;
    std::optional<QTime> time;  // local time where it was taken; iNaturalist takes the zone from the place
    double latitude = 0;
    double longitude = 0;
    QString geoprivacy;         // "open" | "obscured" | "private"
    QString description;
    bool fromVision = false;    // the species was picked from computer vision's suggestions
};

// Typed wrapper over the anonymous iNaturalist v1 API. All calls go
// through the shared HttpClient, so they inherit its rate limiting, retries and
// ETag caching. Results come back on the HttpClient's thread.
//
// Almost every call only reads. The exceptions -- createObservation() and
// addObservationPhoto() -- post to the user's own iNaturalist account, need
// the access token, and are never sent twice after a failure the server may
// have acted on.
class INatClient : public QObject
{
    Q_OBJECT

public:
    explicit INatClient(HttpClient &http, QObject *parent = nullptr);

    void setBaseUrl(const QString &baseUrl) { m_baseUrl = baseUrl; }
    QString baseUrl() const { return m_baseUrl; }

    // A personal API token (from inaturalist.org/users/api_token), sent as a
    // Bearer token on every call this client makes from here on. Empty (the
    // default) means anonymous, public-API-only requests. See HttpClient::get()
    // for why an authenticated call never touches the conditional-GET cache.
    void setAccessToken(const QString &token) { m_accessToken = token; }
    QString accessToken() const { return m_accessToken; }

    // The HttpClient priority of every call this client makes: Background for
    // a second client over the same HttpClient that quiet housekeeping uses,
    // so it never holds up what the user is waiting for.
    void setPriority(HttpClient::Priority priority) { m_priority = priority; }

    // POST /v1/computervision/score_image: what iNaturalist's computer vision
    // takes `jpeg` to be. A location (x = longitude, y = latitude) and date
    // weight it towards what's recorded there and then; they go as form
    // fields -- iNaturalist ignores them in the query. Needs the access token
    // (a 401 means it has expired); a 403 is iNaturalist refusing. The photo
    // is only scored, never posted.
    void scoreImage(const QByteArray &jpeg, std::optional<QPointF> lonLat, std::optional<QDate> observedOn,
                    std::function<void(Outcome<VisionResult>)> done);

    // GET /v1/users/me: the login the access token belongs to.
    void fetchMe(std::function<void(Outcome<QString>)> done);

    // POST /v1/observations: makes the observation and answers with its id.
    // Not retried after a network error or 5xx -- see findObservationByUuid().
    void createObservation(const NewObservation &observation, std::function<void(Outcome<qint64>)> done);

    // GET /v1/observations?uuid=: the id of the observation with this uuid,
    // or nullopt when there's none -- whether a create that failed was made.
    void findObservationByUuid(const QString &uuid, std::function<void(Outcome<std::optional<qint64>>)> done);

    // POST /v1/observation_photos: adds `jpeg` to the observation and
    // answers with the new photo's id (the id a downloaded photo is known by).
    // Not retried after a network error or 5xx.
    void addObservationPhoto(qint64 observationId, const QByteArray &jpeg, const QString &fileName,
                             std::function<void(Outcome<qint64>)> done);

    // GET /v1/places/autocomplete?q=
    void resolvePlaces(const QString &query,
                       std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done);

    // GET /v1/places/{id1,id2,...}  (up to 30 ids; caller batches)
    void fetchPlaces(const QList<qint64> &inatIds,
                     std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done);

    // GET /v1/taxa?q=&rank=
    void searchTaxa(const QString &query, const QString &rank,
                    std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done);

    // GET /v1/taxa/{id}?all_names=true  (taxon + ancestors + children)
    void fetchTaxon(qint64 inatId, std::function<void(Outcome<TaxonDetail>)> done);

    // GET /v1/taxa/{id1,id2,...}  (up to 30 ids; caller batches). With
    // `allNames`, also every name each taxon has (all_names=true).
    void fetchTaxa(const QList<qint64> &inatIds,
                   std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done,
                   bool allNames = false);

    // GET /v1/observations/species_counts?taxon_id=&place_id=&verifiable=true&page=
    void speciesCounts(qint64 taxonId, qint64 placeId, int page, int perPage,
                       std::function<void(Outcome<SpeciesCountsPage>)> done);

    // GET /v1/observations/species_counts?user_login=&taxon_id=&place_id=&page=
    // One observer's "leaf" taxa -- the most specific taxa they've recorded,
    // each with its observation count and ancestry. `taxonId`/`placeId` of 0
    // omit that filter; `quality` picks which observations count.
    void userSpeciesCounts(const QString &userLogin, qint64 taxonId, qint64 placeId,
                           ObservationQuality quality, int page, int perPage,
                           std::function<void(Outcome<SpeciesCountsPage>)> done);

    // GET /v1/observations/species_counts?taxon_id=&place_id=&introduced=true&page=
    // The taxa at or below `taxonId` that iNaturalist lists as introduced in
    // `placeId` (or a place containing it) and that have observations there.
    void introducedSpeciesCounts(qint64 taxonId, qint64 placeId, int page, int perPage,
                                 std::function<void(Outcome<SpeciesCountsPage>)> done);

    // GET /v1/observations?taxon_id=&place_id=&verifiable=true&per_page=0
    // Just the verifiable-observation count for a taxon in a place (0 = no place
    // filter) — enough to tell whether a taxon actually occurs there.
    void observationCount(qint64 taxonId, qint64 placeId,
                          std::function<void(Outcome<int>)> done);

    // GET /v1/observations?user_login=&taxon_id=<comma list>&place_id=&d1=&d2=&photos=true&page=
    // `taxonIds` is one page's worth of ids -- callers batch large taxon lists
    // themselves (see ReferencePhotoFetcher's 30-at-a-time precedent). `placeId`
    // restricts results to that place (0 = no place filter), the same
    // convention as speciesCounts()/observationCount() above. `dateFrom`/`dateTo`
    // restrict to observations made in that (inclusive) date range; an invalid
    // QDate on either end omits that bound (both invalid = no date filter at
    // all, the only behaviour before this parameter existed). Sends the
    // configured access token, if any, so the results reflect what that user
    // can actually see (true coordinates on their own geoprivacy-obscured
    // observations, otherwise the public, possibly-obscured view).
    // `photosOnly` false includes observations without photos.
    void fetchObservations(const QString &userLogin, const QList<qint64> &taxonIds,
                           qint64 placeId, const QDate &dateFrom, const QDate &dateTo, int page,
                           std::function<void(Outcome<ObservationPage>)> done, bool photosOnly = true);

    // Every one of the user's observations with photos, in ascending id
    // order: pass 0 for the first page, then the last result's id to
    // continue. An empty page means there are no more. `taxonId` > 0 limits
    // it to that taxon and everything beneath it (iNat's taxon_id matches
    // descendants); 0 = every taxon.
    void fetchAllObservations(const QString &userLogin, qint64 placeId, qint64 taxonId,
                              qint64 idAbove,
                              std::function<void(Outcome<ObservationPage>)> done);

private:
    void getSpeciesCountsPage(const QUrl &url,
                              std::function<void(Outcome<SpeciesCountsPage>)> done);
    void getObservationPage(const QUrl &url, std::function<void(Outcome<ObservationPage>)> done);
    void request(const QUrl &url, HttpClient::Handler handler, const QString &bearerToken = {});

    HttpClient &m_http;
    HttpClient::Priority m_priority = HttpClient::Priority::Normal;
    QString m_baseUrl = QStringLiteral("https://api.inaturalist.org/v1");
    QString m_accessToken;
};

} // namespace pl::net
