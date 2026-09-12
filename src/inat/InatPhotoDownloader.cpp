#include "inat/InatPhotoDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace pl::inat {

InatPhotoDownloader::InatPhotoDownloader(QByteArray userAgent, QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_userAgent(std::move(userAgent))
{
}

void InatPhotoDownloader::download(const QUrl &url, const QString &destPath,
                                   std::function<void(bool, const QString &)> done)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, destPath, done] {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = status == 0 || (status >= 200 && status < 300);
        if (reply->error() != QNetworkReply::NoError || !httpOk) {
            const QString error = reply->errorString().isEmpty()
                                      ? QStringLiteral("HTTP %1").arg(status)
                                      : reply->errorString();
            done(false, error);
            return;
        }

        const QByteArray bytes = reply->readAll();
        if (!QDir().mkpath(QFileInfo(destPath).absolutePath())) {
            done(false, QStringLiteral("could not create folder for %1").arg(destPath));
            return;
        }
        QFile file(destPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            done(false, QStringLiteral("could not write %1").arg(destPath));
            return;
        }
        done(true, QString());
    });
}

} // namespace pl::inat
