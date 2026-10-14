#pragma once

#include "net/Transport.h"

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QUrl>

#include <functional>
#include <memory>

namespace pl::net {

class HttpCache;

struct HttpResponse
{
    int status = 0;
    QByteArray body;
    bool fromCache = false;   // body served from the conditional-GET cache (304)
    QString error;            // network/transport error, empty if an HTTP reply came back

    bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
};

// Serialised, rate-limited HTTP GET with retry/backoff and ETag conditional
// requests. One request in flight at a time; the next is dispatched no sooner
// than minRequestInterval after the last. Only URLs HttpCache::isCacheable
// accepts are cached. Lives on and is used from one thread
// (the network stack it drives is asynchronous, so this need not be a worker).
//
// Background requests (quiet housekeeping the user isn't waiting on) are only
// dispatched while no Normal request is queued, so a long background job
// never makes a tree build or import wait behind it. They share the one rate
// limit, since they usually go to the same API.
class HttpClient : public QObject
{
    Q_OBJECT

public:
    enum class Priority { Normal, Background };

    HttpClient(std::unique_ptr<Transport> transport, HttpCache *cache,
               QObject *parent = nullptr);
    ~HttpClient() override;

    void setUserAgent(const QByteArray &userAgent) { m_userAgent = userAgent; }
    void setMinRequestIntervalMs(int ms) { m_minIntervalMs = ms; }
    void setMaxRetries(int n) { m_maxRetries = n; }
    void setRetryBaseDelayMs(int ms) { m_retryBaseMs = ms; }

    using Handler = std::function<void(HttpResponse)>;
    // `bearerToken`, when non-empty, is sent as `Authorization: Bearer` and
    // makes this one request bypass the conditional-GET cache entirely (both
    // read and write) -- the cache is keyed purely on URL, so an
    // authenticated and an unauthenticated request to the same URL must never
    // share a cached response (e.g. iNaturalist reveals different data, such
    // as true vs. geoprivacy-obscured coordinates, depending on auth).
    void get(const QUrl &url, Handler handler, const QString &bearerToken = QString(),
             Priority priority = Priority::Normal);

    int queuedCount() const { return int(m_queue.size() + m_backgroundQueue.size()); }
    bool isBusy() const { return m_inFlight || queuedCount() > 0; }

signals:
    void idle();   // queue drained and nothing in flight

private:
    struct Job
    {
        QUrl url;
        Handler handler;
        int attempt = 0;
        QString bearerToken;
        Priority priority = Priority::Normal;
    };

    void pump();
    void dispatch(Job job);
    void finishJob(Job job, HttpResponse response);
    static bool isRetriable(const Transport::Reply &reply);

    std::unique_ptr<Transport> m_transport;
    HttpCache *m_cache;

    QByteArray m_userAgent;
    int m_minIntervalMs = 1100;   // ~55 requests/minute, under iNat's 60
    int m_maxRetries = 3;
    int m_retryBaseMs = 500;

    QQueue<Job> &queueFor(Priority priority);

    QQueue<Job> m_queue;             // Normal
    QQueue<Job> m_backgroundQueue;   // Background: only when m_queue is empty
    bool m_inFlight = false;
    qint64 m_lastDispatchMs = 0;
};

} // namespace pl::net
