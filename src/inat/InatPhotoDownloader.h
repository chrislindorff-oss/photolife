#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

namespace pl::inat {

// Downloads one full-resolution iNaturalist photo's raw bytes and saves them
// verbatim to a destination path -- no decode/rescale/re-encode, unlike
// net::PhotoCache (which is for small cached preview thumbnails only), so the
// file ExifWriter later operates on is genuinely the original. Uses its own
// QNetworkAccessManager since photo URLs are served from iNat's media CDN
// (Amazon S3), not the rate-limited JSON API host the shared HttpClient paces.
//
// Long runs of S3 downloads hit the occasional transient failure even on a
// good connection -- a DNS timeout, a reset connection, S3's "503 Slow Down"
// -- so each photo is retried with exponential backoff (honouring any
// Retry-After) before being reported as failed. A transfer that goes quiet
// for longer than the transfer timeout is aborted and retried too, rather
// than hanging forever. Permanent failures (404, 403, a missing local file)
// aren't retried.
//
// No internal queue or concurrency cap: it's meant to be driven one call at a
// time by InatImportService (download, write EXIF, then the next), which is
// itself enough pacing to be a good citizen of the CDN without adding a
// second throttling layer here.
class InatPhotoDownloader : public QObject
{
    Q_OBJECT

public:
    explicit InatPhotoDownloader(QByteArray userAgent, QObject *parent = nullptr);

    // Attempts per photo (>= 1) and the first retry's delay; each further
    // retry doubles it. Defaults: 4 attempts, 1 s (so 1 s, 2 s, 4 s).
    void setRetryPolicy(int maxAttempts, int baseDelayMs);
    // Abort a transfer after this long with no data (default 60 s).
    void setTransferTimeoutMs(int ms) { m_transferTimeoutMs = ms; }

    void download(const QUrl &url, const QString &destPath,
                 std::function<void(bool ok, const QString &error)> done);

    // Whether a failed attempt is worth retrying: network-level failures
    // other than a missing/forbidden resource, and HTTP 408, 429 and 5xx.
    static bool isTransient(QNetworkReply::NetworkError error, int httpStatus);

private:
    void attempt(const QUrl &url, const QString &destPath, int attemptNo,
                 std::function<void(bool ok, const QString &error)> done);

    QNetworkAccessManager *m_nam;
    QByteArray m_userAgent;
    int m_maxAttempts = 4;
    int m_retryBaseMs = 1000;
    int m_transferTimeoutMs = 60000;
};

} // namespace pl::inat
