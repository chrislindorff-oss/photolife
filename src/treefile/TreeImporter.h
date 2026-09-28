#pragma once

#include "treefile/TreeFile.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

namespace pl::net {
class INatClient;
}
namespace pl::taxonomy {
class TaxonomyStore;
}

namespace pl::treefile {

// Creates a reference tree from a TreeFile (see TreeFile.h).
//
// Rows carrying an iNat id plus a name and rank are written straight to the
// store -- an exported file imports with no network at all. Taxa already in
// the local cache are linked as they are (the cache's own names and synonyms
// win over the file's). Rows with only a name are resolved like a checklist
// row: the cache first, then an iNaturalist search; rows with only an id are
// fetched by id. Ancestors the file doesn't list are fetched and linked so
// the tree stays connected, and a file with no root taxon gets the deepest
// taxon every row shares. A file PhotoLife exported is taken as the exact
// tree: its id rows' ancestors are only filled in for a hand-made file.
//
// Drive it from the thread that owns the INatClient / store connection.
class TreeImporter : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString treeName;         // must not name an existing tree
        TreeFile file;
        QString defaultSource;    // status/origin source when a row gives none
    };

    TreeImporter(pl::net::INatClient &inat, pl::taxonomy::TaxonomyStore &store,
                 QObject *parent = nullptr);

    void start(const Request &request);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total);
    // `imported`: taxa linked from the file's rows; `lookedUp`: of those, how
    // many needed iNaturalist; `unresolved`: rows no taxon was found for.
    void finished(bool ok, const QString &error, int projectId, int imported, int lookedUp,
                  int unresolved);

private:
    void fetchPlace();
    bool writeKnownRows();
    void resolveNext();
    void fetchMissing();
    void fetchNextBatch();
    void finishTree();
    // One file row resolved to a cached taxon, ready to link into the tree.
    // `fillAncestors`: link (fetching if need be) the taxon's ancestors too.
    struct PendingLink
    {
        int row = 0;   // index into the file's rows
        qint64 inatId = 0;
        bool fromChecklist = false;
        bool fillAncestors = false;
    };
    // Links them, with their statuses and origins, in a few batched writes --
    // a round trip per row adds up on a remote (Postgres) catalogue.
    void linkRows(const QList<PendingLink> &links);
    bool stop();
    void fail(const QString &error);

    pl::net::INatClient &m_inat;
    pl::taxonomy::TaxonomyStore &m_store;

    Request m_request;
    bool m_running = false;
    bool m_cancelled = false;
    int m_projectId = -1;

    QList<int> m_nameRows;                 // rows to resolve by name
    QList<int> m_idRows;                   // rows with an id but no name/rank
    int m_nameIndex = 0;
    QSet<qint64> m_linked;                 // taxa linked into the project
    QSet<qint64> m_wanted;                 // ids already asked of iNaturalist
    QHash<qint64, QString> m_ancestryOf;   // linked row taxon -> ancestry, for the root
    QSet<qint64> m_fillFrom;               // linked taxa whose ancestors to fill in
    QList<QList<qint64>> m_batches;
    int m_batchIndex = 0;

    int m_imported = 0;
    int m_lookedUp = 0;
    int m_unresolved = 0;
};

} // namespace pl::treefile
