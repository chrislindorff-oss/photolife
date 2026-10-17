#pragma once

#include "libsync/LibraryStore.h"
#include "thumb/ThumbnailCache.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace pl::libsync {

struct ThumbnailSyncResult
{
    int uploaded = 0;
    int downloaded = 0;
    QStringList failures;
};

// Keeps grid-sized thumbnails online beside the photos (named
// "<sha256>_<px>", the same content hash the thumbnail cache uses), so a
// computer that leaves some photos online only can still browse them:
// computers that have a photo upload its thumbnail, the others download it
// straight into their thumbnail cache.
//
// A photo no bigger than kOwnThumbnailBytes is about the size its thumbnail
// would be, so it gets none online: a computer without it downloads the photo
// itself and makes the thumbnail from that.
class ThumbnailSync
{
public:
    using Progress = std::function<void(const QString &phase, int done, int total)>;
    using CancelFn = std::function<bool()>;

    static constexpr int kSize = thumb::ThumbnailCache::kGridPx;
    static constexpr qint64 kOwnThumbnailBytes = 64 * 1024;

    // A photo on this computer.
    struct Local
    {
        QString path;     // absolute
        QString sha256;
        qint64 size = 0;
    };
    // A photo only online.
    struct Online
    {
        QString sha256;
        RemoteEntry photo;
    };

    ThumbnailSync(LibraryStore &store, QString cacheDir,
                  thumb::ThumbnailCache::SourceLoader rawLoader = {});

    void setProgress(Progress progress) { m_progress = std::move(progress); }
    void setCancel(CancelFn cancel) { m_cancel = std::move(cancel); }
    QString error() const { return m_error; }

    static QString nameFor(const QString &sha256);

    // Uploads the thumbnails missing online for `here` -- from the cache if
    // it has them, else rendered -- and puts the ones missing from the cache
    // for `onlineOnly` into it. nullopt if the store can't be listed.
    std::optional<ThumbnailSyncResult> run(const QList<Local> &here, const QList<Online> &onlineOnly);

private:
    // The thumbnail for the photo at `path`: from the cache, else rendered
    // (and cached). Empty if none can be made. Safe on worker threads.
    QByteArray thumbnailFor(const QString &path, const QString &sha256) const;
    bool cache(const QString &sha256, const QByteArray &jpeg) const;

    LibraryStore &m_store;
    QString m_cacheDir;
    thumb::ThumbnailCache::SourceLoader m_rawLoader;
    Progress m_progress;
    CancelFn m_cancel;
    QString m_error;
};

} // namespace pl::libsync
