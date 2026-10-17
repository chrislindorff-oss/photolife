#include "libsync/ThumbnailSync.h"

#include "libsync/Parallel.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QSet>

#include <mutex>

namespace pl::libsync {

ThumbnailSync::ThumbnailSync(LibraryStore &store, QString cacheDir,
                             thumb::ThumbnailCache::SourceLoader rawLoader)
    : m_store(store), m_cacheDir(std::move(cacheDir))
{
    // RAW decoding isn't known to be safe on several threads at once.
    if (rawLoader) {
        auto mutex = std::make_shared<std::mutex>();
        m_rawLoader = [rawLoader, mutex](const QString &path, int edge) {
            std::lock_guard lock(*mutex);
            return rawLoader(path, edge);
        };
    }
}

QString ThumbnailSync::nameFor(const QString &sha256)
{
    return sha256 + QLatin1Char('_') + QString::number(kSize);
}

bool ThumbnailSync::cache(const QString &sha256, const QByteArray &jpeg) const
{
    const QString target = thumb::ThumbnailCache::diskPathFor(m_cacheDir, sha256, kSize);
    QDir().mkpath(QFileInfo(target).path());
    QSaveFile save(target);
    return save.open(QIODevice::WriteOnly) && save.write(jpeg) == jpeg.size() && save.commit();
}

QByteArray ThumbnailSync::thumbnailFor(const QString &path, const QString &sha256) const
{
    QFile cached(thumb::ThumbnailCache::diskPathFor(m_cacheDir, sha256, kSize));
    if (cached.open(QIODevice::ReadOnly))
        return cached.readAll();
    const QImage image = thumb::ThumbnailCache::render(path, kSize, m_rawLoader);
    if (image.isNull())
        return {};
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 85);
    cache(sha256, jpeg);   // this computer would make the same one to show it
    return jpeg;
}

std::optional<ThumbnailSyncResult> ThumbnailSync::run(const QList<Local> &here,
                                                      const QList<Online> &onlineOnly)
{
    const auto online = m_store.listThumbnails();
    if (!online) {
        m_error = m_store.error();
        return std::nullopt;
    }

    ThumbnailSyncResult result;
    const std::function<bool()> stop = [this] { return m_cancel && m_cancel(); };

    QList<Local> toUpload;
    QSet<QString> queued;
    for (const Local &item : here) {
        if (!item.sha256.isEmpty() && item.size > kOwnThumbnailBytes
            && !online->contains(nameFor(item.sha256)) && !queued.contains(item.sha256)) {
            toUpload << item;
            queued.insert(item.sha256);
        }
    }
    if (!toUpload.isEmpty() && !m_store.prepareThumbnails()) {
        result.failures << QStringLiteral("thumbnails: %1").arg(m_store.error());
        toUpload.clear();
    }
    int finished = 0;
    forEachTransfer<QString>(
        m_store, int(toUpload.size()), stop,
        [&](LibraryStore &store, int i) -> QString {
            const Local &item = toUpload.at(i);
            const QByteArray jpeg = thumbnailFor(item.path, item.sha256);
            if (jpeg.isEmpty())
                return QStringLiteral("%1: no thumbnail could be made").arg(item.path);
            if (!store.putThumbnail(nameFor(item.sha256), jpeg))
                return QStringLiteral("%1: %2").arg(item.path, store.error());
            return {};
        },
        [&](int, QString &&failure) {
            if (m_progress)
                m_progress(QStringLiteral("Uploading thumbnails"), ++finished, int(toUpload.size()));
            if (failure.isEmpty())
                ++result.uploaded;
            else
                result.failures << failure;
        });

    // Theirs: the thumbnail if it's online, else -- for a small photo -- the
    // photo itself.
    QList<Online> toFetch;
    queued.clear();
    for (const Online &item : onlineOnly) {
        if (item.sha256.isEmpty() || queued.contains(item.sha256)
            || QFileInfo::exists(thumb::ThumbnailCache::diskPathFor(m_cacheDir, item.sha256, kSize)))
            continue;
        const bool hasThumb = online->contains(nameFor(item.sha256));
        const bool small = !item.photo.relPath.isEmpty() && item.photo.size > 0
                           && item.photo.size <= kOwnThumbnailBytes;
        if (hasThumb || small) {
            toFetch << item;
            queued.insert(item.sha256);
        }
    }
    finished = 0;
    forEachTransfer<QString>(
        m_store, int(toFetch.size()), stop,
        [&](LibraryStore &store, int i) -> QString {
            const Online &item = toFetch.at(i);
            const QString label = QStringLiteral("thumbnail %1").arg(item.sha256.left(12));
            if (online->contains(nameFor(item.sha256))) {
                const auto jpeg = store.getThumbnail(nameFor(item.sha256));
                if (!jpeg)
                    return QStringLiteral("%1: %2").arg(label, store.error());
                return cache(item.sha256, *jpeg) ? QString()
                                                 : QStringLiteral("%1: couldn't save it").arg(label);
            }
            // Keep the suffix: rendering goes by it.
            const QString temp = QDir(m_cacheDir).filePath(
                QStringLiteral(".photo-%1.%2").arg(item.sha256, QFileInfo(item.photo.relPath).suffix()));
            QDir().mkpath(m_cacheDir);
            if (!store.download(item.photo, temp)) {
                QFile::remove(temp);
                return QStringLiteral("%1: %2").arg(label, store.error());
            }
            const QByteArray jpeg = thumbnailFor(temp, item.sha256);
            QFile::remove(temp);
            return jpeg.isEmpty() ? QStringLiteral("%1: no thumbnail could be made").arg(label) : QString();
        },
        [&](int, QString &&failure) {
            if (m_progress)
                m_progress(QStringLiteral("Downloading thumbnails"), ++finished, int(toFetch.size()));
            if (failure.isEmpty())
                ++result.downloaded;
            else
                result.failures << failure;
        });
    return result;
}

} // namespace pl::libsync
