#pragma once

#include "db/CatalogueDescriptor.h"

#include <QString>

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

    // Opens (creating if needed) the catalogue described by `descriptor`,
    // applies engine-specific setup (SQLite: foreign keys + WAL; Postgres:
    // host/port/credentials), then applies any pending migrations.
    // Returns false on failure; error() then holds a human-readable reason.
    bool open(const CatalogueDescriptor &descriptor);

    // Convenience overload for the common case of a local SQLite file/path
    // (including ":memory:"), used throughout the existing tests.
    bool open(const QString &sqlitePath);

    void close();

    bool isOpen() const;

    // Highest migration applied to the open database (0 = brand new).
    int schemaVersion() const;

    QString path() const { return m_descriptor.sqlitePath; }
    QString error() const { return m_error; }

    // Connection name for QSqlDatabase::database(connectionName()).
    QString connectionName() const { return m_connectionName; }

    // The latest schema version this build knows how to produce, for the
    // given backend (each dialect's migration set must reach the same
    // version numbers -- see tst_database.cpp).
    static int targetSchemaVersion(CatalogueDescriptor::Backend backend = CatalogueDescriptor::Backend::Sqlite);

private:
    bool applyPendingMigrations();
    bool ensureMigrationsTableExists();
    bool runScript(const QString &sql);
    bool fail(const QString &context, const QString &detail);

    QString m_connectionName;
    CatalogueDescriptor m_descriptor;
    QString m_error;
};

} // namespace pl
