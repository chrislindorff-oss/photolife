#include "net/TileCache.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace pl::net {

size_t qHash(const TileCache::Key &k, size_t seed)
{
    return qHashMulti(seed, k.z, k.x, k.y);
}

TileCache::TileCache(QString cacheDir, QByteArray userAgent, QObject *parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir)), m_userAgent(std::move(userAgent)),
      m_nam(new QNetworkAccessManager(this))
{
    QDir().mkpath(m_cacheDir);
}

TileCache::~TileCache() = default;

QString TileCache::diskPath(Key key) const
{
    return QDir(m_cacheDir)
        .filePath(QStringLiteral("%1/%2/%3.png").arg(key.z).arg(key.x).arg(key.y));
}

QUrl TileCache::tileUrl(Key key) const
{
    return QUrl(QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
                    .arg(key.z)
                    .arg(key.x)
                    .arg(key.y));
}

QPixmap TileCache::tile(int z, int x, int y)
{
    const Key key{z, x, y};

    if (const auto it = m_memory.constFind(key); it != m_memory.constEnd())
        return *it;

    const QString disk = diskPath(key);
    if (QFileInfo::exists(disk)) {
        QPixmap pm;
        if (pm.load(disk)) {
            m_memory.insert(key, pm);
            return pm;
        }
    }

    m_neededThisFrame.insert(key);

    if (m_failed.contains(key) || m_pending.contains(key))
        return {};

    m_pending.insert(key);
    m_waiting.enqueue(key);
    pump();
    return {};
}

void TileCache::beginFrame()
{
    m_neededThisFrame.clear();
}

void TileCache::endFrame()
{
    QQueue<Key> kept;
    while (!m_waiting.isEmpty()) {
        const Key key = m_waiting.dequeue();
        if (m_neededThisFrame.contains(key))
            kept.enqueue(key);
        else
            m_pending.remove(key);
    }
    m_waiting = std::move(kept);
}

void TileCache::pump()
{
    while (m_inFlight < kMaxConcurrent && !m_waiting.isEmpty())
        startDownload(m_waiting.dequeue());
}

void TileCache::startDownload(Key key)
{
    QNetworkRequest req{tileUrl(key)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    ++m_inFlight;
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, key, reply] { onFinished(key, reply); });
}

void TileCache::onFinished(Key key, QNetworkReply *reply)
{
    reply->deleteLater();
    --m_inFlight;
    m_pending.remove(key);

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QImage image;
    if (reply->error() == QNetworkReply::NoError && (status == 0 || (status >= 200 && status < 300)))
        image.loadFromData(reply->readAll());

    if (image.isNull()) {
        m_failed.insert(key);
    } else {
        const QString disk = diskPath(key);
        QDir().mkpath(QFileInfo(disk).absolutePath());
        image.save(disk, "PNG");
        m_memory.insert(key, QPixmap::fromImage(image));
    }

    emit ready(key.z, key.x, key.y);
    pump();
}

} // namespace pl::net
