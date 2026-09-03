#include "net/QtNetworkTransport.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace pl::net {

QtNetworkTransport::QtNetworkTransport(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
}

QtNetworkTransport::~QtNetworkTransport() = default;

void QtNetworkTransport::send(const Request &request, Callback callback)
{
    QNetworkRequest req(request.url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!request.userAgent.isEmpty())
        req.setHeader(QNetworkRequest::UserAgentHeader, request.userAgent);
    if (!request.ifNoneMatch.isEmpty())
        req.setRawHeader("If-None-Match", request.ifNoneMatch.toUtf8());
    if (!request.ifModifiedSince.isEmpty())
        req.setRawHeader("If-Modified-Since", request.ifModifiedSince.toUtf8());

    QNetworkReply *reply = m_nam->get(req);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback] {
        Reply out;
        out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        out.body = reply->readAll();
        out.etag = QString::fromUtf8(reply->rawHeader("ETag"));
        out.lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified"));

        const QByteArray retryAfter = reply->rawHeader("Retry-After");
        if (!retryAfter.isEmpty()) {
            bool numeric = false;
            const int seconds = retryAfter.trimmed().toInt(&numeric);
            if (numeric)
                out.retryAfterSeconds = seconds;
        }

        if (reply->error() != QNetworkReply::NoError && out.status == 0)
            out.error = reply->errorString();

        reply->deleteLater();
        callback(out);
    });
}

} // namespace pl::net
