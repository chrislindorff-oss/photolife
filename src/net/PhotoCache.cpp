#include "net/PhotoCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace pl::net {

PhotoCache::PhotoCache(QString cacheDir, QByteArray userAgent, QObject *parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir)), m_userAgent(std::move(userAgent)),
      m_nam(new QNetworkAccessManager(this))
{
    QDir().mkpath(m_cacheDir);
}

PhotoCache::~PhotoCache() = default;

QString PhotoCache::diskPath(const QString &url) const
{
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(m_cacheDir).filePath(hash.left(2) + QLatin1Char('/') + hash + QStringLiteral(".jpg"));
}

QPixmap PhotoCache::photo(const QString &url)
{
    if (url.isEmpty())
        return {};

    if (const auto it = m_memory.constFind(url); it != m_memory.constEnd())
        return *it;

    const QString disk = diskPath(url);
    if (QFileInfo::exists(disk)) {
        QPixmap pm;
        if (pm.load(disk)) {
            m_memory.insert(url, pm);
            return pm;
        }
    }

    if (m_failed.contains(url) || m_pending.contains(url))
        return {};

    m_pending.insert(url);
    m_waiting.enqueue(url);
    pump();
    return {};
}

PhotoCache::DiskUsage PhotoCache::diskUsage(const QStringList &urls) const
{
    DiskUsage usage;
    for (const QString &url : urls) {
        if (url.isEmpty())
            continue;
        const QFileInfo info(diskPath(url));
        if (info.exists()) {
            ++usage.count;
            usage.bytes += info.size();
        }
    }
    return usage;
}

void PhotoCache::pump()
{
    while (m_inFlight < kMaxConcurrent && !m_waiting.isEmpty())
        startDownload(m_waiting.dequeue());
}

void PhotoCache::startDownload(const QString &url)
{
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    ++m_inFlight;
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, url, reply] { onFinished(url, reply); });
}

void PhotoCache::onFinished(const QString &url, QNetworkReply *reply)
{
    reply->deleteLater();
    --m_inFlight;
    m_pending.remove(url);

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QImage image;
    if (reply->error() == QNetworkReply::NoError && (status == 0 || (status >= 200 && status < 300)))
        image.loadFromData(reply->readAll());

    if (image.isNull()) {
        m_failed.insert(url);
    } else {
        if (image.width() > kMaxEdge || image.height() > kMaxEdge)
            image = image.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QString disk = diskPath(url);
        QDir().mkpath(QFileInfo(disk).absolutePath());
        image.save(disk, "JPG", 85);
        m_memory.insert(url, QPixmap::fromImage(image));
    }

    emit ready(url);
    pump();
}

} // namespace pl::net
