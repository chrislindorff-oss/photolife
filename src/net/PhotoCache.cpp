#include "net/PhotoCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QDateTime>
#include <QSaveFile>
#include <QTimer>

namespace pl::net {

PhotoCache::PhotoCache(QString cacheDir, QByteArray userAgent, QObject *parent)
    : QObject(parent), m_cacheDir(std::move(cacheDir)), m_userAgent(std::move(userAgent)),
      m_nam(new QNetworkAccessManager(this))
{
    QDir().mkpath(m_cacheDir);
    // A stalled connection ends as a failure (and a retry), not a photo
    // that never comes.
    m_nam->setTransferTimeout(30000);
}

PhotoCache::~PhotoCache() = default;

QString PhotoCache::diskPath(const QString &url) const
{
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(m_cacheDir).filePath(hash.left(2) + QLatin1Char('/') + hash + QStringLiteral(".jpg"));
}

QPixmap PhotoCache::photo(const QString &url, Urgency urgency)
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

    if (m_pending.contains(url)) {
        if (urgency == Urgency::Now && m_waiting.removeOne(url)) {
            m_waiting.prepend(url);
            pump();
        }
        return {};
    }
    if (const auto it = m_failed.constFind(url); it != m_failed.constEnd()) {
        if (it->permanent || QDateTime::currentMSecsSinceEpoch() - it->at < kFailureMemoryMs)
            return {};
        m_failed.erase(it);   // long enough ago: worth another go
    }

    m_pending.insert(url);
    m_attempts.insert(url, 0);
    enqueue(url, urgency);
    return {};
}

void PhotoCache::retry(const QString &url)
{
    if (url.isEmpty())
        return;
    m_failed.remove(url);
    photo(url, Urgency::Now);
}

void PhotoCache::enqueue(const QString &url, Urgency urgency)
{
    if (urgency == Urgency::Now)
        m_waiting.prepend(url);
    else
        m_waiting.enqueue(url);
    pump();
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

QStringList PhotoCache::largerVariants(const QString &url)
{
    // .../photos/12345/medium.jpg[?123] -> original / large / as given
    static const QRegularExpression sizeRe(
        QStringLiteral(R"(/(square|thumb|small|medium|large|original)(\.[A-Za-z]+)(\?.*)?$)"));
    const auto m = sizeRe.match(url);
    if (!m.hasMatch())
        return {url};
    QStringList out;
    for (const char *size : {"original", "large"}) {
        QString v = url;
        v.replace(m.capturedStart(1), m.capturedLength(1), QLatin1String(size));
        out << v;
    }
    out << url;
    out.removeDuplicates();   // keeps the first: biggest first, whatever `url` was
    return out;
}

QString PhotoCache::largePath(const QString &url) const
{
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(m_cacheDir).filePath(QStringLiteral("large/%1.jpg").arg(hash));
}

void PhotoCache::fetchLarge(const QString &url, LargeDone done)
{
    if (url.isEmpty()) {
        done({}, tr("there's no reference photo"));
        return;
    }
    const QString path = largePath(url);
    if (QFileInfo(path).size() > 0) {
        done(path, {});
        return;
    }
    fetchVariant(url, largerVariants(url), std::move(done));
}

// Tries each variant in turn until one downloads as an image; saves its bytes
// unchanged (no re-encoding: it's for looking at closely).
void PhotoCache::fetchVariant(const QString &url, QStringList variants, LargeDone done)
{
    if (variants.isEmpty()) {
        done({}, tr("iNaturalist didn't send a larger copy of this photo"));
        return;
    }
    const QString next = variants.takeFirst();
    QNetworkRequest req{QUrl(next)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, variants, done = std::move(done)]() mutable {
                reply->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray bytes = reply->error() == QNetworkReply::NoError
                                                 && (status == 0 || (status >= 200 && status < 300))
                                             ? reply->readAll()
                                             : QByteArray();
                if (bytes.isEmpty() || QImage::fromData(bytes).isNull()) {
                    fetchVariant(url, std::move(variants), std::move(done));
                    return;
                }
                const QString path = largePath(url);
                QDir().mkpath(QFileInfo(path).absolutePath());
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                    done({}, tr("couldn't save it to %1").arg(QFileInfo(path).absolutePath()));
                    return;
                }
                done(path, {});
            });
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

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool answered = reply->error() == QNetworkReply::NoError && (status == 0 || (status >= 200 && status < 300));
    QImage image;
    if (answered)
        image.loadFromData(reply->readAll());

    // Not there (a 4xx), or not an image: asking again won't change it.
    const bool permanent = (answered && image.isNull())
                           || (status >= 400 && status < 500 && status != 408 && status != 429);
    if (image.isNull() && !permanent) {
        // A dropped connection, a timeout or a busy server: try again shortly.
        const int tried = ++m_attempts[url];
        if (tried < kMaxAttempts) {
            QTimer::singleShot(m_retryBaseMs * (1 << (tried - 1)), this, [this, url] {
                // At the front: whoever asked has been waiting already.
                if (m_pending.contains(url))
                    enqueue(url, Urgency::Now);
            });
            pump();
            return;
        }
    }
    m_pending.remove(url);
    m_attempts.remove(url);

    if (image.isNull()) {
        m_failed.insert(url, {QDateTime::currentMSecsSinceEpoch(), permanent});
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
