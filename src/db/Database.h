#pragma once

#include <QString>

namespace pl {

// Owns a single SQLite connection to the catalogue and keeps its schema
// up to date by applying numbered migrations from :/migrations/.
class Database
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    // Opens (creating if needed) the SQLite database at `path`, enables
    // foreign keys and WAL, then applies any pending migrations.
    // Returns false on failure; error() then holds a human-readable reason.
    bool open(const QString &path);
    void close();

    bool isOpen() const;

    // Highest migration applied to the open database (0 = brand new).
    int schemaVersion() const;

    QString path() const { return m_path; }
    QString error() const { return m_error; }

    // Connection name for QSqlDatabase::database(connectionName()).
    QString connectionName() const { return m_connectionName; }

    // The latest schema version this build knows how to produce.
    static int targetSchemaVersion();

private:
    bool applyPendingMigrations();
    bool runScript(const QString &sql);
    bool fail(const QString &context, const QString &detail);

    QString m_connectionName;
    QString m_path;
    QString m_error;
};

} // namespace pl
