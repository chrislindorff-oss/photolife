#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QDate>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace pl::net {

class HttpClient;

// The result of one API call: either a value or an error string.
template <typename T>
struct Outcome
{
    T value{};
    QString error;
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

struct ObservationPage
{
    int totalResults = 0;
    QList<pl::taxonomy::Observation> results;
};

// Typed, read-only wrapper over the anonymous iNaturalist v1 API. All calls go
// through the shared HttpClient, so they inherit its rate limiting, retries and
// ETag caching. Results come back on the HttpClient's thread.
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

    // GET /v1/places/autocomplete?q=
    void resolvePlaces(const QString &query,
                       std::function<void(Outcome<QList<pl::taxonomy::Place>>)> done);

    // GET /v1/taxa?q=&rank=
    void searchTaxa(const QString &query, const QString &rank,
                    std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done);

    // GET /v1/taxa/{id}?all_names=true  (taxon + ancestors + children)
    void fetchTaxon(qint64 inatId, std::function<void(Outcome<TaxonDetail>)> done);

    // GET /v1/taxa/{id1,id2,...}  (up to 30 ids; caller batches)
    void fetchTaxa(const QList<qint64> &inatIds,
                   std::function<void(Outcome<QList<pl::taxonomy::Taxon>>)> done);

    // GET /v1/observations/species_counts?taxon_id=&place_id=&verifiable=true&page=
    void speciesCounts(qint64 taxonId, qint64 placeId, int page, int perPage,
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
    void fetchObservations(const QString &userLogin, const QList<qint64> &taxonIds,
                           qint64 placeId, const QDate &dateFrom, const QDate &dateTo, int page,
                           std::function<void(Outcome<ObservationPage>)> done);

private:
    HttpClient &m_http;
    QString m_baseUrl = QStringLiteral("https://api.inaturalist.org/v1");
    QString m_accessToken;
};

} // namespace pl::net
