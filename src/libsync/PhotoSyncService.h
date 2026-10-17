#pragma once

#include "libsync/LibrarySync.h"
#include "libsync/DriveConfig.h"
#include "libsync/S3Config.h"
#include "libsync/ThumbnailSync.h"

#include <QDateTime>
#include <QFuture>
#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

namespace pl::libsync {

// Runs LibrarySync in the background for a Server catalogue whose photos
// PhotoLife keeps online itself. Separate from sync::SyncService: photo
// syncs never touch the catalogue server, need no edit lock, and can run
// alongside it.
class PhotoSyncService : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        Check,      // what a sync would do; transfers nothing
        Upload,     // send new and changed photos (before sending the catalogue)
        Download,   // fetch new and changed photos
        Both,
        FreeUp,     // move the copies here of photos not wanted any more (safely online) to the trash folder
        Fetch,      // download particular photos now (see fetch())
        RemoveOnline,   // take particular photos deleted here out of the store (see removeOnline())
    };

    // What to do about photos deleted on this computer (Settings).
    static constexpr const char *kDeletedHereAsk = "ask";
    static constexpr const char *kDeletedHereRemove = "remove";
    static constexpr const char *kDeletedHereKeep = "keep";

    struct Config
    {
        QString store;          // Settings::kPhotoStore*; "none" or empty: off
        S3Config s3;
        DriveConfig drive;
        QString libraryRoot;
        QString statePath;      // LibraryState file
        QString who;
        QString databasePath;   // the catalogue, read for the tree selection
        QString thumbnailDir;   // the thumbnail cache's folder
        // Keep every photo on this computer, or only those of these trees
        // (see PhotoSelection; unmatched and pinned photos are always kept).
        bool allTrees = true;
        QList<int> trees;
        // Photos deleted here: "ask" (the UI offers them after a sync),
        // "remove" (uploads take them out online too), or "keep" online.
        QString whenDeletedHere = QString::fromLatin1(kDeletedHereAsk);

        bool enabled() const
        {
            return !store.isEmpty() && store != QLatin1String("none") && !libraryRoot.isEmpty();
        }
    };

    struct Outcome
    {
        Mode mode = Mode::Check;
        bool ok = false;
        QString error;
        std::optional<LibraryPlan> plan;          // Check
        std::optional<LibrarySyncResult> result;  // the others
        std::optional<ThumbnailSyncResult> thumbnails;
        QStringList fetched;                      // Fetch: the absolute paths now here
        // Photos known online that aren't on this computer (absolute paths).
        QStringList onlineOnly;
        qint64 durationMs = 0;
        QDateTime finishedAt;
    };

    explicit PhotoSyncService(QObject *parent = nullptr);
    // Cancels a running sync and waits for it to stop (at most a moment: the
    // transfer under way is aborted).
    ~PhotoSyncService() override;

    void setConfig(const Config &config) { m_config = config; }
    const Config &config() const { return m_config; }
    bool isRunning() const { return m_running; }

    // Starts `mode` in the background; finished() reports it. False while
    // another one is running, or when photo sync is off.
    bool run(Mode mode);
    // Downloads these photos (absolute paths) now, and keeps them here.
    bool fetch(const QStringList &paths);
    // Takes these photos (relative paths), deleted here, out of the store.
    bool removeOnline(const QStringList &relPaths);
    // Remembers that these photos (relative paths), deleted here, stay
    // online. Not while a sync runs.
    bool keepOnline(const QStringList &relPaths);
    // Stops the running sync after the file it's on.
    void cancel() { m_cancel.store(true); }

    // The LibraryState file kept beside a catalogue.
    static QString statePathFor(const QString &databasePath);

    // The photos known online that aren't on this computer (absolute paths),
    // from the last listing -- without asking the store. Any thread.
    static QStringList onlineOnlyPaths(const Config &config);

    // A store for `config`, or nullptr (with `error`) if it can't be made.
    static std::unique_ptr<LibraryStore> makeStore(const Config &config, QString *error);

signals:
    void progress(const QString &phase, int done, int total);
    void finished(const pl::libsync::PhotoSyncService::Outcome &outcome);

private:
    Outcome runInWorker(Mode mode, const Config &config, const QStringList &fetchPaths);

    Config m_config;
    bool m_running = false;
    std::atomic_bool m_cancel{false};
    QFuture<Outcome> m_future;
    QStringList m_fetchPaths;
};

} // namespace pl::libsync

Q_DECLARE_METATYPE(pl::libsync::PhotoSyncService::Outcome)
