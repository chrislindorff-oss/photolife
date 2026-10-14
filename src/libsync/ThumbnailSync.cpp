#include "libsync/ThumbnailSync.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>

namespace pl::libsync {

ThumbnailSync::ThumbnailSync(LibraryStore &store, QString cacheDir,
                             thumb::ThumbnailCache::SourceLoader rawLoader)
    : m_store(store), m_cacheDir(std::move(cacheDir)), m_rawLoader(std::move(rawLoader))
{
}

QString ThumbnailSync::nameFor(const QString &sha256)
{
    return sha256 + QLatin1Char('_') + QString::number(kSize);
}

std::optional<ThumbnailSyncResult> ThumbnailSync::run(const QList<QPair<QString, QString>> &here,
                                                      const QStringList &onlineOnly)
{
    const auto online = m_store.listThumbnails();
    if (!online) {
        m_error = m_store.error();
        return std::nullopt;
    }

    ThumbnailSyncResult result;
    auto stop = [this] { return m_cancel && m_cancel(); };

    QList<QPair<QString, QString>> toUpload;
    for (const auto &item : here) {
        if (!item.second.isEmpty() && !online->contains(nameFor(item.second)))
            toUpload << item;
    }
    for (int i = 0; i < toUpload.size() && !stop(); ++i) {
        if (m_progress)
            m_progress(QStringLiteral("Uploading thumbnails"), i, int(toUpload.size()));
        const auto &[path, sha] = toUpload.at(i);
        QByteArray jpeg;
        QFile cached(thumb::ThumbnailCache::diskPathFor(m_cacheDir, sha, kSize));
        if (cached.open(QIODevice::ReadOnly)) {
            jpeg = cached.readAll();
        } else {
            const QImage image = thumb::ThumbnailCache::render(path, kSize, m_rawLoader);
            if (image.isNull()) {
                result.failures << QStringLiteral("%1: no thumbnail could be made").arg(path);
                continue;
            }
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "JPG", 85);
            // Keep it: this computer would make the same one to show it.
            QDir().mkpath(QFileInfo(cached.fileName()).path());
            QSaveFile save(cached.fileName());
            if (save.open(QIODevice::WriteOnly)) {
                save.write(jpeg);
                save.commit();
            }
        }
        if (m_store.putThumbnail(nameFor(sha), jpeg))
            ++result.uploaded;
        else
            result.failures << QStringLiteral("%1: %2").arg(path, m_store.error());
    }

    QStringList toDownload;
    for (const QString &sha : onlineOnly) {
        if (!sha.isEmpty() && online->contains(nameFor(sha))
            && !QFileInfo::exists(thumb::ThumbnailCache::diskPathFor(m_cacheDir, sha, kSize)))
            toDownload << sha;
    }
    for (int i = 0; i < toDownload.size() && !stop(); ++i) {
        if (m_progress)
            m_progress(QStringLiteral("Downloading thumbnails"), i, int(toDownload.size()));
        const QString &sha = toDownload.at(i);
        const auto jpeg = m_store.getThumbnail(nameFor(sha));
        if (!jpeg) {
            result.failures << QStringLiteral("thumbnail %1: %2").arg(sha.left(12), m_store.error());
            continue;
        }
        const QString target = thumb::ThumbnailCache::diskPathFor(m_cacheDir, sha, kSize);
        QDir().mkpath(QFileInfo(target).path());
        QSaveFile save(target);
        if (save.open(QIODevice::WriteOnly) && save.write(*jpeg) == jpeg->size() && save.commit())
            ++result.downloaded;
        else
            result.failures << QStringLiteral("thumbnail %1: couldn't save it").arg(sha.left(12));
    }
    return result;
}

} // namespace pl::libsync
