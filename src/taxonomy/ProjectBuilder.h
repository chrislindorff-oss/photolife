#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

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
    };

    ProjectBuilder(pl::net::INatClient &inat, TaxonomyStore &store, QObject *parent = nullptr);

    void start(const Request &request);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(const QString &phase, int done, int total);
    void finished(bool ok, const QString &error, int projectId);

private:
    void resolvePlace();
    void resolveRootTaxon();
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

    QSet<qint64> m_stored;                 // taxa written this run (inat ids)
    QSet<qint64> m_neededAncestors;
    QList<QList<qint64>> m_ancestorBatches;
    int m_ancestorBatchIndex = 0;
    int m_ancestorFilled = 0;
    int m_ancestorTotal = 0;
};

} // namespace pl::taxonomy
