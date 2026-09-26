#pragma once

#include "net/INatClient.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace pl::taxonomy {
class TaxonomyStore;
}

namespace pl::inat {

// Gathers everything SpeciesComparison::compare() needs: one observer's
// leaf taxa from observations/species_counts (all pages), plus the rank of
// every ancestor of those taxa and of the given tree leaves. Ranks come from
// the local taxon cache first; ancestors that aren't cached are fetched from
// iNaturalist 30 at a time and written to the cache, so a repeat comparison
// needs only the species_counts pages.
//
// Drive it from the thread that owns the INatClient / store connection.
class InatSpeciesListFetcher : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString userLogin;
        qint64 taxonId = 0;    // limit to this group (0 = every taxon)
        qint64 placeId = 0;    // limit to this place (0 = worldwide)
        net::ObservationQuality quality = net::ObservationQuality::Any;
        QList<qint64> treeLeafIds;
    };

    InatSpeciesListFetcher(net::INatClient &inat, taxonomy::TaxonomyStore &store,
                           QObject *parent = nullptr);

    void start(const Request &request);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    // total <= 0 while the total isn't known yet.
    void progress(const QString &phase, int done, int total);
    void finished(bool ok, const QString &error, QList<pl::net::SpeciesCount> inatLeaves,
                  QHash<qint64, pl::taxonomy::Taxon> known);

private:
    static constexpr int kPerPage = 500;
    static constexpr int kTaxaBatchSize = 30;

    void fetchCountsPage();
    void resolveAncestors();
    void fetchNextTaxaBatch();
    bool checkCancelled();
    void fail(const QString &error);

    net::INatClient &m_inat;
    taxonomy::TaxonomyStore &m_store;
    Request m_request;
    bool m_running = false;
    bool m_cancelled = false;
    int m_page = 1;
    int m_total = 0;
    QList<net::SpeciesCount> m_leaves;
    QHash<qint64, taxonomy::Taxon> m_known;
    QList<qint64> m_missing;
    int m_missingDone = 0;
};

} // namespace pl::inat
