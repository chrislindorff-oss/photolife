#include "libsync/PhotoSyncService.h"

#include "libsync/GoogleDriveStore.h"
#include "libsync/LibraryState.h"
#include "libsync/PhotoSelection.h"
#include "libsync/S3Store.h"
#include "raw/RawPreview.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QUuid>
#include <QtConcurrent>

namespace pl::libsync {

namespace {

QString cleanRoot(const QString &root)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(root));
}

// The catalogue, read-only, on the calling thread; closed when it goes.
class CatalogueReader
{
public:
    explicit CatalogueReader(const QString &path)
        : m_name(QStringLiteral("photosync-catalogue-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        m_open = !path.isEmpty() && db.open();
    }
    ~CatalogueReader()
    {
        QSqlDatabase::database(m_name, false).close();
        QSqlDatabase::removeDatabase(m_name);
    }
    bool isOpen() const { return m_open; }
    QString name() const { return m_name; }

private:
    QString m_name;
    bool m_open = false;
};

QStringList onlineOnlyFrom(LibraryState &state, const QString &root)
{
    QStringList out;
    const auto remote = state.remote();
    for (auto it = remote.cbegin(); it != remote.cend(); ++it) {
        const QString path = root + QLatin1Char('/') + it.key();
        if (LibrarySync::isSyncable(it.key()) && !QFileInfo::exists(path))
            out << path;
    }
    return out;
}

} // namespace

PhotoSyncService::PhotoSyncService(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<pl::libsync::PhotoSyncService::Outcome>();
}

PhotoSyncService::~PhotoSyncService()
{
    m_cancel.store(true);
    if (m_future.isRunning())
        m_future.waitForFinished();
}

QString PhotoSyncService::statePathFor(const QString &databasePath)
{
    return QFileInfo(databasePath).absoluteDir().filePath(QStringLiteral("library_state.db"));
}

std::unique_ptr<LibraryStore> PhotoSyncService::makeStore(const Config &config, QString *error)
{
    if (config.store == QLatin1String("s3")) {
        if (!config.s3.isComplete()) {
            *error = tr("The photo storage settings are incomplete — see Server Settings.");
            return nullptr;
        }
        return std::make_unique<S3Store>(config.s3);
    }
    if (config.store == QLatin1String("gdrive")) {
        if (!config.drive.isComplete()) {
            *error = tr("Sign in with Google and choose the shared Drive folder in Server Settings.");
            return nullptr;
        }
        return std::make_unique<GoogleDriveStore>(config.drive);
    }
    *error = tr("Photo sync is off for this catalogue.");
    return nullptr;
}

QStringList PhotoSyncService::onlineOnlyPaths(const Config &config)
{
    if (!config.enabled() || !QFileInfo::exists(config.statePath))
        return {};
    LibraryState state(config.statePath);
    if (!state.open())
        return {};
    return onlineOnlyFrom(state, cleanRoot(config.libraryRoot));
}

bool PhotoSyncService::run(Mode mode)
{
    if (m_running || !m_config.enabled())
        return false;
    m_running = true;
    m_cancel.store(false);
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        m_running = false;
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        emit finished(outcome);
    });
    const Config config = m_config;
    const QStringList fetchPaths = std::exchange(m_fetchPaths, {});
    m_future = QtConcurrent::run([this, mode, config, fetchPaths] {
        QElapsedTimer timer;
        timer.start();
        Outcome out = runInWorker(mode, config, fetchPaths);
        out.durationMs = timer.elapsed();
        out.finishedAt = QDateTime::currentDateTime();
        return out;
    });
    watcher->setFuture(m_future);
    return true;
}

bool PhotoSyncService::fetch(const QStringList &paths)
{
    if (m_running || paths.isEmpty())
        return false;
    m_fetchPaths = paths;
    if (!run(Mode::Fetch)) {
        m_fetchPaths.clear();
        return false;
    }
    return true;
}

PhotoSyncService::Outcome PhotoSyncService::runInWorker(Mode mode, const Config &config,
                                                        const QStringList &fetchPaths)
{
    Outcome out;
    out.mode = mode;
    const QString root = cleanRoot(config.libraryRoot);

    if (!QDir().mkpath(root)) {
        out.error = tr("Couldn't open the photo library folder %1.").arg(QDir::toNativeSeparators(root));
        return out;
    }
    const auto store = makeStore(config, &out.error);
    if (!store)
        return out;
    LibraryState state(config.statePath);
    if (!state.open()) {
        out.error = tr("Couldn't open the photo sync record: %1").arg(state.error());
        return out;
    }

    auto cancel = [this] { return m_cancel.load(); };
    auto report = [this](const QString &phase, int done, int total) {
        QMetaObject::invokeMethod(
            this, [this, phase, done, total] { emit progress(phase, done, total); },
            Qt::QueuedConnection);
    };

    LibrarySync sync(*store, state, root);
    sync.setCancel(cancel);
    sync.setProgress(report);

    if (mode == Mode::Fetch) {
        QStringList rels;
        for (const QString &path : fetchPaths) {
            const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
            if (clean.startsWith(root + QLatin1Char('/')))
                rels << clean.mid(root.size() + 1);
        }
        out.result = sync.fetch(rels);
        out.ok = true;
        for (const QString &rel : std::as_const(rels)) {
            if (QFileInfo::exists(root + QLatin1Char('/') + rel))
                out.fetched << root + QLatin1Char('/') + rel;
        }
        out.onlineOnly = onlineOnlyFrom(state, root);
        return out;
    }

    // What this computer keeps: everything, or the ticked trees' photos.
    CatalogueReader catalogue(config.databasePath);
    LibrarySyncOptions options;
    options.who = config.who;
    options.upload = mode == Mode::Upload || mode == Mode::Both || mode == Mode::Check;
    options.download = mode == Mode::Download || mode == Mode::Both || mode == Mode::Check;
    options.freeUp = mode == Mode::FreeUp;
    if (mode == Mode::FreeUp) {
        options.upload = false;
        options.download = false;
    }
    if (!config.allTrees) {
        if (!catalogue.isOpen()) {
            out.error = tr("Couldn't read the catalogue to see which photos to keep.");
            return out;
        }
        QString error;
        const auto notWanted = PhotoSelection::notWanted(catalogue.name(), config.trees, &error);
        if (!notWanted) {
            out.error = tr("Couldn't work out which photos to keep: %1").arg(error);
            return out;
        }
        QSet<QString> skip;
        for (const QString &path : *notWanted) {
            const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
            if (clean.startsWith(root + QLatin1Char('/')))
                skip.insert(clean.mid(root.size() + 1));
        }
        const QSet<QString> pinned = state.pinned();
        options.wanted = [skip, pinned](const QString &rel) {
            return !skip.contains(rel) || pinned.contains(rel);
        };
    } else if (mode == Mode::FreeUp) {
        out.error = tr("Every photo is kept on this computer, so there's nothing to free up.");
        return out;
    }

    if (mode == Mode::Check) {
        out.plan = sync.plan(options);
        out.ok = out.plan.has_value();
    } else {
        out.result = sync.run(options);
        out.ok = out.result.has_value();
        if (out.result)
            out.plan = out.result->plan;
    }
    if (!out.ok) {
        out.error = sync.error();
        return out;
    }
    out.onlineOnly = onlineOnlyFrom(state, root);

    // Thumbnails: ours up for the others; theirs down for what's online only.
    const bool thumbsUp = mode == Mode::Upload || mode == Mode::Both;
    const bool thumbsDown = (mode == Mode::Download || mode == Mode::Both) && !out.onlineOnly.isEmpty();
    if ((thumbsUp || thumbsDown) && !config.thumbnailDir.isEmpty() && !(out.result && out.result->cancelled)) {
        QList<ThumbnailSync::Local> here;
        if (thumbsUp) {
            const auto base = state.load();
            for (auto it = base.cbegin(); it != base.cend(); ++it) {
                if (it->local && !it->sha256.isEmpty())
                    here.append({root + QLatin1Char('/') + it.key(), it->sha256, it->size});
            }
        }
        QList<ThumbnailSync::Online> onlineHashes;
        if (thumbsDown) {
            const QHash<QString, QString> hashes =
                catalogue.isOpen() ? PhotoSelection::contentHashes(catalogue.name())
                                   : QHash<QString, QString>();
            const auto remote = state.remote();
            for (const QString &path : std::as_const(out.onlineOnly)) {
                const RemoteEntry photo = remote.value(path.mid(root.size() + 1));
                QString sha = photo.sha256;
                if (sha.isEmpty())
                    sha = hashes.value(path);   // the catalogue keeps forward slashes too
                if (!sha.isEmpty())
                    onlineHashes.append({sha, photo});
            }
        }
        thumb::ThumbnailCache::SourceLoader rawLoader;
        if (raw::isAvailable())
            rawLoader = [](const QString &path, int edge) { return raw::extractPreview(path, edge); };
        ThumbnailSync thumbs(*store, config.thumbnailDir, rawLoader);
        thumbs.setCancel(cancel);
        thumbs.setProgress(report);
        out.thumbnails = thumbs.run(here, onlineHashes);   // best effort
    }
    return out;
}

} // namespace pl::libsync
