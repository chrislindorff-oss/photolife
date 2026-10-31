#include "net/TileCache.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

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
    QString url = m_urlTemplate;
    url.replace(QLatin1String("{z}"), QString::number(key.z))
        .replace(QLatin1String("{x}"), QString::number(key.x))
        .replace(QLatin1String("{y}"), QString::number(key.y));
    return QUrl(url);
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

    if (m_pending.contains(key))
        return {};
    if (const auto it = m_failed.constFind(key); it != m_failed.constEnd()) {
        if (it->permanent || QDateTime::currentDateTimeUtc() < it->retryAt)
            return {};
        // Due another try (the failure count is kept, for the next wait).
    }

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
    // Told to slow down: nothing new until then.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_pausedUntil.isValid() && now < m_pausedUntil) {
        if (!m_pumpScheduled) {
            m_pumpScheduled = true;
            QTimer::singleShot(int(now.msecsTo(m_pausedUntil)) + 50, this, [this] {
                m_pumpScheduled = false;
                pump();
            });
        }
        return;
    }
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
    // A download that hangs would otherwise hold its slot for good, and with
    // them all held no more tiles ever load.
    req.setTransferTimeout(m_timeoutMs);

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
        Failure &f = m_failed[key];
        ++f.count;
        // A 4xx other than "too many requests" / "timeout" won't change.
        f.permanent = status >= 400 && status < 500 && status != 408 && status != 429;
        int waitMs = std::min(m_retryBaseMs * (1 << std::min(f.count - 1, 6)), 5 * 60 * 1000);
        if (status == 429 || status == 503) {
            // The server's busy: hold everything off as long as it asks.
            bool ok = false;
            const int retryAfter = reply->rawHeader("Retry-After").trimmed().toInt(&ok);
            const int pauseMs = ok && retryAfter > 0 ? std::min(retryAfter, 300) * 1000 : 30000;
            waitMs = std::max(waitMs, pauseMs);
            m_pausedUntil = QDateTime::currentDateTimeUtc().addMSecs(pauseMs);
        }
        f.retryAt = QDateTime::currentDateTimeUtc().addMSecs(waitMs);
        if (!f.permanent) {
            // Due again: a view still showing it asks for it when it repaints.
            QTimer::singleShot(waitMs + 50, this, [this, key] { emit ready(key.z, key.x, key.y); });
        }
    } else {
        m_failed.remove(key);
        const QString disk = diskPath(key);
        QDir().mkpath(QFileInfo(disk).absolutePath());
        image.save(disk, "PNG");
        m_memory.insert(key, QPixmap::fromImage(image));
    }

    emit ready(key.z, key.x, key.y);
    pump();
}

} // namespace pl::net
