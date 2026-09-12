#include "taxonomy/ReferencePhotoFetcher.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

namespace pl::taxonomy {
namespace {
constexpr int kBatchSize = 30;   // GET /v1/taxa/{ids} accepts up to 30 ids
} // namespace

ReferencePhotoFetcher::ReferencePhotoFetcher(pl::net::INatClient &inat, TaxonomyStore &store,
                                             QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

void ReferencePhotoFetcher::start(int projectId)
{
    if (m_running)
        return;
    m_running = true;
    m_cancelled = false;
    m_projectId = projectId;
    m_index = 0;
    m_done = 0;
    m_updated = 0;

    m_batches.clear();
    const QList<qint64> pending = m_store.projectLeafTaxaMissingPhoto(projectId);
    m_total = int(pending.size());
    for (int i = 0; i < pending.size(); i += kBatchSize)
        m_batches.append(pending.mid(i, kBatchSize));

    emit progress(0, m_total);
    fetchNext();
}

bool ReferencePhotoFetcher::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void ReferencePhotoFetcher::fetchNext()
{
    if (checkCancelled())
        return;
    if (m_index >= m_batches.size()) {
        succeed();
        return;
    }

    const QList<qint64> batch = m_batches.at(m_index);
    m_inat.fetchTaxa(batch, [this, batch](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
        if (checkCancelled())
            return;
        if (!out.ok()) {
            fail(out.error);
            return;
        }

        for (const Taxon &taxon : out.value) {
            if (taxon.inatId <= 0 || taxon.photoUrl.isEmpty())
                continue;
            if (m_store.upsertTaxon(taxon) > 0)
                ++m_updated;
        }

        m_done += batch.size();
        ++m_index;
        emit progress(m_done, m_total);
        fetchNext();
    });
}

void ReferencePhotoFetcher::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, m_updated);
}

void ReferencePhotoFetcher::succeed()
{
    m_running = false;
    emit finished(true, QString(), m_updated);
}

} // namespace pl::taxonomy
