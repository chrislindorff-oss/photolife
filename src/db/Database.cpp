#include "db/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>

namespace pl {
namespace {

// Migrations live at :/migrations/<dialect>/NNN_description.sql and are
// applied in ascending NNN order. NNN is the schema version the script
// produces. The sqlite/ and postgres/ subtrees must expose the same set of
// version numbers -- see tst_database.cpp.
struct Migration
{
    int version;
    QString resourcePath;
};

QString migrationsDir(CatalogueDescriptor::Backend backend)
{
    return backend == CatalogueDescriptor::Backend::Postgres
               ? QStringLiteral(":/migrations/postgres")
               : QStringLiteral(":/migrations/sqlite");
}

QList<Migration> discoverMigrations(CatalogueDescriptor::Backend backend)
{
    QList<Migration> migrations;
    const QRegularExpression pattern(QStringLiteral("^(\\d+)_.*\\.sql$"));

    QDir dir(migrationsDir(backend));
    const QStringList entries = dir.entryList(QStringList{QStringLiteral("*.sql")},
                                              QDir::Files, QDir::Name);
    for (const QString &name : entries) {
        const QRegularExpressionMatch m = pattern.match(name);
        if (!m.hasMatch())
            continue;
        migrations.push_back({m.captured(1).toInt(), dir.filePath(name)});
    }

    std::sort(migrations.begin(), migrations.end(),
              [](const Migration &a, const Migration &b) { return a.version < b.version; });
    return migrations;
}

// Splits a migration script into individual statements. Our migration SQL is
// authored without semicolons inside string literals or triggers, so a plain
// split on ';' terminators is sufficient and predictable.
QStringList splitStatements(const QString &sql)
{
    QStringList statements;
    QString current;
    for (const QString &rawLine : sql.split(QLatin1Char('\n'))) {
        QString line = rawLine;
        const int comment = line.indexOf(QStringLiteral("--"));
        if (comment >= 0)
            line.truncate(comment);
        current += line;
        current += QLatin1Char('\n');
        if (line.trimmed().endsWith(QLatin1Char(';'))) {
            const QString trimmed = current.trimmed();
            if (!trimmed.isEmpty())
                statements << trimmed;
            current.clear();
        }
    }
    if (!current.trimmed().isEmpty())
        statements << current.trimmed();
    return statements;
}

int nextConnectionSuffix()
{
    static std::atomic_int counter{0};
    return counter.fetch_add(1);
}

// A managed instance that has gone to sleep (e.g. Neon's free-tier
// autosuspend, see docs/postgres-backend.md) can take several seconds to
// wake, but a connection that can't be reached at all -- wrong host, network
// down -- would otherwise hang until the OS TCP timeout (minutes).
// connect_timeout caps that at a bounded, clearly reported failure instead.
void configurePostgresConnection(QSqlDatabase &db, const CatalogueDescriptor &descriptor)
{
    db.setHostName(descriptor.pgHost);
    db.setPort(descriptor.pgPort);
    db.setDatabaseName(descriptor.pgDbName);
    db.setUserName(descriptor.pgUser);
    db.setPassword(descriptor.pgPassword);
    db.setConnectOptions(
        QStringLiteral("sslmode=%1;connect_timeout=15").arg(descriptor.pgSslMode));
}

} // namespace

Database::Database()
    : m_connectionName(QStringLiteral("photolife-catalogue-%1").arg(nextConnectionSuffix()))
{
}

Database::~Database()
{
    close();
}

int Database::targetSchemaVersion(CatalogueDescriptor::Backend backend)
{
    const QList<Migration> migrations = discoverMigrations(backend);
    return migrations.isEmpty() ? 0 : migrations.last().version;
}

CatalogueDescriptor::Backend Database::backendFor(const QString &connectionName)
{
    if (!QSqlDatabase::contains(connectionName))
        return CatalogueDescriptor::Backend::Sqlite;
    const QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    return db.driverName() == QStringLiteral("QPSQL") ? CatalogueDescriptor::Backend::Postgres
                                                        : CatalogueDescriptor::Backend::Sqlite;
}

QString Database::nowIsoExpr(CatalogueDescriptor::Backend backend)
{
    return backend == CatalogueDescriptor::Backend::Postgres
               ? QStringLiteral(
                     "to_char(now() at time zone 'utc', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')")
               : QStringLiteral("strftime('%Y-%m-%dT%H:%M:%fZ','now')");
}

bool Database::fail(const QString &context, const QString &detail)
{
    m_error = detail.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, detail);
    return false;
}

bool Database::open(const QString &sqlitePath)
{
    return open(CatalogueDescriptor::sqlite(sqlitePath));
}

bool Database::open(const CatalogueDescriptor &descriptor, const ProgressCallback &onProgress)
{
    close();
    m_error.clear();
    m_descriptor = descriptor;

    if (descriptor.backend == CatalogueDescriptor::Backend::Sqlite) {
        const QString &path = descriptor.sqlitePath;
        if (!path.isEmpty() && path != QStringLiteral(":memory:")) {
            const QFileInfo info(path);
            if (!QDir().mkpath(info.absolutePath()))
                return fail(QStringLiteral("Cannot create %1").arg(info.absolutePath()), {});
        }

        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        db.setDatabaseName(path);
        if (!db.open())
            return fail(QStringLiteral("Cannot open %1").arg(path), db.lastError().text());

        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    } else {
        if (onProgress)
            onProgress(QStringLiteral("Connecting to %1@%2:%3/%4…")
                           .arg(descriptor.pgUser, descriptor.pgHost)
                           .arg(descriptor.pgPort)
                           .arg(descriptor.pgDbName));

        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), m_connectionName);
        configurePostgresConnection(db, descriptor);
        if (!db.open()) {
            return fail(QStringLiteral("Cannot connect to %1@%2:%3/%4")
                             .arg(descriptor.pgUser, descriptor.pgHost)
                             .arg(descriptor.pgPort)
                             .arg(descriptor.pgDbName),
                        db.lastError().text());
        }
    }

    if (!applyPendingMigrations(onProgress)) {
        close();
        return false;
    }

    return true;
}

bool Database::probeReachable(const CatalogueDescriptor &descriptor, QString *errorOut)
{
    if (descriptor.backend != CatalogueDescriptor::Backend::Postgres)
        return true;

    const QString connectionName =
        QStringLiteral("photolife-probe-%1").arg(nextConnectionSuffix());
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connectionName);
        configurePostgresConnection(db, descriptor);
        ok = db.open();
        if (!ok && errorOut)
            *errorOut = db.lastError().text();
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

void Database::close()
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

bool Database::isOpen() const
{
    return QSqlDatabase::contains(m_connectionName)
           && QSqlDatabase::database(m_connectionName, false).isOpen();
}

bool Database::ensureMigrationsTableExists()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QSqlQuery create(db);
    if (!create.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "  version INTEGER PRIMARY KEY,"
            "  applied_at TEXT NOT NULL"
            ")"))) {
        return fail(QStringLiteral("Cannot create schema_migrations"), create.lastError().text());
    }

    // Catalogues created before this table existed tracked their schema
    // version in SQLite's PRAGMA user_version instead. On first open under
    // the new scheme, carry that version forward as already-applied rows
    // rather than re-running DDL for tables that already exist.
    if (m_descriptor.backend == CatalogueDescriptor::Backend::Sqlite) {
        QSqlQuery count(db);
        if (count.exec(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")) && count.next()
            && count.value(0).toInt() == 0) {
            QSqlQuery legacy(db);
            if (legacy.exec(QStringLiteral("PRAGMA user_version")) && legacy.next()) {
                const int legacyVersion = legacy.value(0).toInt();
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                for (int v = 1; v <= legacyVersion; ++v) {
                    QSqlQuery insert(db);
                    insert.prepare(QStringLiteral(
                        "INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)"));
                    insert.addBindValue(v);
                    insert.addBindValue(now);
                    insert.exec();
                }
            }
        }
    }

    return true;
}

int Database::schemaVersion() const
{
    if (!isOpen())
        return -1;
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))
        || !query.next())
        return -1;
    return query.value(0).toInt();
}

bool Database::runScript(const QString &sql)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    for (const QString &statement : splitStatements(sql)) {
        QSqlQuery query(db);
        if (!query.exec(statement)) {
            return fail(QStringLiteral("Statement failed"),
                        QStringLiteral("%1\n%2").arg(query.lastError().text(), statement));
        }
    }
    return true;
}

bool Database::applyPendingMigrations(const ProgressCallback &onProgress)
{
    if (!ensureMigrationsTableExists())
        return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    const int current = schemaVersion();
    if (current < 0)
        return fail(QStringLiteral("Cannot read schema version"), {});

    const QList<Migration> migrations = discoverMigrations(m_descriptor.backend);
    int pendingIndex = 0;
    int pendingCount = 0;
    for (const Migration &migration : migrations) {
        if (migration.version > current)
            ++pendingCount;
    }

    for (const Migration &migration : migrations) {
        if (migration.version <= current)
            continue;
        ++pendingIndex;
        if (onProgress)
            onProgress(QStringLiteral("Applying catalogue update %1 of %2…")
                            .arg(pendingIndex)
                            .arg(pendingCount));

        QFile file(migration.resourcePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return fail(QStringLiteral("Cannot read migration %1").arg(migration.resourcePath),
                        {});
        }
        const QString sql = QString::fromUtf8(file.readAll());

        if (!db.transaction())
            return fail(QStringLiteral("Cannot begin transaction"), db.lastError().text());

        if (!runScript(sql)) {
            db.rollback();
            return false;
        }

        QSqlQuery bump(db);
        bump.prepare(QStringLiteral(
            "INSERT INTO schema_migrations (version, applied_at) VALUES (?, ?)"));
        bump.addBindValue(migration.version);
        bump.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!bump.exec()) {
            db.rollback();
            return fail(QStringLiteral("Cannot record migration %1").arg(migration.version),
                        bump.lastError().text());
        }

        if (!db.commit())
            return fail(QStringLiteral("Cannot commit migration %1").arg(migration.version),
                        db.lastError().text());
    }

    return true;
}

} // namespace pl
