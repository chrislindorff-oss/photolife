#include "net/HttpClient.h"

#include "taxonomy/TaxonomyStore.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>

namespace pl::net {

HttpClient::HttpClient(std::unique_ptr<Transport> transport, pl::taxonomy::TaxonomyStore *cache,
                       QObject *parent)
    : QObject(parent), m_transport(std::move(transport)), m_cache(cache)
{
}

HttpClient::~HttpClient() = default;

void HttpClient::get(const QUrl &url, Handler handler)
{
    m_queue.enqueue({url, std::move(handler), 0});
    pump();
}

bool HttpClient::isRetriable(const Transport::Reply &reply)
{
    if (!reply.error.isEmpty() && reply.status == 0)
        return true;   // network failure
    switch (reply.status) {
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
        return true;
    default:
        return false;
    }
}

void HttpClient::pump()
{
    if (m_inFlight)
        return;
    if (m_queue.isEmpty()) {
        emit idle();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = m_lastDispatchMs == 0
                            ? 0
                            : std::max<qint64>(0, m_minIntervalMs - (now - m_lastDispatchMs));

    m_inFlight = true;
    Job job = m_queue.dequeue();
    QTimer::singleShot(int(wait), this, [this, job = std::move(job)]() mutable {
        dispatch(std::move(job));
    });
}

void HttpClient::dispatch(Job job)
{
    m_lastDispatchMs = QDateTime::currentMSecsSinceEpoch();

    const QString urlKey = job.url.toString(QUrl::FullyEncoded);

    Transport::Request req;
    req.url = job.url;
    req.userAgent = m_userAgent;

    std::optional<pl::taxonomy::TaxonomyStore::CacheEntry> cached;
    if (m_cache) {
        cached = m_cache->cachedResponse(urlKey);
        if (cached) {
            req.ifNoneMatch = cached->etag;
            req.ifModifiedSince = cached->lastModified;
        }
    }

    m_transport->send(req, [this, job = std::move(job), urlKey, cached](Transport::Reply reply) mutable {
        // 304: serve the cached body.
        if (reply.status == 304 && cached) {
            HttpResponse resp;
            resp.status = 200;
            resp.body = cached->body;
            resp.fromCache = true;
            finishJob(std::move(job), std::move(resp));
            return;
        }

        // Retry transient failures with capped exponential backoff.
        if (isRetriable(reply) && job.attempt < m_maxRetries) {
            const int nextAttempt = job.attempt + 1;
            int delayMs = m_retryBaseMs * (1 << job.attempt);
            if (reply.retryAfterSeconds > 0)
                delayMs = std::max(delayMs, reply.retryAfterSeconds * 1000);
            delayMs = std::min(delayMs, 30000);

            Job retry = std::move(job);
            retry.attempt = nextAttempt;
            m_inFlight = false;
            QTimer::singleShot(delayMs, this, [this, retry = std::move(retry)]() mutable {
                m_queue.prepend(std::move(retry));
                pump();
            });
            return;
        }

        HttpResponse resp;
        resp.status = reply.status;
        resp.body = reply.body;
        resp.error = reply.error;

        if (m_cache && reply.status == 200) {
            m_cache->storeResponse(urlKey, reply.etag, reply.lastModified, reply.status,
                                   reply.body);
        }

        finishJob(std::move(job), std::move(resp));
    });
}

void HttpClient::finishJob(Job job, HttpResponse response)
{
    if (job.handler)
        job.handler(std::move(response));

    m_inFlight = false;
    pump();
}

} // namespace pl::net
