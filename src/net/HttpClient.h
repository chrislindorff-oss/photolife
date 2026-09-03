#pragma once

#include "net/Transport.h"

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QUrl>

#include <functional>
#include <memory>

namespace pl::taxonomy {
class TaxonomyStore;
}

namespace pl::net {

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
// than minRequestInterval after the last. Lives on and is used from one thread
// (the network stack it drives is asynchronous, so this need not be a worker).
class HttpClient : public QObject
{
    Q_OBJECT

public:
    HttpClient(std::unique_ptr<Transport> transport, pl::taxonomy::TaxonomyStore *cache,
               QObject *parent = nullptr);
    ~HttpClient() override;

    void setUserAgent(const QByteArray &userAgent) { m_userAgent = userAgent; }
    void setMinRequestIntervalMs(int ms) { m_minIntervalMs = ms; }
    void setMaxRetries(int n) { m_maxRetries = n; }
    void setRetryBaseDelayMs(int ms) { m_retryBaseMs = ms; }

    using Handler = std::function<void(HttpResponse)>;
    void get(const QUrl &url, Handler handler);

    int queuedCount() const { return int(m_queue.size()); }
    bool isBusy() const { return m_inFlight || !m_queue.isEmpty(); }

signals:
    void idle();   // queue drained and nothing in flight

private:
    struct Job
    {
        QUrl url;
        Handler handler;
        int attempt = 0;
    };

    void pump();
    void dispatch(Job job);
    void finishJob(Job job, HttpResponse response);
    static bool isRetriable(const Transport::Reply &reply);

    std::unique_ptr<Transport> m_transport;
    pl::taxonomy::TaxonomyStore *m_cache;

    QByteArray m_userAgent;
    int m_minIntervalMs = 1100;   // ~55 requests/minute, under iNat's 60
    int m_maxRetries = 3;
    int m_retryBaseMs = 500;

    QQueue<Job> m_queue;
    bool m_inFlight = false;
    qint64 m_lastDispatchMs = 0;
};

} // namespace pl::net
