#include "libsync/BlockingHttp.h"

#include <QEventLoop>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

namespace pl::libsync {

QByteArray BlockingHttp::Response::header(const QByteArray &name) const
{
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0)
            return value;
    }
    return {};
}

BlockingHttp::BlockingHttp(int idleTimeoutMs) : m_idleTimeoutMs(idleTimeoutMs) {}

BlockingHttp::~BlockingHttp() = default;

BlockingHttp::Response BlockingHttp::send(const QNetworkRequest &request, const QByteArray &verb,
                                          QIODevice *body, const QByteArray &bodyBytes,
                                          QIODevice *sink, const ByteProgress &progress,
                                          const CancelFn &cancel)
{
    if (!m_nam)
        m_nam = std::make_unique<QNetworkAccessManager>();

    // HEAD must go through head(): as a custom verb Qt waits for the body its
    // Content-Length promises.
    QNetworkReply *reply = verb == "HEAD" ? m_nam->head(request)
                           : body         ? m_nam->sendCustomRequest(request, verb, body)
                                          : m_nam->sendCustomRequest(request, verb, bodyBytes);

    Response out;
    QEventLoop loop;
    bool timedOut = false;

    QTimer idle;
    idle.setSingleShot(true);
    idle.setInterval(m_idleTimeoutMs);
    QObject::connect(&idle, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });

    QTimer cancelPoll;
    cancelPoll.setInterval(200);
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&] {
        if (cancel && cancel()) {
            out.cancelled = true;
            reply->abort();
        }
    });

    bool sinkFailed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        idle.start();
        if (sink && !sinkFailed) {
            const QByteArray chunk = reply->readAll();
            if (sink->write(chunk) != chunk.size()) {
                sinkFailed = true;
                reply->abort();
            }
        }
    });
    QObject::connect(reply, &QNetworkReply::uploadProgress, &loop, [&](qint64 done, qint64 total) {
        idle.start();
        if (progress && body)
            progress(done, total);
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                     [&](qint64 done, qint64 total) {
                         idle.start();
                         if (progress && !body)
                             progress(done, total);
                     });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    idle.start();
    if (cancel)
        cancelPoll.start();
    if (!reply->isFinished())
        loop.exec();

    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.headers = reply->rawHeaderPairs();
    if (sink) {
        if (!sinkFailed) {
            const QByteArray rest = reply->readAll();
            if (sink->write(rest) != rest.size())
                sinkFailed = true;
        }
    } else {
        out.body = reply->readAll();
    }

    if (out.cancelled)
        out.error = QStringLiteral("Cancelled");
    else if (timedOut)
        out.error = QStringLiteral("No response for %1 s").arg(m_idleTimeoutMs / 1000);
    else if (sinkFailed)
        out.error = QStringLiteral("Couldn't write the downloaded file (%1)").arg(sink->errorString());
    else if (out.status == 0 && reply->error() != QNetworkReply::NoError)
        out.error = reply->errorString();
    if (!out.error.isEmpty() && (timedOut || out.cancelled || sinkFailed))
        out.status = 0;

    delete reply;   // finished; a worker thread may have no event loop to run deleteLater()
    return out;
}

} // namespace pl::libsync
