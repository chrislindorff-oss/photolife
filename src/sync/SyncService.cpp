#include "sync/SyncService.h"

#include "db/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtConcurrent/QtConcurrentRun>

namespace pl::sync {
namespace {

void removeDatabaseFiles(const QString &path)
{
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(path + suffix);
}

} // namespace

SyncService::SyncService(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<pl::sync::SyncService::Outcome>();
}

QString SyncService::basePath(const QString &databasePath)
{
    QString p = databasePath;
    if (p.endsWith(QLatin1String(".db")))
        p.chop(3);
    return p + QStringLiteral(".base.db");
}

QString SyncService::pulledPath(const QString &databasePath)
{
    QString p = databasePath;
    if (p.endsWith(QLatin1String(".db")))
        p.chop(3);
    return p + QStringLiteral(".pulled.db");
}

bool SyncService::applyPendingPull(const QString &databasePath)
{
    const QString pulled = pulledPath(databasePath);
    const QString pulledBase = basePath(pulled);
    // Both halves, or nothing: an interrupted pull leaves no base.
    if (!QFile::exists(pulled) || !QFile::exists(pulledBase))
        return false;

    // Never lose the catalogue being replaced: keep it, WAL files and all
    // (they can hold committed changes not yet written into the main file).
    const QFileInfo info(databasePath);
    const QString stem = info.completeBaseName();
    if (info.exists()) {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        // Always numbered, so name order is age order (for pruning below).
        QString backup;
        for (int n = 1; backup.isEmpty() || QFile::exists(backup); ++n)
            backup = info.dir().filePath(QStringLiteral("%1.before-sync-%2-%3.db")
                                             .arg(stem, stamp)
                                             .arg(n, 2, 10, QLatin1Char('0')));
        for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            if (QFile::exists(databasePath + suffix) && !QFile::rename(databasePath + suffix, backup + suffix))
                return false;   // keep what we have rather than risk it
        }
        // Only the newest few.
        QStringList backups = info.dir().entryList(
            {stem + QStringLiteral(".before-sync-*.db")}, QDir::Files, QDir::Name | QDir::Reversed);
        for (qsizetype i = kBackupsKept; i < backups.size(); ++i)
            removeDatabaseFiles(info.dir().filePath(backups.at(i)));
    }
    if (!QFile::rename(pulled, databasePath))
        return false;
    QFile::remove(basePath(databasePath));
    QFile::rename(pulledBase, basePath(databasePath));
    removeDatabaseFiles(pulled);
    return true;
}

int SyncService::pendingChanges(const QString &connectionName) const
{
    CatalogueSync sync(connectionName, QString(), PathMapping(m_config.libraryRoot));
    const auto n = sync.pendingChanges(basePath(m_config.databasePath));
    return n ? *n : -1;
}

bool SyncService::run(Op op)
{
    if (m_running)
        return false;
    m_running = true;
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        m_running = false;
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        emit finished(outcome);
    });
    const Config config = m_config;
    watcher->setFuture(QtConcurrent::run([this, op, config] {
        QElapsedTimer timer;
        timer.start();
        Outcome out = runInWorker(op, config);
        out.durationMs = timer.elapsed();
        out.finishedAt = QDateTime::currentDateTime();
        return out;
    }));
    return true;
}

bool SyncService::restoreBackup(const QString &path)
{
    if (m_running)
        return false;
    m_restoreSource = path;
    return run(Op::RestoreBackup);
}

bool SyncService::publishFromFile(const QString &path)
{
    if (m_running)
        return false;
    m_restoreSource = path;
    return run(Op::PublishFromFile);
}

QStringList SyncService::backups() const
{
    // Everything kept beside the catalogue under its name: automatic
    // before-download backups, and any copies set aside by hand.
    const QFileInfo info(m_config.databasePath);
    const QString stem = info.completeBaseName();
    const QSet<QString> working = {basePath(m_config.databasePath), pulledPath(m_config.databasePath),
                                   basePath(pulledPath(m_config.databasePath))};
    QStringList out;
    const QFileInfoList files = info.dir().entryInfoList({stem + QStringLiteral(".*.db")},
                                                         QDir::Files, QDir::Time);
    for (const QFileInfo &f : files) {
        if (!working.contains(f.absoluteFilePath()) && !f.fileName().endsWith(QLatin1String(".tmp")))
            out << f.absoluteFilePath();
    }
    return out;
}

// Operations that only need this computer's catalogue.
SyncService::Outcome SyncService::runLocal(Op op, const Config &config)
{
    Outcome out;
    out.op = op;
    const QString base = basePath(config.databasePath);
    const QString pulled = pulledPath(config.databasePath);
    auto stage = [&](const QString &catalogue, const QString &newBase) -> bool {
        removeDatabaseFiles(pulled);
        QFile::remove(basePath(pulled));
        for (const QString &suffix : {QString(), QStringLiteral("-wal")}) {
            if (QFile::exists(catalogue + suffix) && !QFile::copy(catalogue + suffix, pulled + suffix)) {
                out.error = tr("Could not copy %1.").arg(catalogue + suffix);
                return false;
            }
        }
        return true;
    };

    switch (op) {
    case Op::Changes: {
        Database local;
        if (!local.open(config.databasePath)) {
            out.error = local.error();
            return out;
        }
        QString error;
        out.changes = summarizeChanges(local.connectionName(), base, &error);
        if (!out.changes) {
            out.error = error;
            return out;
        }
        out.pending = out.changes->totalRows;
        out.ok = true;
        return out;
    }
    case Op::Discard:
    case Op::DiscardAndRelease:
        // The last synced copy becomes the catalogue at the next start.
        if (!QFile::exists(base)) {
            out.error = tr("There's no record of the last sync to go back to.");
            return out;
        }
        if (!stage(base, base) || !QFile::copy(base, basePath(pulled))) {
            removeDatabaseFiles(pulled);
            return out;
        }
        out.pulled = true;
        out.ok = true;
        return out;
    case Op::RestoreBackup: {
        const QString source = m_restoreSource;
        if (!QFile::exists(source)) {
            out.error = tr("%1 doesn't exist.").arg(source);
            return out;
        }
        qint64 version = 0;
        {
            Database local;
            if (local.open(config.databasePath)) {
                QSqlQuery q(QSqlDatabase::database(local.connectionName(), false));
                if (q.exec(QStringLiteral("SELECT version FROM catalogue_sync WHERE id = 1")) && q.next())
                    version = q.value(0).toLongLong();
            }
        }
        if (!stage(source, QString()))
            return out;
        {
            // Bring it to this version's schema, and keep this computer's sync
            // position: whatever the backup differs by then shows up as
            // changes to review, never as something already on the server.
            Database restored;
            if (!restored.open(pulled)) {
                out.error = tr("The backup couldn't be opened: %1").arg(restored.error());
                removeDatabaseFiles(pulled);
                return out;
            }
            QSqlQuery q(QSqlDatabase::database(restored.connectionName(), false));
            q.prepare(QStringLiteral("UPDATE catalogue_sync SET version = ? WHERE id = 1"));
            q.addBindValue(version);
            q.exec();
        }
        const QString newBase = QFile::exists(base) ? base : pulled;
        if (!QFile::copy(newBase, basePath(pulled))) {
            out.error = tr("Could not stage the restored catalogue.");
            removeDatabaseFiles(pulled);
            return out;
        }
        out.pulled = true;
        out.ok = true;
        return out;
    }
    default:
        break;
    }
    return out;
}

// On the worker thread: every connection used here is opened (and closed)
// here, as QSqlDatabase requires.
SyncService::Outcome SyncService::runInWorker(Op op, const Config &config)
{
    if (op == Op::Changes || op == Op::Discard || op == Op::RestoreBackup)
        return runLocal(op, config);
    Outcome out;
    out.op = op;
    auto report = [this](const QString &phase, int done, int total) {
        QMetaObject::invokeMethod(this, [this, phase, done, total] {
            emit progress(phase, done, total);
        }, Qt::QueuedConnection);
    };

    report(tr("Connecting to the shared catalogue…"), 0, 0);
    Database local;
    if (!local.open(config.databasePath)) {
        out.error = tr("Could not open this computer's catalogue: %1").arg(local.error());
        return out;
    }
    QElapsedTimer connectTimer;
    connectTimer.start();
    Database server;
    if (!server.open(config.server)) {
        out.error = tr("Could not reach the shared catalogue: %1").arg(server.error());
        out.unreachable = true;
        return out;
    }
    const qint64 connectMs = connectTimer.elapsed();
    const PathMapping paths(config.libraryRoot);
    // After a push or publish the server matches this catalogue: a download
    // still waiting to be swapped in is older, and must not replace it.
    auto discardPendingPull = [&] {
        const QString pulled = pulledPath(config.databasePath);
        removeDatabaseFiles(pulled);
        QFile::remove(basePath(pulled));
    };
    CatalogueSync sync(local.connectionName(), server.connectionName(), paths);
    sync.setProgress(report);
    out.localVersion = sync.localVersion();
    const QString base = basePath(config.databasePath);

    // Builds the server's catalogue beside ours, for applyPendingPull().
    auto pullBeside = [&]() -> bool {
        const QString pulled = pulledPath(config.databasePath);
        removeDatabaseFiles(pulled);
        QFile::remove(basePath(pulled));
        Database target;
        if (!target.open(pulled)) {
            out.error = tr("Could not create the downloaded catalogue: %1").arg(target.error());
            return false;
        }
        CatalogueSync into(target.connectionName(), server.connectionName(), paths);
        into.setProgress(report);
        const auto version = into.pull();
        if (!version || !into.snapshotTo(basePath(pulled))) {
            out.error = tr("Could not get the shared catalogue: %1").arg(into.error());
            target.close();
            removeDatabaseFiles(pulled);
            QFile::remove(basePath(pulled));
            return false;
        }
        out.pulled = true;
        return true;
    };

    switch (op) {
    case Op::Check: {
        out.state = sync.serverState();
        if (!out.state) {
            out.error = sync.error();
            return out;
        }
        if (QFile::exists(base)) {
            QString error;
            out.changes = summarizeChanges(local.connectionName(), base, &error);
            out.pending = out.changes ? out.changes->totalRows : -1;
        }
        out.ok = true;
        return out;
    }
    case Op::TestConnection: {
        ConnectionTest test;
        test.connectMs = connectMs;
        QSqlQuery q(QSqlDatabase::database(server.connectionName(), false));
        QElapsedTimer rt;
        rt.start();
        int trips = 0;
        for (int i = 0; i < 3; ++i) {
            if (q.exec(QStringLiteral("SELECT 1")) && q.next())
                ++trips;
        }
        if (trips > 0)
            test.roundTripMs = double(rt.elapsed()) / trips;
        const bool pg = Database::backendFor(server.connectionName())
                        == CatalogueDescriptor::Backend::Postgres;
        if (q.exec(pg ? QStringLiteral("SELECT version()") : QStringLiteral("SELECT sqlite_version()"))
            && q.next())
            test.serverVersion = q.value(0).toString();
        out.test = test;
        out.state = sync.serverState();
        if (!out.state) {
            out.error = sync.error();
            return out;
        }
        out.ok = true;
        return out;
    }
    case Op::DiscardAndRelease: {
        Outcome staged = runLocal(Op::Discard, config);
        staged.op = op;
        if (!staged.ok)
            return staged;
        if (!sync.releaseLock(config.identity)) {
            staged.ok = false;
            staged.error = tr("Your changes will be discarded, but the edit lock couldn't be "
                              "released: %1")
                               .arg(sync.error());
        }
        staged.state = sync.serverState();
        return staged;
    }
    case Op::Pull:
        out.ok = pullBeside();
        out.state = sync.serverState();
        return out;
    case Op::StartEditing:
    case Op::BreakLock: {
        // The lock first, so nobody can push between our pull and our edits.
        if (!sync.acquireLock(config.identity, op == Op::BreakLock)) {
            out.state = sync.serverState();
            out.error = sync.error();
            return out;
        }
        out.state = sync.serverState();
        if (!out.state) {
            out.error = sync.error();
            return out;
        }
        if (out.state->version != out.localVersion || !QFile::exists(base)) {
            if (!pullBeside())
                return out;
        }
        out.ok = true;
        return out;
    }
    case Op::Push:
    case Op::FinishEditing: {
        const auto pushed = sync.push(base, config.identity);
        if (!pushed) {
            out.state = sync.serverState();
            out.error = sync.error();
            return out;
        }
        out.sent = pushed->changedRows;
        out.outsideRoot = pushed->outsideRoot;
        out.localVersion = pushed->newVersion;
        discardPendingPull();
        if (op == Op::FinishEditing && !sync.releaseLock(config.identity)) {
            out.error = sync.error();
            return out;
        }
        out.state = sync.serverState();
        out.pending = 0;
        out.ok = true;
        return out;
    }
    case Op::PublishFromFile: {
        // Copy the source (read-only to it), then publish the copy from its
        // own connection and stage it -- with its snapshot -- as this
        // server's working copy for the next start.
        const QString source = m_restoreSource;
        const QString pulled = pulledPath(config.databasePath);
        removeDatabaseFiles(pulled);
        QFile::remove(basePath(pulled));
        {
            Database reader;
            if (!reader.open(source)) {
                out.error = tr("Could not open %1: %2").arg(source, reader.error());
                return out;
            }
            QSqlQuery q(QSqlDatabase::database(reader.connectionName(), false));
            QString escaped = pulled;
            escaped.replace(QLatin1Char('\''), QLatin1String("''"));
            if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped))) {
                out.error = tr("Could not copy %1.").arg(source);
                return out;
            }
        }
        Database copy;
        if (!copy.open(pulled)) {
            out.error = copy.error();
            return out;
        }
        CatalogueSync from(copy.connectionName(), server.connectionName(), paths);
        from.setProgress(report);
        const auto published = from.publish(basePath(pulled), config.identity);
        if (!published) {
            out.error = from.error();
            out.state = from.serverState();
            copy.close();
            removeDatabaseFiles(pulled);
            QFile::remove(basePath(pulled));
            return out;
        }
        out.sent = published->changedRows;
        out.outsideRoot = published->outsideRoot;
        out.localVersion = published->newVersion;
        out.state = from.serverState();
        out.pulled = true;
        out.ok = true;
        return out;
    }
    case Op::Publish: {
        const auto published = sync.publish(base, config.identity);
        if (!published) {
            out.state = sync.serverState();
            out.error = sync.error();
            return out;
        }
        out.sent = published->changedRows;
        out.outsideRoot = published->outsideRoot;
        out.localVersion = published->newVersion;
        discardPendingPull();
        out.state = sync.serverState();
        out.pending = 0;
        out.ok = true;
        return out;
    }
    }
    return out;
}

} // namespace pl::sync
