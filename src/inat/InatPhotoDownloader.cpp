#include "inat/InatPhotoDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

namespace pl::inat {

InatPhotoDownloader::InatPhotoDownloader(QByteArray userAgent, QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_userAgent(std::move(userAgent))
{
}

void InatPhotoDownloader::setRetryPolicy(int maxAttempts, int baseDelayMs)
{
    m_maxAttempts = std::max(1, maxAttempts);
    m_retryBaseMs = std::max(0, baseDelayMs);
}

bool InatPhotoDownloader::isTransient(QNetworkReply::NetworkError error, int httpStatus)
{
    if (httpStatus > 0)
        return httpStatus == 408 || httpStatus == 429 || httpStatus >= 500;
    switch (error) {
    case QNetworkReply::ContentNotFoundError:
    case QNetworkReply::ContentAccessDenied:
    case QNetworkReply::ContentOperationNotPermittedError:
    case QNetworkReply::ProtocolUnknownError:
    case QNetworkReply::ProtocolInvalidOperationError:
        return false;
    default:
        // Connection refused/reset, host not found (a DNS hiccup), timeouts
        // (OperationCanceledError, from the transfer timeout), TLS handshake
        // failures, temporary network failures...
        return true;
    }
}

void InatPhotoDownloader::download(const QUrl &url, const QString &destPath,
                                   std::function<void(bool, const QString &)> done)
{
    attempt(url, destPath, 1, std::move(done));
}

void InatPhotoDownloader::attempt(const QUrl &url, const QString &destPath, int attemptNo,
                                  std::function<void(bool, const QString &)> done)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(m_transferTimeoutMs);
    if (!m_userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, destPath, attemptNo, done = std::move(done)]() mutable {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = status == 0 || (status >= 200 && status < 300);
        if (reply->error() != QNetworkReply::NoError || !httpOk) {
            const int httpStatus = httpOk ? 0 : status;
            if (attemptNo < m_maxAttempts && isTransient(reply->error(), httpStatus)) {
                int delayMs = m_retryBaseMs * (1 << (attemptNo - 1));
                bool ok = false;
                const int retryAfter = reply->rawHeader("Retry-After").trimmed().toInt(&ok);
                if (ok && retryAfter > 0)
                    delayMs = std::max(delayMs, std::min(retryAfter, 60) * 1000);
                QTimer::singleShot(delayMs, this,
                                   [this, url, destPath, attemptNo, done = std::move(done)]() mutable {
                                       attempt(url, destPath, attemptNo + 1, std::move(done));
                                   });
                return;
            }
            QString error = reply->error() == QNetworkReply::OperationCanceledError
                                ? tr("the download stalled for over %n second(s)", nullptr,
                                     m_transferTimeoutMs / 1000)
                            : reply->errorString().isEmpty() ? QStringLiteral("HTTP %1").arg(status)
                                                             : reply->errorString();
            if (attemptNo > 1)
                error = tr("%1 (after %n attempt(s))", nullptr, attemptNo).arg(error);
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
