#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkRequest>
#include <QPair>
#include <QString>

#include <functional>
#include <memory>

class QIODevice;
class QNetworkAccessManager;

namespace pl::libsync {

// Synchronous HTTP for the photo stores: each call runs a local event loop
// until its reply finishes, streaming large bodies to and from files. Unlike
// net::HttpClient there is no cache and no rate limiting -- photo transfers
// are big, few and not repeatable from a cache. Use from one thread only (the
// network manager is made on the first call, on the calling thread).
class BlockingHttp
{
public:
    using ByteProgress = std::function<void(qint64 done, qint64 total)>;
    using CancelFn = std::function<bool()>;

    struct Response
    {
        int status = 0;               // HTTP status; 0 if no reply arrived
        QByteArray body;              // when not streamed to a sink
        QList<QPair<QByteArray, QByteArray>> headers;
        QString error;                // network/transport failure; empty on any HTTP reply
        bool cancelled = false;

        bool ok() const { return status >= 200 && status < 300; }
        QByteArray header(const QByteArray &name) const;
    };

    // A request is given up when nothing is sent or received for
    // `idleTimeoutMs` (big transfers can take as long as they need).
    explicit BlockingHttp(int idleTimeoutMs = 60000);
    ~BlockingHttp();

    // Sends `verb` with `body` (an open device read for the body) or else
    // `bodyBytes`. With a `sink` (open for writing) the reply body is
    // streamed into it instead of returned.
    Response send(const QNetworkRequest &request, const QByteArray &verb, QIODevice *body,
                  const QByteArray &bodyBytes = {}, QIODevice *sink = nullptr,
                  const ByteProgress &progress = {}, const CancelFn &cancel = {});

private:
    int m_idleTimeoutMs;
    std::unique_ptr<QNetworkAccessManager> m_nam;
};

} // namespace pl::libsync
