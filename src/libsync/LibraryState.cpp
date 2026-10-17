#include "libsync/LibraryState.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace pl::libsync {

LibraryState::LibraryState(QString path)
    : m_path(std::move(path)),
      m_connection(QStringLiteral("libsync-state-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

LibraryState::~LibraryState()
{
    if (QSqlDatabase::contains(m_connection)) {
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool LibraryState::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    if (!q.exec(sql)) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryState::open()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        m_error = db.lastError().text();
        return false;
    }
    return exec(QStringLiteral("PRAGMA journal_mode = WAL"))
           && exec(QStringLiteral("PRAGMA synchronous = NORMAL"))
           && exec(QStringLiteral("CREATE TABLE IF NOT EXISTS synced_file ("
                                  " rel_path       TEXT PRIMARY KEY,"
                                  " md5            TEXT NOT NULL,"
                                  " sha256         TEXT NOT NULL DEFAULT '',"
                                  " size           INTEGER NOT NULL,"
                                  " mtime          INTEGER NOT NULL,"
                                  " remote_version TEXT NOT NULL,"
                                  " local          INTEGER NOT NULL DEFAULT 1)"))
           && exec(QStringLiteral("CREATE TABLE IF NOT EXISTS remote_file ("
                                  " rel_path    TEXT PRIMARY KEY,"
                                  " size        INTEGER NOT NULL,"
                                  " version     TEXT NOT NULL,"
                                  " md5         TEXT NOT NULL DEFAULT '',"
                                  " sha256      TEXT NOT NULL DEFAULT '',"
                                  " uploaded_by TEXT NOT NULL DEFAULT '',"
                                  " remote_id   TEXT NOT NULL DEFAULT '')"))
           && exec(QStringLiteral("CREATE TABLE IF NOT EXISTS pinned (rel_path TEXT PRIMARY KEY)"))
           && exec(QStringLiteral("CREATE TABLE IF NOT EXISTS kept_online ("
                                  " rel_path       TEXT PRIMARY KEY,"
                                  " remote_version TEXT NOT NULL)"));
}

bool LibraryState::replaceRemote(const QList<RemoteEntry> &entries)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    if (!db.transaction() || !exec(QStringLiteral("DELETE FROM remote_file"))) {
        m_error = db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO remote_file (rel_path, size, version, md5, sha256, uploaded_by, remote_id)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"));
    for (const RemoteEntry &e : entries) {
        q.addBindValue(e.relPath);
        q.addBindValue(e.size);
        q.addBindValue(e.version);
        q.addBindValue(e.md5);
        q.addBindValue(e.sha256);
        q.addBindValue(e.uploadedBy);
        q.addBindValue(e.remoteId);
        if (!q.exec()) {
            m_error = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    return db.commit();
}

QHash<QString, RemoteEntry> LibraryState::remote()
{
    QHash<QString, RemoteEntry> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT rel_path, size, version, md5, sha256, uploaded_by, remote_id FROM remote_file"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        RemoteEntry e;
        e.relPath = q.value(0).toString();
        e.size = q.value(1).toLongLong();
        e.version = q.value(2).toString();
        e.md5 = q.value(3).toString();
        e.sha256 = q.value(4).toString();
        e.uploadedBy = q.value(5).toString();
        e.remoteId = q.value(6).toString();
        out.insert(e.relPath, e);
    }
    return out;
}

bool LibraryState::pin(const QString &relPath)
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO pinned (rel_path) VALUES (?)"));
    q.addBindValue(relPath);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

QSet<QString> LibraryState::pinned()
{
    QSet<QString> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT rel_path FROM pinned"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next())
        out.insert(q.value(0).toString());
    return out;
}

QHash<QString, SyncedFile> LibraryState::load()
{
    QHash<QString, SyncedFile> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT rel_path, md5, sha256, size, mtime, remote_version, local FROM synced_file"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        SyncedFile f;
        f.relPath = q.value(0).toString();
        f.md5 = q.value(1).toString();
        f.sha256 = q.value(2).toString();
        f.size = q.value(3).toLongLong();
        f.mtime = q.value(4).toLongLong();
        f.remoteVersion = q.value(5).toString();
        f.local = q.value(6).toInt() != 0;
        out.insert(f.relPath, f);
    }
    return out;
}

bool LibraryState::put(const SyncedFile &file)
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO synced_file (rel_path, md5, sha256, size, mtime, remote_version, local)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(file.relPath);
    q.addBindValue(file.md5);
    q.addBindValue(file.sha256);
    q.addBindValue(file.size);
    q.addBindValue(file.mtime);
    q.addBindValue(file.remoteVersion);
    q.addBindValue(file.local ? 1 : 0);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryState::remove(const QString &relPath)
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("DELETE FROM synced_file WHERE rel_path = ?"));
    q.addBindValue(relPath);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryState::keepOnline(const QString &relPath, const QString &remoteVersion)
{
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO kept_online (rel_path, remote_version) VALUES (?, ?)"));
    q.addBindValue(relPath);
    q.addBindValue(remoteVersion);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

QHash<QString, QString> LibraryState::keptOnline()
{
    QHash<QString, QString> out;
    QSqlQuery q(QSqlDatabase::database(m_connection, false));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT rel_path, remote_version FROM kept_online"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next())
        out.insert(q.value(0).toString(), q.value(1).toString());
    return out;
}

bool LibraryState::forget(const QString &relPath)
{
    for (const char *table : {"synced_file", "remote_file", "kept_online", "pinned"}) {
        QSqlQuery q(QSqlDatabase::database(m_connection, false));
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE rel_path = ?").arg(QLatin1String(table)));
        q.addBindValue(relPath);
        if (!q.exec()) {
            m_error = q.lastError().text();
            return false;
        }
    }
    return true;
}

bool LibraryState::begin()
{
    return QSqlDatabase::database(m_connection, false).transaction();
}

bool LibraryState::commit()
{
    return QSqlDatabase::database(m_connection, false).commit();
}

} // namespace pl::libsync
