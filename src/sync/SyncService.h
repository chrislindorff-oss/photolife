#pragma once

#include "db/CatalogueDescriptor.h"
#include "sync/CatalogueSync.h"
#include "sync/ChangeSummary.h"

#include <QDateTime>

#include <QObject>
#include <QString>

#include <optional>

namespace pl::sync {

// Runs catalogue sync operations (see CatalogueSync) for the app, one at a
// time, on a worker thread with its own connections to the local catalogue
// and the server -- the server can be slow to reach, and nothing here may
// stall the window.
//
// A pull never touches the open catalogue: it builds "<catalogue>.pulled.db"
// (plus its snapshot) beside it, and applyPendingPull() swaps it in at the
// next start, before the catalogue is opened -- the app restarts to finish it.
class SyncService : public QObject
{
    Q_OBJECT

public:
    enum class Op {
        Check,            // server state + how many local changes are unsent
        Pull,             // get the server's catalogue (restart to use it)
        StartEditing,     // take the edit lock; pull first if the server is newer
        BreakLock,        // StartEditing, taking the lock from whoever has it
        Push,             // send changes, keep the lock
        FinishEditing,    // send changes, release the lock
        Publish,          // replace the server's catalogue with this one
        Changes,          // describe the unsent local changes (no server needed)
        TestConnection,   // time a connection and report the server's details
        Discard,          // back to the last synced copy (restart to finish)
        DiscardAndRelease,// ...and give the edit lock back
        RestoreBackup,    // stage restoreSource (restart to finish); see restoreBackup()
        PublishFromFile,  // publish a copy of another catalogue file; see publishFromFile()
    };

    // What TestConnection measured.
    struct ConnectionTest
    {
        qint64 connectMs = -1;     // opening the connection
        double roundTripMs = -1;   // average of three SELECT 1
        QString serverVersion;     // Postgres's own version string
    };

    struct Outcome
    {
        Op op = Op::Check;
        bool ok = false;
        QString error;
        std::optional<ServerState> state;
        qint64 localVersion = 0;
        int pending = -1;        // unsent local changes (-1: unknown)
        int sent = 0;            // rows a push/publish sent
        int outsideRoot = 0;     // photo paths outside the library root
        bool pulled = false;     // a newer catalogue is waiting: restart to use it
        bool unreachable = false;// the server couldn't be reached at all
        std::optional<ChangeSummary> changes;   // Changes (and Check)
        std::optional<ConnectionTest> test;     // TestConnection
        qint64 durationMs = 0;
        QDateTime finishedAt;
    };

    struct Config
    {
        QString databasePath;              // the local catalogue file
        CatalogueDescriptor server;
        SyncIdentity identity;
        QString libraryRoot;
    };

    explicit SyncService(QObject *parent = nullptr);

    void setConfig(const Config &config) { m_config = config; }
    const Config &config() const { return m_config; }
    bool isRunning() const { return m_running; }

    // Starts `op` in the background; finished() reports it. Ignored (false)
    // while another operation is running.
    bool run(Op op);
    // RestoreBackup of the catalogue file at `path`.
    bool restoreBackup(const QString &path);
    // Starts (or replaces) the server's catalogue from a COPY of the catalogue
    // file at `path` (e.g. the Local catalogue): the copy is published and
    // becomes this server's working copy at the next start. `path` itself is
    // only read.
    bool publishFromFile(const QString &path);

    // This catalogue's automatic before-download backups, newest first.
    QStringList backups() const;

    // Unsent local changes, counted on the calling thread against the local
    // catalogue connection `connectionName` (no server needed). -1 on error.
    int pendingChanges(const QString &connectionName) const;

    static QString basePath(const QString &databasePath);
    static QString pulledPath(const QString &databasePath);
    // Swaps a completed pull into place. Call before opening the catalogue.
    // The catalogue it replaces is kept (with its -wal/-shm files) as
    // "<catalogue>.before-sync-<yyyyMMdd-HHmmss>-<nn>.db"; the newest
    // kBackupsKept of those are kept. True if it swapped.
    static bool applyPendingPull(const QString &databasePath);
    static constexpr int kBackupsKept = 3;

signals:
    void progress(const QString &phase, int done, int total);
    void finished(const pl::sync::SyncService::Outcome &outcome);

private:
    Outcome runInWorker(Op op, const Config &config);
    Outcome runLocal(Op op, const Config &config);

    QString m_restoreSource;

    Config m_config;
    bool m_running = false;
};

} // namespace pl::sync

Q_DECLARE_METATYPE(pl::sync::SyncService::Outcome)
