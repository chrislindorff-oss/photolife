#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace pl::net {

// A two-level cache for remote thumbnail images (iNaturalist taxon reference
// photos): an in-memory QPixmap cache over an on-disk JPEG cache, both keyed by
// a hash of the source URL. Downloads run through a private
// QNetworkAccessManager, a few at a time, off the API's rate-limited HttpClient.
//
// Ask with photo(); connect ready() to refresh when a miss has been filled.
// Lives on and is used from the GUI thread.
class PhotoCache : public QObject
{
    Q_OBJECT

public:
    // Downloaded images are scaled so their longest edge is at most this.
    static constexpr int kMaxEdge = 320;

    PhotoCache(QString cacheDir, QByteArray userAgent, QObject *parent = nullptr);
    ~PhotoCache() override;

    // The pixmap for `url` if it is already in memory or on disk; otherwise a
    // null pixmap and a scheduled download that emits ready(url) on completion.
    // A url that has already failed to download stays null and is not retried.
    QPixmap photo(const QString &url);

    // Where the on-disk cache lives, for reporting total storage use.
    QString cacheDir() const { return m_cacheDir; }

    struct DiskUsage
    {
        int count = 0;       // how many of `urls` have a file on disk
        qint64 bytes = 0;    // their combined size
    };
    // How much of `urls` (e.g. a reference tree's taxon photo URLs) is already
    // downloaded, and how large that is. Urls not yet fetched are simply not
    // counted, rather than triggering a download.
    DiskUsage diskUsage(const QStringList &urls) const;

signals:
    void ready(const QString &url);

private:
    void pump();
    void startDownload(const QString &url);
    void onFinished(const QString &url, QNetworkReply *reply);
    QString diskPath(const QString &url) const;

    QString m_cacheDir;
    QByteArray m_userAgent;
    QNetworkAccessManager *m_nam;

    QHash<QString, QPixmap> m_memory;   // url -> pixmap
    QSet<QString> m_pending;            // url in flight or queued
    QSet<QString> m_failed;             // url that errored; don't retry
    QQueue<QString> m_waiting;          // url queued behind the concurrency limit
    int m_inFlight = 0;

    static constexpr int kMaxConcurrent = 4;
};

} // namespace pl::net
