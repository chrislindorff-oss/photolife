#include "taxonomy/CommonNameFetcher.h"

#include "net/INatClient.h"
#include "taxonomy/TaxonomyStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace pl::taxonomy {
namespace {
constexpr int kBatchSize = 30;   // GET /v1/taxa/{ids} accepts up to 30 ids
} // namespace

CommonNameFetcher::CommonNameFetcher(pl::net::INatClient &inat, TaxonomyStore &store,
                                     QString recordPath, QObject *parent)
    : QObject(parent),
      m_inat(inat),
      m_store(store),
      m_recordPath(std::move(recordPath)),
      m_connection(QStringLiteral("common-names-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

CommonNameFetcher::~CommonNameFetcher()
{
    if (QSqlDatabase::contains(m_connection)) {
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

QString CommonNameFetcher::recordPathFor(const QString &databasePath)
{
    return QFileInfo(databasePath).absoluteDir().filePath(QStringLiteral("common_names.db"));
}

bool CommonNameFetcher::openRecord()
{
    if (QSqlDatabase::contains(m_connection))
        return QSqlDatabase::database(m_connection, false).isOpen();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_recordPath);
    if (!db.open())
        return false;
    QSqlQuery q(db);
    return q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS fetched ("
                                 " inat_id    INTEGER PRIMARY KEY,"
                                 " fetched_at TEXT NOT NULL)"));
}

QList<qint64> CommonNameFetcher::due(Scope scope, bool force)
{
    QList<qint64> out;
    for (const auto &[id, tier] : m_store.treeTaxaByNamePriority()) {
        if (scope == Scope::All || tier == 0)
            out << id;
    }
    if (force || !openRecord())
        return out;
    QSet<qint64> fresh;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("SELECT inat_id FROM fetched WHERE fetched_at >= ?"));
    q.addBindValue(QDateTime::currentDateTimeUtc().addDays(-kRefreshDays).toString(Qt::ISODate));
    if (q.exec()) {
        while (q.next())
            fresh.insert(q.value(0).toLongLong());
    }
    out.removeIf([&](qint64 id) { return fresh.contains(id); });
    return out;
}

void CommonNameFetcher::start(Scope scope, bool force)
{
    if (m_running)
        return;
    m_running = true;
    m_scope = scope;
    m_cancelled = false;
    m_index = 0;
    m_done = 0;
    m_updated = 0;
    m_batches.clear();
    if (!openRecord()) {
        finish(false, tr("couldn't open %1").arg(m_recordPath));
        return;
    }
    const QList<qint64> pending = due(scope, force);
    m_total = int(pending.size());
    for (qsizetype i = 0; i < pending.size(); i += kBatchSize)
        m_batches.append(pending.mid(i, kBatchSize));
    emit progress(0, m_total);
    fetchNext();
}

bool CommonNameFetcher::checkCancelled()
{
    if (!m_running)
        return true;   // already finished: swallow any late callback
    if (m_cancelled) {
        finish(false, QStringLiteral("cancelled"));
        return true;
    }
    return false;
}

void CommonNameFetcher::fetchNext()
{
    if (checkCancelled())
        return;
    if (m_index >= m_batches.size()) {
        finish(true, QString());
        return;
    }
    const QList<qint64> batch = m_batches.at(m_index);
    m_inat.fetchTaxa(
        batch,
        [this, batch](pl::net::Outcome<QList<Taxon>> out) {
            if (checkCancelled())
                return;
            if (!out.ok()) {
                finish(false, out.error);
                return;
            }
            QList<Taxon> full;
            for (const Taxon &t : std::as_const(out.value)) {
                if (t.inatId > 0 && t.allNames)
                    full << t;
            }
            const int before = m_store.commonNameCount(batch);
            if (!full.isEmpty() && m_store.upsertTaxa(full).isEmpty()) {
                finish(false, tr("couldn't save the names"));
                return;
            }
            m_updated += std::max(0, m_store.commonNameCount(batch) - before);

            // Recorded per batch, so a stopped run carries on from here.
            QSqlDatabase db = QSqlDatabase::database(m_connection, false);
            db.transaction();
            const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            for (const Taxon &t : std::as_const(full)) {
                QSqlQuery q(db);
                q.prepare(QStringLiteral("INSERT OR REPLACE INTO fetched (inat_id, fetched_at) VALUES (?, ?)"));
                q.addBindValue(qlonglong(t.inatId));
                q.addBindValue(now);
                q.exec();
            }
            db.commit();

            m_done += int(batch.size());
            ++m_index;
            emit progress(m_done, m_total);
            fetchNext();
        },
        /*allNames=*/true);
}

void CommonNameFetcher::finish(bool ok, const QString &error)
{
    m_running = false;
    emit finished(ok, error, m_updated);
}

} // namespace pl::taxonomy
