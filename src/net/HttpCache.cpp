#include "net/HttpCache.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QVariant>
#include <QtDebug>

#include <atomic>

namespace pl::net {
namespace {

int nextConnectionSuffix()
{
    static std::atomic_int counter{0};
    return counter.fetch_add(1);
}

const QString kNowExpr = QStringLiteral("strftime('%Y-%m-%dT%H:%M:%fZ','now')");

} // namespace

HttpCache::HttpCache()
    : m_connectionName(QStringLiteral("photolife-http-cache-%1").arg(nextConnectionSuffix()))
{
}

HttpCache::~HttpCache()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool HttpCache::open(const QString &path, int maxAgeDays, qint64 maxBytes)
{
    if (path != QStringLiteral(":memory:"))
        QDir().mkpath(QFileInfo(path).absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
        qWarning() << "HTTP cache disabled: cannot open" << path << db.lastError().text();
        return false;
    }

    QSqlQuery q(db);
    // auto_vacuum only takes effect on a database with no tables yet, i.e.
    // when this file is first created; it lets prune() hand freed pages back.
    q.exec(QStringLiteral("PRAGMA auto_vacuum = INCREMENTAL"));
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS http_cache ("
                               "  url           TEXT PRIMARY KEY,"
                               "  etag          TEXT,"
                               "  last_modified TEXT,"
                               "  status        INTEGER,"
                               "  body          BLOB,"
                               "  used_at       TEXT NOT NULL"
                               ")"))) {
        qWarning() << "HTTP cache disabled: cannot create table" << q.lastError().text();
        db.close();
        return false;
    }

    prune(maxAgeDays, maxBytes);
    return true;
}

bool HttpCache::isOpen() const
{
    return QSqlDatabase::contains(m_connectionName)
           && QSqlDatabase::database(m_connectionName, false).isOpen();
}

bool HttpCache::isCacheable(const QUrl &url)
{
    const QUrlQuery query(url);
    if (query.hasQueryItem(QStringLiteral("page")) || query.hasQueryItem(QStringLiteral("id_above")))
        return false;

    const QString path = url.path();
    if (path.contains(QStringLiteral("/observations")))
        return false;

    const QString lastSegment = path.section(QLatin1Char('/'), -1);
    return !lastSegment.contains(QLatin1Char(','));
}

std::optional<HttpCache::Entry> HttpCache::lookup(const QString &url) const
{
    if (!isOpen())
        return std::nullopt;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
        "SELECT body, etag, last_modified, status FROM http_cache WHERE url = ?"));
    q.addBindValue(url);
    if (!q.exec() || !q.next())
        return std::nullopt;

    Entry e;
    e.body = q.value(0).toByteArray();
    e.etag = q.value(1).toString();
    e.lastModified = q.value(2).toString();
    e.status = q.value(3).toInt();
    return e;
}

bool HttpCache::store(const QString &url, const QString &etag, const QString &lastModified,
                      int status, const QByteArray &body)
{
    if (!isOpen())
        return false;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral(
                  "INSERT INTO http_cache (url, etag, last_modified, status, body, used_at) "
                  "VALUES (?, ?, ?, ?, ?, %1) "
                  "ON CONFLICT(url) DO UPDATE SET etag = excluded.etag, "
                  "  last_modified = excluded.last_modified, status = excluded.status, "
                  "  body = excluded.body, used_at = excluded.used_at")
                  .arg(kNowExpr));
    q.addBindValue(url);
    q.addBindValue(etag.isEmpty() ? QVariant() : etag);
    q.addBindValue(lastModified.isEmpty() ? QVariant() : lastModified);
    q.addBindValue(status);
    q.addBindValue(body);
    return q.exec();
}

void HttpCache::touch(const QString &url)
{
    if (!isOpen())
        return;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    q.prepare(QStringLiteral("UPDATE http_cache SET used_at = %1 WHERE url = ?").arg(kNowExpr));
    q.addBindValue(url);
    q.exec();
}

void HttpCache::prune(int maxAgeDays, qint64 maxBytes)
{
    if (!isOpen())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    QSqlQuery expire(db);
    expire.prepare(QStringLiteral(
        "DELETE FROM http_cache WHERE used_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', ?)"));
    expire.addBindValue(QStringLiteral("-%1 days").arg(maxAgeDays));
    expire.exec();

    // Keep the most recently used entries whose bodies fit in the budget.
    QSqlQuery trim(db);
    trim.prepare(QStringLiteral(
        "DELETE FROM http_cache WHERE url IN ("
        "  SELECT url FROM ("
        "    SELECT url, SUM(COALESCE(LENGTH(body), 0)) "
        "           OVER (ORDER BY used_at DESC, url ROWS UNBOUNDED PRECEDING) AS running "
        "    FROM http_cache) "
        "  WHERE running > ?)"));
    trim.addBindValue(maxBytes);
    trim.exec();

    QSqlQuery vacuum(db);
    vacuum.exec(QStringLiteral("PRAGMA incremental_vacuum"));
}

qint64 HttpCache::totalBodyBytes() const
{
    if (!isOpen())
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    if (!q.exec(QStringLiteral("SELECT COALESCE(SUM(LENGTH(body)), 0) FROM http_cache"))
        || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

int HttpCache::entryCount() const
{
    if (!isOpen())
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_connectionName, false));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM http_cache")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

} // namespace pl::net
