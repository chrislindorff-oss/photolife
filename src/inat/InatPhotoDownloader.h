#pragma once

#include <QByteArray>
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
// QNetworkAccessManager since photo URLs are served from iNat's media CDN,
// not the rate-limited JSON API host the shared HttpClient paces.
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

    void download(const QUrl &url, const QString &destPath,
                 std::function<void(bool ok, const QString &error)> done);

private:
    QNetworkAccessManager *m_nam;
    QByteArray m_userAgent;
};

} // namespace pl::inat
