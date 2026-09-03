#include "thumb/ThumbnailCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QtConcurrent>

namespace pl::thumb {
namespace {

QString memKey(const QString &contentHash, int px)
{
    return QString::number(px) + QLatin1Char(':') + contentHash;
}

// Key we can still use when a rendition has no content hash yet: the source path.
QString effectiveKey(const QString &contentHash, const QString &sourcePath)
{
    if (!contentHash.isEmpty())
        return contentHash;
    return QString::fromLatin1(
        QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QImage scaleToLongestEdge(QImage image, int longestEdge)
{
    if (image.isNull())
        return image;
    if (image.width() <= longestEdge && image.height() <= longestEdge)
        return image;
    return image.scaled(longestEdge, longestEdge, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

} // namespace

ThumbnailCache::ThumbnailCache(QString cacheDir, QObject *parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir))
{
    QDir().mkpath(m_cacheDir);
}

ThumbnailCache::~ThumbnailCache() = default;

void ThumbnailCache::setRawLoader(SourceLoader loader)
{
    m_rawLoader = std::move(loader);
}

QString ThumbnailCache::diskPath(const QString &contentHash, int longestEdge) const
{
    const QString shard = contentHash.left(2);
    return QDir(m_cacheDir)
        .filePath(QString::number(longestEdge) + QLatin1Char('/') + shard + QLatin1Char('/')
                  + contentHash + QStringLiteral(".jpg"));
}

bool ThumbnailCache::isCached(const QString &contentHash, int longestEdge) const
{
    if (contentHash.isEmpty())
        return false;
    if (m_memory.contains(memKey(contentHash, longestEdge)))
        return true;
    return QFileInfo::exists(diskPath(contentHash, longestEdge));
}

QPixmap ThumbnailCache::thumbnail(const QString &contentHash, const QString &sourcePath,
                                  int longestEdge)
{
    const QString key = effectiveKey(contentHash, sourcePath);
    const QString mkey = memKey(key, longestEdge);

    if (const auto it = m_memory.constFind(mkey); it != m_memory.constEnd())
        return *it;

    const QString disk = diskPath(key, longestEdge);
    if (QFileInfo::exists(disk)) {
        QPixmap pm;
        if (pm.load(disk)) {
            m_memory.insert(mkey, pm);
            return pm;
        }
    }

    schedule(key, sourcePath, longestEdge);
    return {};
}

void ThumbnailCache::schedule(const QString &key, const QString &sourcePath, int longestEdge)
{
    const QString mkey = memKey(key, longestEdge);
    if (m_pending.contains(mkey))
        return;
    m_pending.insert(mkey);

    SourceLoader rawLoader = m_rawLoader;
    auto *watcherCtx = this;

    (void)QtConcurrent::run([watcherCtx, key, mkey, sourcePath, longestEdge, rawLoader] {
        QImage image;

        QImageReader reader(sourcePath);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            const QSize full = reader.size();
            if (full.isValid() && (full.width() > longestEdge || full.height() > longestEdge)) {
                QSize target = full;
                target.scale(longestEdge, longestEdge, Qt::KeepAspectRatio);
                reader.setScaledSize(target);
            }
            image = reader.read();
        }
        if (image.isNull() && rawLoader)
            image = rawLoader(sourcePath, longestEdge);

        image = scaleToLongestEdge(std::move(image), longestEdge);

        QMetaObject::invokeMethod(
            watcherCtx,
            [watcherCtx, mkey, key, longestEdge, image] {
                watcherCtx->onGenerated(mkey, key, longestEdge, image);
            },
            Qt::QueuedConnection);
    });
}

void ThumbnailCache::onGenerated(const QString &mkey, const QString &contentHash, int longestEdge,
                                 const QImage &image)
{
    m_pending.remove(mkey);

    if (!image.isNull()) {
        const QString disk = diskPath(contentHash, longestEdge);
        QDir().mkpath(QFileInfo(disk).absolutePath());
        image.save(disk, "JPG", 85);
        m_memory.insert(mkey, QPixmap::fromImage(image));
    }

    emit ready(contentHash, longestEdge);
}

} // namespace pl::thumb
