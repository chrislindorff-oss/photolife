#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

namespace pl::net {

// The one operation HttpClient needs from the outside world: an async GET.
// The production implementation wraps QNetworkAccessManager; tests substitute a
// fake so the rate-limiting, retry and caching logic can be exercised offline.
class Transport
{
public:
    virtual ~Transport() = default;

    struct Request
    {
        QUrl url;
        QByteArray userAgent;
        QString ifNoneMatch;      // -> If-None-Match
        QString ifModifiedSince;  // -> If-Modified-Since
    };

    struct Reply
    {
        int status = 0;              // HTTP status; 0 if the request never completed
        QByteArray body;
        QString etag;
        QString lastModified;
        int retryAfterSeconds = 0;   // from a Retry-After header, if any
        QString error;               // transport/network error; empty on an HTTP reply
    };

    using Callback = std::function<void(Reply)>;

    virtual void send(const Request &request, Callback callback) = 0;
};

} // namespace pl::net
