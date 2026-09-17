#pragma once

#include <QString>

namespace pl {

// Identifies which catalogue a Database should open: a local SQLite file, or
// a shared Postgres database multiple collaborators point at. Threaded
// through Application/ScanService/MatchService in place of the bare
// database-path string used before the Postgres backend existed.
struct CatalogueDescriptor
{
    enum class Backend { Sqlite, Postgres };

    Backend backend = Backend::Sqlite;

    // Backend::Sqlite: path to the catalogue file, or ":memory:" in tests.
    QString sqlitePath;

    // Backend::Postgres connection parameters.
    QString pgHost;
    int pgPort = 5432;
    QString pgDbName;
    QString pgUser;
    QString pgPassword;
    QString pgSslMode = QStringLiteral("prefer");

    static CatalogueDescriptor sqlite(const QString &path)
    {
        CatalogueDescriptor descriptor;
        descriptor.backend = Backend::Sqlite;
        descriptor.sqlitePath = path;
        return descriptor;
    }
};

} // namespace pl
