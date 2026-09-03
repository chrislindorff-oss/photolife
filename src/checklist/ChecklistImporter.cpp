#include "checklist/ChecklistImporter.h"

#include "match/NameParser.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

namespace pl::checklist {
namespace {
constexpr int kBatchSize = 30;

QString genusToken(const QString &name)
{
    return name.section(QLatin1Char(' '), 0, 0).toLower();
}
} // namespace

ChecklistImporter::ChecklistImporter(pl::net::INatClient &inat, pl::taxonomy::TaxonomyStore &store,
                                     QObject *parent)
    : QObject(parent), m_inat(inat), m_store(store)
{
}

bool ChecklistImporter::stop()
{
    if (!m_running)
        return true;
    if (m_cancelled) {
        fail(QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void ChecklistImporter::start(const Request &request)
{
    if (m_running)
        return;
    m_request = request;
    m_running = true;
    m_cancelled = false;
    m_index = m_imported = m_skipped = m_unresolved = 0;
    m_known.clear();
    m_missingAncestors.clear();
    m_batches.clear();
    m_batchIndex = 0;

    if (m_request.projectId < 0) {
        fail(QStringLiteral("no project to import into"));
        return;
    }

    m_projectGenera.clear();
    for (const QString &g : m_store.projectGenera(m_request.projectId))
        m_projectGenera.insert(g);

    processNext();
}

void ChecklistImporter::processNext()
{
    // Handle rows that need no network call in a loop; only return to the event
    // loop (awaiting a callback) when a lookup is actually issued.
    while (true) {
        if (stop())
            return;

        if (m_index >= m_request.entries.size()) {
            fillAncestors();
            return;
        }

        if ((m_index % 25) == 0)
            emit progress(m_index, m_request.entries.size());

        const ChecklistEntry entry = m_request.entries.at(m_index++);
        const match::ParsedName parsed = match::parseName(entry.name);

        if (m_request.onlyKnownGenera && !parsed.genus.isEmpty()
            && !m_projectGenera.contains(parsed.genus.toLower())
            && !m_projectGenera.contains(genusToken(entry.name))) {
            ++m_skipped;
            continue;
        }

        const QString folded = pl::taxonomy::TaxonomyStore::foldName(entry.name);

        auto finishRow = [this, entry, folded](qint64 taxonInatId, const QString &acceptedName,
                                               const QString &ancestry) {
            m_known.insert(taxonInatId);

            if (pl::taxonomy::TaxonomyStore::foldName(acceptedName) != folded)
                m_store.addName(taxonInatId, entry.name, QStringLiteral("synonym"));

            if (!entry.status.isEmpty()) {
                pl::taxonomy::StatusRecord status;
                status.placeInatId = m_request.placeInatId;
                status.status = entry.status;
                status.source = m_request.source.isEmpty() ? QStringLiteral("checklist")
                                                           : m_request.source;
                m_store.addStatus(taxonInatId, status);
            }

            m_store.addProjectTaxon(m_request.projectId, taxonInatId, true, true);
            ++m_imported;

            for (const QString &part : ancestry.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                bool ok = false;
                const qint64 id = part.toLongLong(&ok);
                if (ok && !m_known.contains(id) && !m_store.taxonLocalId(id))
                    m_missingAncestors.insert(id);
            }
        };

        // Already cached (from the iNat pull or an earlier import)? No network call.
        if (const auto cached = m_store.taxonInatIdByFoldedName(folded)) {
            finishRow(*cached, entry.name, QString());
            continue;
        }

        m_inat.searchTaxa(entry.name, QString(),
            [this, entry, finishRow](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
                if (stop())
                    return;
                if (!out.ok()) {
                    fail(QStringLiteral("lookup failed for \"%1\": %2").arg(entry.name, out.error));
                    return;
                }

                const pl::taxonomy::Taxon *chosen = nullptr;
                for (const auto &t : out.value) {
                    if (t.isActive) {
                        chosen = &t;
                        break;
                    }
                }
                if (!chosen && !out.value.isEmpty())
                    chosen = &out.value.first();

                if (!chosen) {
                    ++m_unresolved;
                } else {
                    m_store.upsertTaxon(*chosen);
                    finishRow(chosen->inatId, chosen->name, chosen->ancestry);
                }
                processNext();
            });
        return;   // await the callback
    }
}

void ChecklistImporter::fillAncestors()
{
    if (stop())
        return;

    QList<qint64> pending;
    for (qint64 id : m_missingAncestors) {
        if (id > 0 && !m_store.taxonLocalId(id))
            pending.append(id);
    }
    for (int i = 0; i < pending.size(); i += kBatchSize)
        m_batches.append(pending.mid(i, kBatchSize));
    m_batchIndex = 0;
    fillNextBatch();
}

void ChecklistImporter::fillNextBatch()
{
    if (stop())
        return;
    if (m_batchIndex >= m_batches.size()) {
        succeed();
        return;
    }

    const QList<qint64> batch = m_batches.at(m_batchIndex++);
    m_inat.fetchTaxa(batch, [this](pl::net::Outcome<QList<pl::taxonomy::Taxon>> out) {
        if (stop())
            return;
        if (out.ok()) {
            for (const auto &t : out.value) {
                m_store.upsertTaxon(t);
                m_store.addProjectTaxon(m_request.projectId, t.inatId, false, false);
            }
        }
        fillNextBatch();
    });
}

void ChecklistImporter::fail(const QString &error)
{
    m_running = false;
    emit finished(false, error, m_imported, m_skipped, m_unresolved);
}

void ChecklistImporter::succeed()
{
    m_store.setProjectRefreshedNow(m_request.projectId);
    m_running = false;
    emit progress(m_request.entries.size(), m_request.entries.size());
    emit finished(true, QString(), m_imported, m_skipped, m_unresolved);
}

} // namespace pl::checklist
