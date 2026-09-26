#pragma once

#include "checklist/ChecklistParser.h"

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
}

namespace pl::checklist {

// Imports a parsed checklist into an existing project: resolves each row against
// the cached taxonomy (or iNaturalist once), records the conservation status in
// taxon_status, links the taxon into the project as from_checklist, and keeps
// the checklist's spelling as a synonym so the matcher recognises it. Fills any
// missing ancestor taxa at the end so the tree stays connected.
//
// A status (or origin: native/introduced) belongs to the taxon, not to one
// tree, so a row skipped for being outside the project's genera still has it
// recorded when its taxon
// is already in the local cache (e.g. from another tree) -- the stats of every
// tree holding that taxon then include it. Such rows cost no network call and
// are not added to the project.
//
// Drive it from the thread that owns the INatClient / store connection.
class ChecklistImporter : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        int projectId = -1;
        QString source;                    // taxon_status.source, e.g. "VBA 2021"
        std::optional<qint64> placeInatId;
        QList<ChecklistEntry> entries;
        bool onlyKnownGenera = true;        // skip rows whose genus isn't in the project
    };

    // Rows of the last run skipped as outside the project's genera whose
    // status and/or origin was still recorded (see the class comment).
    int statusesOutsideProject() const { return m_statusesOutsideProject; }

    ChecklistImporter(pl::net::INatClient &inat, pl::taxonomy::TaxonomyStore &store,
                      QObject *parent = nullptr);

    void start(const Request &request);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, int imported, int skipped, int unresolved);

private:
    void processNext();
    void fillAncestors();
    void fillNextBatch();
    void fail(const QString &error);
    void succeed();
    bool stop();
    // The row's conservation status and origin, whichever it has.
    void recordFacts(qint64 taxonInatId, const ChecklistEntry &entry);

    pl::net::INatClient &m_inat;
    pl::taxonomy::TaxonomyStore &m_store;

    Request m_request;
    bool m_running = false;
    bool m_cancelled = false;

    QSet<QString> m_projectGenera;
    int m_index = 0;
    int m_imported = 0;
    int m_skipped = 0;
    int m_statusesOutsideProject = 0;
    int m_unresolved = 0;

    QSet<qint64> m_known;               // taxa already in the store/project this run
    QSet<qint64> m_missingAncestors;
    QList<QList<qint64>> m_batches;
    int m_batchIndex = 0;
};

} // namespace pl::checklist
