#pragma once

#include "db/CatalogueDescriptor.h"

#include <QString>

#include <functional>

namespace pl {

// Owns a single connection to the catalogue -- a local SQLite file, or a
// shared Postgres database -- and keeps its schema up to date by applying
// numbered migrations from :/migrations/<dialect>/.
class Database
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    // Reports a human-readable step ("Connecting…", "Applying migration 2 of
    // 5…") as open() works through connecting and migrating, so a caller can
    // keep a "please wait" UI honest instead of it looking frozen.
    using ProgressCallback = std::function<void(const QString &)>;

    // Opens (creating if needed) the catalogue described by `descriptor`,
    // applies engine-specific setup (SQLite: foreign keys + WAL; Postgres:
    // host/port/credentials), then applies any pending migrations.
    // Returns false on failure; error() then holds a human-readable reason.
    bool open(const CatalogueDescriptor &descriptor, const ProgressCallback &onProgress = {});

    // Convenience overload for the common case of a local SQLite file/path
    // (including ":memory:"), used throughout the existing tests.
    bool open(const QString &sqlitePath);

    void close();

    bool isOpen() const;

    // Attempts a throwaway connection to `descriptor` (Postgres only --
    // always true for Sqlite) and immediately closes it, without touching
    // this Database's own connection or applying migrations. Lets a caller
    // check reachability -- e.g. from a background thread, while a "connecting"
    // UI stays responsive on the GUI thread -- before committing to the real
    // open(), which the rest of the app then does on its own thread as usual.
    static bool probeReachable(const CatalogueDescriptor &descriptor, QString *errorOut = nullptr);

    // Highest migration applied to the open database (0 = brand new).
    int schemaVersion() const;

    QString path() const { return m_descriptor.sqlitePath; }
    QString error() const { return m_error; }

    // The descriptor this connection was opened with, so a caller can open an
    // independent connection to the same catalogue elsewhere (e.g. a
    // background worker thread).
    CatalogueDescriptor descriptor() const { return m_descriptor; }

    // Connection name for QSqlDatabase::database(connectionName()).
    QString connectionName() const { return m_connectionName; }

    // The latest schema version this build knows how to produce, for the
    // given backend (each dialect's migration set must reach the same
    // version numbers -- see tst_database.cpp).
    static int targetSchemaVersion(CatalogueDescriptor::Backend backend = CatalogueDescriptor::Backend::Sqlite);

    // Which backend an already-open connection (by name) is talking to.
    // Store classes only ever see a connectionName, not the descriptor that
    // opened it, so this is how they pick a dialect-specific SQL fragment
    // (see nowIsoExpr()) without a constructor signature change.
    static CatalogueDescriptor::Backend backendFor(const QString &connectionName);

    // A SQL expression producing the current UTC time as the ISO-8601-with-
    // milliseconds string the rest of the app parses (Qt::ISODateWithMs),
    // in the given backend's dialect. SQLite: strftime(...). Postgres:
    // to_char(now() ...).
    static QString nowIsoExpr(CatalogueDescriptor::Backend backend);

private:
    bool applyPendingMigrations(const ProgressCallback &onProgress);
    bool ensureMigrationsTableExists();
    bool runScript(const QString &sql);
    bool fail(const QString &context, const QString &detail);

    QString m_connectionName;
    CatalogueDescriptor m_descriptor;
    QString m_error;
};

} // namespace pl
