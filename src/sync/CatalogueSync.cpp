#include "sync/CatalogueSync.h"

#include "db/Database.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlField>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace pl::sync {
namespace {

// Tables that are bookkeeping, not catalogue content.
const QSet<QString> kSkippedTables = {QStringLiteral("schema_migrations"),
                                      QStringLiteral("catalogue_sync")};

// The columns holding absolute photo-library paths (see PathMapping). A
// folder-scoped learned name keeps its folder's path as its scope ('global'
// otherwise, which passes through untouched).
const QSet<QString> kPathColumns = {QStringLiteral("folder.path"),
                                    QStringLiteral("rendition.path"),
                                    QStringLiteral("name_alias.scope")};

// Literal statements are flushed at about this size: big enough that a push
// is few round trips, small enough to stay a sane single statement.
constexpr qsizetype kStatementChars = 256 * 1024;
constexpr qsizetype kKeysPerDelete = 500;
constexpr int kSqliteMaxParams = 900;   // under SQLite's classic 999 limit

QString quoted(const QString &identifier)
{
    return QLatin1Char('"') + identifier + QLatin1Char('"');
}

QString columnList(const QStringList &columns)
{
    QStringList q;
    for (const QString &c : columns)
        q << quoted(c);
    return q.join(QStringLiteral(", "));
}

QString normalisedRoot(QString root)
{
    root = QDir::fromNativeSeparators(root.trimmed());
    while (root.size() > 1 && root.endsWith(QLatin1Char('/')))
        root.chop(1);
    return root;
}

Qt::CaseSensitivity pathCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

} // namespace

// --- PathMapping --------------------------------------------------------------

PathMapping::PathMapping(const QString &libraryRoot) : m_root(normalisedRoot(libraryRoot)) {}

QString PathMapping::toServer(const QString &localPath, bool *outsideRoot) const
{
    if (outsideRoot)
        *outsideRoot = false;
    if (m_root.isEmpty())
        return localPath;
    const QString path = QDir::fromNativeSeparators(localPath);
    if (path.compare(m_root, pathCase()) == 0)
        return QLatin1String(kPrefix);
    if (path.startsWith(m_root + QLatin1Char('/'), pathCase()))
        return QLatin1String(kPrefix) + path.mid(m_root.size() + 1);
    if (outsideRoot)
        *outsideRoot = true;
    return localPath;
}

QString PathMapping::toLocal(const QString &serverPath) const
{
    if (m_root.isEmpty() || !serverPath.startsWith(QLatin1String(kPrefix)))
        return serverPath;
    const QString rel = serverPath.mid(int(qstrlen(kPrefix)));
    return rel.isEmpty() ? m_root : m_root + QLatin1Char('/') + rel;
}

// --- CatalogueSync ------------------------------------------------------------

CatalogueSync::CatalogueSync(QString localConnection, QString serverConnection, PathMapping paths)
    : m_local(std::move(localConnection)),
      m_server(std::move(serverConnection)),
      m_paths(std::move(paths))
{
}

bool CatalogueSync::fail(const QString &message)
{
    m_error = message;
    return false;
}

void CatalogueSync::report(const QString &phase, int done, int total) const
{
    if (m_progress)
        m_progress(phase, done, total);
}

bool CatalogueSync::serverIsPostgres() const
{
    return Database::backendFor(m_server) == CatalogueDescriptor::Backend::Postgres;
}

QString CatalogueSync::nowExpr() const
{
    return Database::nowIsoExpr(Database::backendFor(m_server));
}

QString CatalogueSync::literal(const QVariant &value) const
{
    if (!value.isValid() || value.isNull())
        return QStringLiteral("NULL");
    QSqlField field(QString(), value.metaType());
    field.setValue(value);
    return QSqlDatabase::database(m_server, false).driver()->formatValue(field);
}

bool CatalogueSync::execServer(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(m_server, false));
    if (!q.exec(sql))
        return fail(q.lastError().text());
    return true;
}

// The local catalogue's tables, parents before children (by foreign keys),
// with their columns and primary keys.
bool CatalogueSync::loadTables()
{
    m_tables.clear();
    QSqlDatabase db = QSqlDatabase::database(m_local, false);
    QSqlQuery names(db);
    if (!names.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' "
                                   "AND name NOT LIKE 'sqlite_%' ORDER BY name")))
        return fail(names.lastError().text());

    QList<Table> tables;
    while (names.next()) {
        Table t;
        t.name = names.value(0).toString();
        if (kSkippedTables.contains(t.name))
            continue;
        QSqlQuery info(db);
        info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(quoted(t.name)));
        QList<std::pair<int, QString>> pk;
        while (info.next()) {
            const QString column = info.value(1).toString();
            if (kPathColumns.contains(t.name + QLatin1Char('.') + column))
                t.pathColumns << int(t.columns.size());
            t.columns << column;
            if (const int pos = info.value(5).toInt(); pos > 0)
                pk.append({pos, column});
        }
        std::sort(pk.begin(), pk.end());
        for (const auto &p : pk)
            t.pk << p.second;
        if (t.pk.isEmpty())
            return fail(QStringLiteral("table %1 has no primary key").arg(t.name));
        QSqlQuery fks(db);
        fks.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(quoted(t.name)));
        while (fks.next()) {
            const QString parent = fks.value(2).toString();
            if (parent != t.name && !t.parents.contains(parent))
                t.parents << parent;
        }
        tables << t;
    }

    // Parents first: repeatedly take the tables whose parents are all placed.
    QSet<QString> placed;
    while (!tables.isEmpty()) {
        bool progressed = false;
        for (qsizetype i = 0; i < tables.size();) {
            const Table &t = tables.at(i);
            const bool ready = std::all_of(t.parents.cbegin(), t.parents.cend(),
                                           [&](const QString &p) {
                                               return placed.contains(p) || kSkippedTables.contains(p);
                                           });
            if (ready) {
                placed.insert(t.name);
                m_tables << t;
                tables.removeAt(i);
                progressed = true;
            } else {
                ++i;
            }
        }
        if (!progressed)
            return fail(QStringLiteral("circular foreign keys between catalogue tables"));
    }
    return true;
}

std::optional<ServerState> CatalogueSync::serverState()
{
    QSqlDatabase db = QSqlDatabase::database(m_server, false);
    QSqlQuery q(db);
    ServerState state;
    if (!q.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")) || !q.next()) {
        fail(q.lastError().text());
        return std::nullopt;
    }
    state.schemaVersion = q.value(0).toInt();
    if (!q.exec(QStringLiteral("SELECT version, locked_by, locked_machine, locked_at, pushed_by, "
                               "pushed_at FROM catalogue_sync WHERE id = 1"))
        || !q.next()) {
        fail(QStringLiteral("the server catalogue has no sync record (%1)")
                 .arg(q.lastError().text()));
        return std::nullopt;
    }
    state.version = q.value(0).toLongLong();
    state.lockedBy = q.value(1).toString();
    state.lockedMachine = q.value(2).toString();
    state.lockedAt = q.value(3).toString();
    state.pushedBy = q.value(4).toString();
    state.pushedAt = q.value(5).toString();
    if (q.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM taxon), (SELECT COUNT(*) FROM capture), "
                               "(SELECT COUNT(*) FROM project)"))
        && q.next()) {
        state.empty = q.value(0).toLongLong() + q.value(1).toLongLong() == 0;
        state.photos = q.value(1).toInt();
        state.trees = q.value(2).toInt();
    }
    return state;
}

bool CatalogueSync::acquireLock(const SyncIdentity &who, bool breakExisting)
{
    QSqlQuery q(QSqlDatabase::database(m_server, false));
    q.prepare(QStringLiteral("UPDATE catalogue_sync SET locked_by = ?, locked_machine = ?, "
                             "locked_at = %1 WHERE id = 1 AND (%2 OR locked_by IS NULL "
                             "OR (locked_by = ? AND locked_machine = ?))")
                  .arg(nowExpr(), breakExisting ? QStringLiteral("1 = 1")
                                                : QStringLiteral("1 = 0")));
    q.addBindValue(who.user);
    q.addBindValue(who.machine);
    q.addBindValue(who.user);
    q.addBindValue(who.machine);
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail(QStringLiteral("someone else is editing the catalogue"));
    return true;
}

bool CatalogueSync::releaseLock(const SyncIdentity &who)
{
    QSqlQuery q(QSqlDatabase::database(m_server, false));
    q.prepare(QStringLiteral("UPDATE catalogue_sync SET locked_by = NULL, locked_machine = NULL, "
                             "locked_at = NULL WHERE id = 1 AND locked_by = ? "
                             "AND locked_machine = ?"));
    q.addBindValue(who.user);
    q.addBindValue(who.machine);
    if (!q.exec())
        return fail(q.lastError().text());
    return true;
}

qint64 CatalogueSync::localVersion() const
{
    QSqlQuery q(QSqlDatabase::database(m_local, false));
    if (q.exec(QStringLiteral("SELECT version FROM catalogue_sync WHERE id = 1")) && q.next())
        return q.value(0).toLongLong();
    return 0;
}

bool CatalogueSync::setLocalVersion(qint64 version)
{
    QSqlQuery q(QSqlDatabase::database(m_local, false));
    q.prepare(QStringLiteral("UPDATE catalogue_sync SET version = ? WHERE id = 1"));
    q.addBindValue(version);
    if (!q.exec())
        return fail(q.lastError().text());
    return true;
}

bool CatalogueSync::setSnapshotVersion(const QString &path, qint64 version)
{
    if (!attach(path, QStringLiteral("nextbase")))
        return false;
    QSqlQuery q(QSqlDatabase::database(m_local, false));
    q.prepare(QStringLiteral("UPDATE nextbase.catalogue_sync SET version = ? WHERE id = 1"));
    q.addBindValue(version);
    const bool ok = q.exec();
    if (!ok)
        fail(q.lastError().text());
    detach(QStringLiteral("nextbase"));
    return ok;
}

bool CatalogueSync::snapshotTo(const QString &path)
{
    const QString tmp = path + QStringLiteral(".tmp");
    QFile::remove(tmp);
    QSqlDatabase db = QSqlDatabase::database(m_local, false);
    QSqlQuery q(db);
    QString escaped = tmp;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped)))
        return fail(QStringLiteral("could not save the sync snapshot: %1").arg(q.lastError().text()));
    QFile::remove(path);
    if (!QFile::rename(tmp, path))
        return fail(QStringLiteral("could not save the sync snapshot to %1").arg(path));
    return true;
}

// --- sending ------------------------------------------------------------------

bool CatalogueSync::sendRows(const Table &table, const QList<QVariantList> &rows, bool upsert,
                             int *outsideRoot)
{
    if (rows.isEmpty())
        return true;

    // Postgres's identity ids refuse explicit values without this.
    QString head = QStringLiteral("INSERT INTO %1 (%2) ").arg(quoted(table.name), columnList(table.columns));
    if (serverIsPostgres() && table.pk == QStringList{QStringLiteral("id")})
        head += QStringLiteral("OVERRIDING SYSTEM VALUE ");
    head += QStringLiteral("VALUES ");

    QString tail;
    if (upsert) {
        QStringList sets;
        for (const QString &c : table.columns) {
            if (!table.pk.contains(c))
                sets << QStringLiteral("%1 = excluded.%1").arg(quoted(c));
        }
        tail = QStringLiteral(" ON CONFLICT (%1) DO ").arg(columnList(table.pk))
               + (sets.isEmpty() ? QStringLiteral("NOTHING") : QStringLiteral("UPDATE SET ")
                                                                   + sets.join(QStringLiteral(", ")));
    }

    QString sql;
    int sent = 0;
    auto flush = [&]() -> bool {
        if (sql.isEmpty())
            return true;
        if (!execServer(head + sql + tail))
            return fail(QStringLiteral("%1: %2").arg(table.name, m_error));
        sql.clear();
        report(QStringLiteral("Sending %1").arg(table.name), sent, int(rows.size()));
        return true;
    };

    for (const QVariantList &row : rows) {
        QStringList values;
        values.reserve(row.size());
        for (int c = 0; c < row.size(); ++c) {
            if (table.pathColumns.contains(c) && !row.at(c).isNull()) {
                bool outside = false;
                const QString path = row.at(c).toString();
                values << literal(m_paths.toServer(path, &outside));
                if (outside && outsideRoot && QDir::isAbsolutePath(path))
                    ++*outsideRoot;
            } else {
                values << literal(row.at(c));
            }
        }
        if (!sql.isEmpty())
            sql += QLatin1Char(',');
        sql += QLatin1Char('(') + values.join(QLatin1Char(',')) + QLatin1Char(')');
        ++sent;
        if (sql.size() >= kStatementChars && !flush())
            return false;
    }
    return flush();
}

bool CatalogueSync::deleteRows(const Table &table, const QList<QVariantList> &keys)
{
    for (qsizetype i = 0; i < keys.size(); i += kKeysPerDelete) {
        QStringList tuples;
        for (const QVariantList &key : keys.mid(i, kKeysPerDelete)) {
            QStringList parts;
            for (const QVariant &v : key)
                parts << literal(v);
            tuples << (parts.size() == 1 ? parts.first()
                                         : QLatin1Char('(') + parts.join(QLatin1Char(',')) + QLatin1Char(')'));
        }
        const QString target = table.pk.size() == 1
                                   ? quoted(table.pk.first())
                                   : QLatin1Char('(') + columnList(table.pk) + QLatin1Char(')');
        if (!execServer(QStringLiteral("DELETE FROM %1 WHERE %2 IN (%3)")
                            .arg(quoted(table.name), target, tuples.join(QLatin1Char(',')))))
            return fail(QStringLiteral("%1: %2").arg(table.name, m_error));
    }
    return true;
}

// After explicit ids were inserted, move each identity sequence past them so
// rows the server creates itself never collide.
bool CatalogueSync::resetIdentities(const QList<QString> &tables)
{
    if (!serverIsPostgres())
        return true;
    for (const QString &t : tables) {
        const auto it = std::find_if(m_tables.cbegin(), m_tables.cend(),
                                     [&](const Table &x) { return x.name == t; });
        if (it == m_tables.cend() || it->pk != QStringList{QStringLiteral("id")})
            continue;
        if (!execServer(QStringLiteral(
                "SELECT setval(pg_get_serial_sequence('%1', 'id'), "
                "COALESCE((SELECT MAX(id) FROM %2), 0) + 1, false)")
                            .arg(t, quoted(t))))
            return false;
    }
    return true;
}

// Moves the server to the next version -- only from `expected`, and (with
// `requireLock`) only for the lock holder. One conditional UPDATE, run first
// in the push's transaction, so it also holds the row until the commit.
bool CatalogueSync::bumpVersion(qint64 expected, const SyncIdentity &who, bool requireLock)
{
    QSqlQuery q(QSqlDatabase::database(m_server, false));
    QString sql = QStringLiteral("UPDATE catalogue_sync SET version = version + 1, pushed_at = %1, "
                                 "pushed_by = ? WHERE id = 1 AND version = ?")
                      .arg(nowExpr());
    if (requireLock)
        sql += QStringLiteral(" AND locked_by = ? AND locked_machine = ?");
    q.prepare(sql);
    q.addBindValue(who.user);
    q.addBindValue(expected);
    if (requireLock) {
        q.addBindValue(who.user);
        q.addBindValue(who.machine);
    }
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.numRowsAffected() == 1)
        return true;

    const auto state = serverState();
    if (state && !state->heldBy(who) && requireLock)
        return fail(state->isLocked()
                        ? QStringLiteral("%1 is editing the catalogue, so your changes can't be "
                                         "sent")
                              .arg(state->lockedBy)
                        : QStringLiteral("you don't hold the edit lock"));
    return fail(QStringLiteral("the shared catalogue changed after you last got it (server "
                               "version %1, yours %2)")
                    .arg(state ? state->version : -1)
                    .arg(expected));
}

// --- diff ---------------------------------------------------------------------

bool CatalogueSync::attach(const QString &path, const QString &alias)
{
    QSqlQuery q(QSqlDatabase::database(m_local, false));
    q.prepare(QStringLiteral("ATTACH DATABASE ? AS %1").arg(alias));
    q.addBindValue(path);
    if (!q.exec())
        return fail(q.lastError().text());
    return true;
}

void CatalogueSync::detach(const QString &alias)
{
    QSqlQuery(QSqlDatabase::database(m_local, false))
        .exec(QStringLiteral("DETACH DATABASE %1").arg(alias));
}

std::optional<QList<std::pair<const CatalogueSync::Table *, CatalogueSync::Diff>>>
CatalogueSync::diff(const QString &newer, const QString &older)
{
    QSqlDatabase db = QSqlDatabase::database(m_local, false);
    QList<std::pair<const Table *, Diff>> diffs;
    for (const Table &t : std::as_const(m_tables)) {
        Diff d;
        QStringList pkPositions;
        for (const QString &k : t.pk)
            pkPositions << QString::number(t.columns.indexOf(k) + 1);
        const QString cols = columnList(t.columns);
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT %1 FROM %4.%2 EXCEPT SELECT %1 FROM %5.%2 ORDER BY %3")
                        .arg(cols, quoted(t.name), pkPositions.join(QLatin1Char(',')), newer,
                             older))) {
            fail(QStringLiteral("%1: %2").arg(t.name, q.lastError().text()));
            return std::nullopt;
        }
        while (q.next()) {
            QVariantList row;
            for (int c = 0; c < t.columns.size(); ++c)
                row << q.value(c);
            d.upserts << row;
        }
        const QString keys = columnList(t.pk);
        if (!q.exec(QStringLiteral("SELECT %1 FROM %3.%2 EXCEPT SELECT %1 FROM %4.%2")
                        .arg(keys, quoted(t.name), older, newer))) {
            fail(QStringLiteral("%1: %2").arg(t.name, q.lastError().text()));
            return std::nullopt;
        }
        while (q.next()) {
            QVariantList key;
            for (int c = 0; c < t.pk.size(); ++c)
                key << q.value(c);
            d.deletes << key;
        }
        if (!d.upserts.isEmpty() || !d.deletes.isEmpty())
            diffs.append({&t, d});
    }
    return diffs;
}

std::optional<int> CatalogueSync::pendingChanges(const QString &basePath)
{
    if (!loadTables())
        return std::nullopt;
    if (!QFile::exists(basePath)) {
        fail(QStringLiteral("no sync snapshot at %1").arg(basePath));
        return std::nullopt;
    }
    if (!attach(basePath, QStringLiteral("base")))
        return std::nullopt;
    const auto diffs = diff(QStringLiteral("main"), QStringLiteral("base"));
    detach(QStringLiteral("base"));
    if (!diffs)
        return std::nullopt;
    int n = 0;
    for (const auto &[table, diff] : *diffs)
        n += int(diff.upserts.size() + diff.deletes.size());
    return n;
}

// --- operations ---------------------------------------------------------------

std::optional<CatalogueSync::PushResult> CatalogueSync::push(const QString &basePath,
                                                            const SyncIdentity &who)
{
    if (!loadTables())
        return std::nullopt;
    if (!QFile::exists(basePath)) {
        fail(QStringLiteral("no sync snapshot at %1 -- get the catalogue from the server first")
                 .arg(basePath));
        return std::nullopt;
    }
    // Freeze the catalogue first: what's sent, and the next base, are this
    // copy -- not the live file, which the user may be changing meanwhile.
    report(QStringLiteral("Finding your changes"), 0, 0);
    const QString next = basePath + QStringLiteral(".next");
    if (!snapshotTo(next))
        return std::nullopt;
    if (!attach(next, QStringLiteral("snap")))
        return std::nullopt;
    if (!attach(basePath, QStringLiteral("base"))) {
        detach(QStringLiteral("snap"));
        return std::nullopt;
    }
    const auto diffs = diff(QStringLiteral("snap"), QStringLiteral("base"));
    detach(QStringLiteral("snap"));
    detach(QStringLiteral("base"));
    auto discardNext = [&] { QFile::remove(next); };
    if (!diffs) {
        discardNext();
        return std::nullopt;
    }

    const qint64 base = localVersion();
    PushResult result;
    result.newVersion = base;
    for (const auto &[table, diff] : *diffs)
        result.changedRows += int(diff.upserts.size() + diff.deletes.size());
    if (result.changedRows == 0) {
        discardNext();
        return result;   // nothing to send
    }

    QSqlDatabase server = QSqlDatabase::database(m_server, false);
    if (!server.transaction()) {
        fail(server.lastError().text());
        discardNext();
        return std::nullopt;
    }
    auto abort = [&]() -> std::optional<PushResult> {
        server.rollback();
        discardNext();
        return std::nullopt;
    };
    if (!bumpVersion(base, who, true))
        return abort();
    // Children's deletes first, then parents' inserts/updates.
    for (auto it = diffs->crbegin(); it != diffs->crend(); ++it) {
        if (!deleteRows(*it->first, it->second.deletes))
            return abort();
    }
    QList<QString> inserted;
    for (const auto &[table, diff] : *diffs) {
        if (!sendRows(*table, diff.upserts, true, &result.outsideRoot))
            return abort();
        if (!diff.upserts.isEmpty())
            inserted << table->name;
    }
    if (!resetIdentities(inserted))
        return abort();
    if (!server.commit()) {
        fail(server.lastError().text());
        return abort();
    }

    result.newVersion = base + 1;
    // The frozen copy is exactly what the server now has: it's the new base.
    // It was taken before the push, so it still says the old version.
    if (!setSnapshotVersion(next, result.newVersion)) {
        fail(QStringLiteral("sent, but could not update the sync snapshot: %1").arg(m_error));
        return std::nullopt;
    }
    QFile::remove(basePath);
    if (!QFile::rename(next, basePath) || !setLocalVersion(result.newVersion)) {
        fail(QStringLiteral("sent, but could not update the sync snapshot"));
        return std::nullopt;
    }
    return result;
}

std::optional<CatalogueSync::PushResult> CatalogueSync::publish(const QString &basePath,
                                                               const SyncIdentity &who)
{
    if (!loadTables())
        return std::nullopt;
    const auto state = serverState();
    if (!state)
        return std::nullopt;
    const int localSchema = [&] {
        QSqlQuery q(QSqlDatabase::database(m_local, false));
        return q.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")) && q.next()
                   ? q.value(0).toInt()
                   : -1;
    }();
    if (state->schemaVersion != localSchema) {
        fail(QStringLiteral("the server catalogue is at schema version %1, this one at %2")
                 .arg(state->schemaVersion)
                 .arg(localSchema));
        return std::nullopt;
    }
    if (!acquireLock(who))
        return std::nullopt;

    // Publish a frozen copy, which then becomes the base (see push()).
    const QString next = basePath + QStringLiteral(".next");
    if (!snapshotTo(next) || !attach(next, QStringLiteral("snap")))
        return std::nullopt;
    QSqlDatabase server = QSqlDatabase::database(m_server, false);
    if (!server.transaction()) {
        fail(server.lastError().text());
        detach(QStringLiteral("snap"));
        QFile::remove(next);
        return std::nullopt;
    }
    auto abort = [&]() -> std::optional<PushResult> {
        server.rollback();
        detach(QStringLiteral("snap"));
        QFile::remove(next);
        return std::nullopt;
    };
    if (!bumpVersion(state->version, who, true))
        return abort();

    // Empty the server, children first.
    report(QStringLiteral("Clearing the server catalogue"), 0, 0);
    if (serverIsPostgres()) {
        QStringList names;
        for (const Table &t : std::as_const(m_tables))
            names << quoted(t.name);
        if (!execServer(QStringLiteral("TRUNCATE %1").arg(names.join(QStringLiteral(", ")))))
            return abort();
    } else {
        for (auto it = m_tables.crbegin(); it != m_tables.crend(); ++it) {
            if (!execServer(QStringLiteral("DELETE FROM %1").arg(quoted(it->name))))
                return abort();
        }
    }

    PushResult result;
    QList<QString> all;
    QSqlDatabase local = QSqlDatabase::database(m_local, false);
    for (const Table &t : std::as_const(m_tables)) {
        QSqlQuery q(local);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT %1 FROM snap.%2 ORDER BY %3")
                        .arg(columnList(t.columns), quoted(t.name), columnList(t.pk)))) {
            fail(q.lastError().text());
            return abort();
        }
        QList<QVariantList> rows;
        while (q.next()) {
            QVariantList row;
            for (int c = 0; c < t.columns.size(); ++c)
                row << q.value(c);
            rows << row;
        }
        if (!sendRows(t, rows, false, &result.outsideRoot))
            return abort();
        result.changedRows += int(rows.size());
        all << t.name;
    }
    if (!resetIdentities(all))
        return abort();
    if (!server.commit()) {
        fail(server.lastError().text());
        return abort();
    }
    detach(QStringLiteral("snap"));
    result.newVersion = state->version + 1;
    if (!setSnapshotVersion(next, result.newVersion)) {
        fail(QStringLiteral("published, but could not save the sync snapshot: %1").arg(m_error));
        return std::nullopt;
    }
    QFile::remove(basePath);
    if (!QFile::rename(next, basePath) || !setLocalVersion(result.newVersion)) {
        fail(QStringLiteral("published, but could not save the sync snapshot"));
        return std::nullopt;
    }
    return result;
}

std::optional<qint64> CatalogueSync::pull()
{
    if (!loadTables())
        return std::nullopt;
    QSqlDatabase local = QSqlDatabase::database(m_local, false);
    QSqlDatabase server = QSqlDatabase::database(m_server, false);

    {
        QSqlQuery q(local);
        const int localSchema = q.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations"))
                                        && q.next()
                                    ? q.value(0).toInt()
                                    : -1;
        const auto state = serverState();
        if (!state)
            return std::nullopt;
        if (state->schemaVersion != localSchema) {
            fail(state->schemaVersion > localSchema
                     ? QStringLiteral("the shared catalogue needs a newer PhotoLife (schema %1, "
                                      "this version has %2)")
                           .arg(state->schemaVersion)
                           .arg(localSchema)
                     : QStringLiteral("the shared catalogue is at an older schema (%1) than "
                                      "this PhotoLife (%2)")
                           .arg(state->schemaVersion)
                           .arg(localSchema));
            return std::nullopt;
        }
    }

    // One consistent view of the server for the whole copy.
    if (!server.transaction()) {
        fail(server.lastError().text());
        return std::nullopt;
    }
    if (serverIsPostgres())
        execServer(QStringLiteral("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY"));

    QSqlQuery(local).exec(QStringLiteral("PRAGMA foreign_keys = OFF"));
    if (!local.transaction()) {
        server.rollback();
        fail(local.lastError().text());
        return std::nullopt;
    }
    auto abort = [&]() -> std::optional<qint64> {
        local.rollback();
        server.rollback();
        QSqlQuery(local).exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        return std::nullopt;
    };

    qint64 version = 0;
    {
        QSqlQuery v(server);
        if (!v.exec(QStringLiteral("SELECT version FROM catalogue_sync WHERE id = 1")) || !v.next()) {
            fail(v.lastError().text());
            return abort();
        }
        version = v.value(0).toLongLong();
    }

    int tableIndex = 0;
    for (const Table &t : std::as_const(m_tables)) {
        report(QStringLiteral("Getting %1").arg(t.name), tableIndex++, int(m_tables.size()));
        QSqlQuery clear(local);
        if (!clear.exec(QStringLiteral("DELETE FROM %1").arg(quoted(t.name)))) {
            fail(clear.lastError().text());
            return abort();
        }
        QSqlQuery src(server);
        src.setForwardOnly(true);
        if (!src.exec(QStringLiteral("SELECT %1 FROM %2").arg(columnList(t.columns), quoted(t.name)))) {
            fail(QStringLiteral("%1: %2").arg(t.name, src.lastError().text()));
            return abort();
        }

        const int perRow = int(t.columns.size());
        const int rowsPerStatement = std::max(1, kSqliteMaxParams / perRow);
        const QString rowPlaceholder = QStringLiteral("(%1)").arg(
            QStringList(perRow, QStringLiteral("?")).join(QLatin1Char(',')));
        auto insertSql = [&](int rows) {
            return QStringLiteral("INSERT INTO %1 (%2) VALUES %3")
                .arg(quoted(t.name), columnList(t.columns),
                     QStringList(rows, rowPlaceholder).join(QLatin1Char(',')));
        };
        QSqlQuery full(local);
        full.prepare(insertSql(rowsPerStatement));
        QVariantList pending;
        int pendingRows = 0;
        auto flush = [&](QSqlQuery &q) -> bool {
            for (const QVariant &v : std::as_const(pending))
                q.addBindValue(v);
            if (!q.exec())
                return fail(QStringLiteral("%1: %2").arg(t.name, q.lastError().text()));
            pending.clear();
            pendingRows = 0;
            return true;
        };
        while (src.next()) {
            for (int c = 0; c < perRow; ++c) {
                const QVariant v = src.value(c);
                pending << (t.pathColumns.contains(c) && !v.isNull()
                                ? QVariant(m_paths.toLocal(v.toString()))
                                : v);
            }
            if (++pendingRows == rowsPerStatement && !flush(full))
                return abort();
        }
        if (pendingRows > 0) {
            QSqlQuery last(local);
            last.prepare(insertSql(pendingRows));
            if (!flush(last))
                return abort();
        }
    }

    if (!setLocalVersion(version)) {
        return abort();
    }
    if (!local.commit()) {
        fail(local.lastError().text());
        return abort();
    }
    server.commit();
    QSqlQuery(local).exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    report(QStringLiteral("Done"), int(m_tables.size()), int(m_tables.size()));
    return version;
}

} // namespace pl::sync
