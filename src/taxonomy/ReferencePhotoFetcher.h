#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace pl::net {
class INatClient;
}

namespace pl::taxonomy {

class TaxonomyStore;

// Fills in the cached iNaturalist reference photo (default_photo) for a
// reference tree's species that don't have one yet. Walks the project's
// leaf-rank taxa in batches of 30 through GET /v1/taxa/{ids}, and upserts each
// returned taxon so its photo_url / photo_attribution land in the cache.
//
// One HTTP request per 30 taxa, so a large tree takes a handful of rate-limited
// requests. Safe to re-run: taxa that already have a photo are skipped.
//
// Drive it from the thread that owns the INatClient / store connection.
class ReferencePhotoFetcher : public QObject
{
    Q_OBJECT

public:
    ReferencePhotoFetcher(pl::net::INatClient &inat, TaxonomyStore &store,
                          QObject *parent = nullptr);

    void start(int projectId);
    void cancel() { m_cancelled = true; }
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString &error, int updated);

private:
    void fetchNext();
    void fail(const QString &error);
    void succeed();
    bool checkCancelled();

    pl::net::INatClient &m_inat;
    TaxonomyStore &m_store;

    bool m_running = false;
    bool m_cancelled = false;
    int m_projectId = -1;

    QList<QList<qint64>> m_batches;   // leaf taxon ids, 30 per batch
    int m_index = 0;
    int m_done = 0;                   // taxa processed, for progress
    int m_total = 0;
    int m_updated = 0;
};

} // namespace pl::taxonomy
