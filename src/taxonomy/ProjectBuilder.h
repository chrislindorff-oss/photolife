#pragma once

#include "taxonomy/TaxonomyTypes.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <optional>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Builds (or refreshes) a reference-tree project from the iNaturalist API:
// resolve the place, resolve the root taxon, pull the region's species list,
// then fill in the intermediate ranks so the tree is connected root-to-leaf.
// Everything is written to the TaxonomyStore as it arrives; a failed or
// cancelled build leaves a partial project that a later run completes.
//
// Drive it from the thread that owns the INatClient / store connection.
class ProjectBuilder : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString projectName;
        QString taxonQuery;   // e.g. "Orchidaceae"
        QString rank;         // optional, e.g. "family"
        QString placeQuery;   // e.g. "Victoria, Australia"; empty = global
        int perPage = 200;

        // When set, used as-is instead of searching/guessing: the caller has
        // already confirmed this is the right match (e.g. via a picker UI).
        std::optional<Place> confirmedPlace;
        std::optional<Taxon> confirmedTaxon;
    };

    ProjectBuilder(pl::net::INatClient &inat, TaxonomyStore &store, QObject *parent = nullptr);

    void start(const Request &request);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

    // Answer to confirmMoreSpecies(): keep pulling the next 1,000 species, or
    // stop here and finalise the tree with what has been fetched so far. Calling
    // either when the builder is not waiting for an answer is a no-op.
    void continueFetching();
    void stopFetching();

signals:
    void progress(const QString &phase, int done, int total);

    // The species list has passed a checkpoint (10,000, then every 5,000) and
    // more remain. The build is paused until continueFetching() / stopFetching().
    void confirmMoreSpecies(int fetched, int estimatedTotal);

    void finished(bool ok, const QString &error, int projectId);

private:
    void resolvePlace();
    void resolveRootTaxon();
    void useRootTaxon(const Taxon &taxon);
    void fetchRootDetail();
    void fetchSpeciesPage(int page);
    void fillAncestors();
    void fillNextAncestorBatch();
    void fail(const QString &error);
    void succeed();
    bool checkCancelled();

    pl::net::INatClient &m_inat;
    TaxonomyStore &m_store;

    Request m_request;
    bool m_running = false;
    bool m_cancelled = false;

    qint64 m_placeId = 0;
    qint64 m_rootTaxonId = 0;
    int m_projectId = -1;

    int m_speciesTotal = 0;
    int m_speciesSeen = 0;
    int m_nextConfirmAt = 0;     // species count at which to next ask the user
    int m_pendingPage = 0;       // species page to resume on continueFetching()
    bool m_awaitingConfirm = false;

    QSet<qint64> m_stored;                 // taxa written this run (inat ids)
    QSet<qint64> m_neededAncestors;
    QList<QList<qint64>> m_ancestorBatches;
    int m_ancestorBatchIndex = 0;
    int m_ancestorFilled = 0;
    int m_ancestorTotal = 0;
};

} // namespace pl::taxonomy
