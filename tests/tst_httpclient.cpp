#include <QtTest>

#include <QElapsedTimer>
#include <QTimer>

#include "db/Database.h"
#include "net/HttpClient.h"
#include "net/Transport.h"
#include "taxonomy/TaxonomyStore.h"

using namespace pl;
using namespace pl::net;

namespace {

// Scripted transport: `responder(request, callIndex)` decides each reply; every
// request is recorded and answered on the next event-loop turn.
class FakeTransport : public Transport
{
public:
    std::function<Reply(const Request &, int)> responder;
    QList<Request> received;

    void send(const Request &request, Callback callback) override
    {
        const int index = received.size();
        received.append(request);
        Reply reply = responder ? responder(request, index) : Reply{};
        QTimer::singleShot(0, [callback = std::move(callback), reply = std::move(reply)] {
            callback(reply);
        });
    }
};

Transport::Reply okReply(const QByteArray &body, const QString &etag = {})
{
    Transport::Reply r;
    r.status = 200;
    r.body = body;
    r.etag = etag;
    return r;
}

} // namespace

class TestHttpClient : public QObject
{
    Q_OBJECT

private slots:
    void servesSuccessfulBody();
    void serialisesAndRateLimits();
    void usesEtagAndServesCachedBodyOn304();
    void retriesTransientFailureThenSucceeds();
    void doesNotRetryClientError();
    void retriesOnNetworkError();
};

void TestHttpClient::servesSuccessfulBody()
{
    auto transport = std::make_unique<FakeTransport>();
    transport->responder = [](const Transport::Request &, int) {
        return okReply(QByteArrayLiteral("{\"ok\":true}"));
    };

    HttpClient client(std::move(transport), nullptr);
    client.setMinRequestIntervalMs(0);

    HttpResponse got;
    bool done = false;
    client.get(QUrl(QStringLiteral("https://example.test/a")), [&](HttpResponse r) {
        got = r;
        done = true;
    });

    QTRY_VERIFY(done);
    QVERIFY(got.ok());
    QCOMPARE(got.body, QByteArrayLiteral("{\"ok\":true}"));
    QVERIFY(!got.fromCache);
}

void TestHttpClient::serialisesAndRateLimits()
{
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *raw = transport.get();
    transport->responder = [](const Transport::Request &, int) {
        return okReply(QByteArrayLiteral("x"));
    };

    HttpClient client(std::move(transport), nullptr);
    client.setMinRequestIntervalMs(120);

    int completed = 0;
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 3; ++i)
        client.get(QUrl(QStringLiteral("https://example.test/%1").arg(i)),
                   [&](HttpResponse) { ++completed; });

    QTRY_COMPARE(completed, 3);
    // Three requests, two >=120 ms gaps between dispatches.
    QVERIFY2(clock.elapsed() >= 220, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(raw->received.size(), 3);
}

void TestHttpClient::usesEtagAndServesCachedBodyOn304()
{
    Database db;
    QVERIFY(db.open(QStringLiteral(":memory:")));
    taxonomy::TaxonomyStore store(db.connectionName());

    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *raw = transport.get();
    transport->responder = [](const Transport::Request &req, int call) -> Transport::Reply {
        if (call == 0)
            return okReply(QByteArrayLiteral("payload-v1"), QStringLiteral("\"etag-1\""));
        // Second call must carry the stored validator.
        Transport::Reply r;
        r.status = req.ifNoneMatch == QStringLiteral("\"etag-1\"") ? 304 : 200;
        return r;
    };

    HttpClient client(std::move(transport), &store);
    client.setMinRequestIntervalMs(0);

    const QUrl url(QStringLiteral("https://api.inaturalist.org/v1/taxa/1"));

    HttpResponse first;
    bool firstDone = false;
    client.get(url, [&](HttpResponse r) { first = r; firstDone = true; });
    QTRY_VERIFY(firstDone);
    QCOMPARE(first.body, QByteArrayLiteral("payload-v1"));
    QVERIFY(!first.fromCache);

    HttpResponse second;
    bool secondDone = false;
    client.get(url, [&](HttpResponse r) { second = r; secondDone = true; });
    QTRY_VERIFY(secondDone);
    QVERIFY(second.ok());
    QVERIFY(second.fromCache);
    QCOMPARE(second.body, QByteArrayLiteral("payload-v1"));
    QCOMPARE(raw->received.size(), 2);
    QCOMPARE(raw->received.at(1).ifNoneMatch, QStringLiteral("\"etag-1\""));
}

void TestHttpClient::retriesTransientFailureThenSucceeds()
{
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *raw = transport.get();
    transport->responder = [](const Transport::Request &, int call) -> Transport::Reply {
        if (call < 2) {
            Transport::Reply r;
            r.status = 503;
            return r;
        }
        return okReply(QByteArrayLiteral("finally"));
    };

    HttpClient client(std::move(transport), nullptr);
    client.setMinRequestIntervalMs(0);
    client.setRetryBaseDelayMs(10);
    client.setMaxRetries(3);

    HttpResponse got;
    bool done = false;
    client.get(QUrl(QStringLiteral("https://example.test/flaky")),
               [&](HttpResponse r) { got = r; done = true; });

    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(got.ok());
    QCOMPARE(got.body, QByteArrayLiteral("finally"));
    QCOMPARE(raw->received.size(), 3);
}

void TestHttpClient::doesNotRetryClientError()
{
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *raw = transport.get();
    transport->responder = [](const Transport::Request &, int) {
        Transport::Reply r;
        r.status = 404;
        r.body = QByteArrayLiteral("not found");
        return r;
    };

    HttpClient client(std::move(transport), nullptr);
    client.setMinRequestIntervalMs(0);

    HttpResponse got;
    bool done = false;
    client.get(QUrl(QStringLiteral("https://example.test/missing")),
               [&](HttpResponse r) { got = r; done = true; });

    QTRY_VERIFY(done);
    QCOMPARE(got.status, 404);
    QVERIFY(!got.ok());
    QCOMPARE(raw->received.size(), 1);
}

void TestHttpClient::retriesOnNetworkError()
{
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *raw = transport.get();
    transport->responder = [](const Transport::Request &, int call) -> Transport::Reply {
        if (call == 0) {
            Transport::Reply r;
            r.error = QStringLiteral("Host unreachable");
            return r;
        }
        return okReply(QByteArrayLiteral("recovered"));
    };

    HttpClient client(std::move(transport), nullptr);
    client.setMinRequestIntervalMs(0);
    client.setRetryBaseDelayMs(10);

    HttpResponse got;
    bool done = false;
    client.get(QUrl(QStringLiteral("https://example.test/net")),
               [&](HttpResponse r) { got = r; done = true; });

    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(got.ok());
    QCOMPARE(raw->received.size(), 2);
}

QTEST_GUILESS_MAIN(TestHttpClient)
#include "tst_httpclient.moc"
