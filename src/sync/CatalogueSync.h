#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace pl::sync {

// Translates the catalogue's absolute photo paths (folder.path,
// rendition.path, and a folder-scoped name_alias's scope) between this
// machine and the shared server, so
// collaborators can keep the photo library at different places -- or on
// different operating systems. On the server a path under the library root
// is stored as "lib:/<relative path, forward slashes>"; a path outside it is
// stored unchanged (and counted, so a push can say so).
class PathMapping
{
public:
    explicit PathMapping(const QString &libraryRoot = {});

    QString libraryRoot() const { return m_root; }
    QString toServer(const QString &localPath, bool *outsideRoot = nullptr) const;
    QString toLocal(const QString &serverPath) const;

    static constexpr const char *kPrefix = "lib:/";

private:
    QString m_root;   // forward slashes, no trailing slash; empty = no mapping
};

// Who is syncing: shown to collaborators while holding the edit lock.
struct SyncIdentity
{
    QString user;
    QString machine;
};

// The server's catalogue_sync row, plus what's needed to decide what to do.
struct ServerState
{
    int schemaVersion = 0;
    qint64 version = 0;        // bumped by every push
    QString lockedBy;          // empty: nobody is editing
    QString lockedMachine;
    QString lockedAt;          // ISO-8601 UTC
    QString pushedBy;          // who made the latest push, and when
    QString pushedAt;
    bool empty = true;         // no taxa and no photos yet
    int trees = 0;             // reference trees
    int photos = 0;            // captures

    bool isLocked() const { return !lockedBy.isEmpty(); }
    bool heldBy(const SyncIdentity &who) const
    {
        return lockedBy == who.user && lockedMachine == who.machine;
    }
};

// Keeps a local SQLite catalogue and a shared server catalogue (Postgres, or
// SQLite in tests) in step, for "Shared -- sync with server" mode: everyone
// works on a local copy; one person at a time holds the server's edit lock
// and pushes their changes. There is never anything to merge.
//
//  - pull():    copies the whole server catalogue into a fresh local one.
//  - push():    sends the rows that changed since `basePath` (a snapshot of
//               the catalogue as last pulled/pushed), in one transaction,
//               then refreshes the snapshot. Needs the lock, and the server
//               still at the version last pulled. It works from a frozen
//               copy of the catalogue taken first, so edits made while it
//               runs are simply left for the next push.
//  - publish(): replaces the server's contents with the local catalogue
//               (first-time setup).
//
// Both catalogues must be at the same schema version. Bulk statements are
// sent as literal SQL, a few hundred KB at a time: one round trip each,
// where a prepared statement costs three on Postgres -- it matters on a
// remote server.
//
// Synchronous and thread-agnostic: give it connection names opened on the
// calling thread (see QSqlDatabase's threading rules).
class CatalogueSync
{
public:
    using Progress = std::function<void(const QString &phase, int done, int total)>;

    CatalogueSync(QString localConnection, QString serverConnection, PathMapping paths);

    void setProgress(Progress progress) { m_progress = std::move(progress); }
    QString error() const { return m_error; }

    std::optional<ServerState> serverState();

    // Takes the edit lock (or confirms we have it). With `breakExisting`,
    // takes it from whoever holds it. False when someone else holds it --
    // serverState() says who.
    bool acquireLock(const SyncIdentity &who, bool breakExisting = false);
    bool releaseLock(const SyncIdentity &who);

    // The server version the local catalogue was last pulled or pushed at.
    qint64 localVersion() const;

    // Replaces the local catalogue's contents with the server's. The local
    // catalogue should be a fresh one (see the class comment); returns the
    // server version pulled, or nullopt.
    std::optional<qint64> pull();

    struct PushResult
    {
        int changedRows = 0;     // rows inserted, updated or deleted on the server
        int outsideRoot = 0;     // photo paths outside the library root
        qint64 newVersion = 0;
    };
    std::optional<PushResult> push(const QString &basePath, const SyncIdentity &who);

    // How many rows differ from the snapshot at `basePath` -- what a push
    // would send.
    std::optional<int> pendingChanges(const QString &basePath);

    std::optional<PushResult> publish(const QString &basePath, const SyncIdentity &who);

    // Writes a consistent copy of the local catalogue to `path` (replacing
    // it), for use as the next push's base.
    bool snapshotTo(const QString &path);

private:
    struct Table
    {
        QString name;
        QStringList columns;
        QStringList pk;
        QStringList parents;       // tables it references
        QList<int> pathColumns;    // indexes into columns
    };

    bool loadTables();
    bool serverIsPostgres() const;
    QString literal(const QVariant &value) const;
    QString nowExpr() const;
    bool execServer(const QString &sql);
    bool sendRows(const Table &table, const QList<QVariantList> &rows, bool upsert,
                  int *outsideRoot);
    bool deleteRows(const Table &table, const QList<QVariantList> &keys);
    bool resetIdentities(const QList<QString> &tables);
    bool bumpVersion(qint64 expected, const SyncIdentity &who, bool requireLock);
    bool setLocalVersion(qint64 version);
    // Records `version` in the snapshot at `path` too: Discard copies the
    // snapshot back over the catalogue, version and all.
    bool setSnapshotVersion(const QString &path, qint64 version);
    struct Diff
    {
        QList<QVariantList> upserts;
        QList<QVariantList> deletes;   // primary-key values
    };
    // Rows of schema `newer` that differ from schema `older` (both attached
    // to, or the main schema of, the local connection).
    std::optional<QList<std::pair<const Table *, Diff>>> diff(const QString &newer,
                                                             const QString &older);
    bool attach(const QString &path, const QString &alias);
    void detach(const QString &alias);
    bool fail(const QString &message);
    void report(const QString &phase, int done, int total) const;

    QString m_local;
    QString m_server;
    PathMapping m_paths;
    Progress m_progress;
    QString m_error;
    QList<Table> m_tables;   // parents before children
};

} // namespace pl::sync
