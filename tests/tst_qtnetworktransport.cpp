// QtNetworkTransport: a request to a server that never answers is given up,
// not waited on for good.
#include "net/QtNetworkTransport.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace pl::net;

class TestQtNetworkTransport : public QObject
{
    Q_OBJECT

private slots:
    void aStalledRequestFailsAfterTheIdleTimeout();
    void anAnsweredRequestIsUnaffected();
};

void TestQtNetworkTransport::aStalledRequestFailsAfterTheIdleTimeout()
{
    // Accepts the connection, reads the request, and says nothing.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QList<QTcpSocket *> held;
    connect(&server, &QTcpServer::newConnection, &server, [&] { held << server.nextPendingConnection(); });

    QtNetworkTransport transport;
    transport.setIdleTimeouts(400, 600);
    for (const QByteArray &verb : {QByteArray("GET"), QByteArray("POST")}) {
        Transport::Request request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/v1/taxa").arg(server.serverPort()));
        request.verb = verb;
        if (verb != "GET")
            request.body = "x=1";
        bool done = false;
        Transport::Reply reply;
        transport.send(request, [&](Transport::Reply r) {
            reply = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QCOMPARE(reply.status, 0);
        QVERIFY2(reply.error.contains(QStringLiteral("no response")), qPrintable(reply.error));
    }
}

void TestQtNetworkTransport::anAnsweredRequestIsUnaffected()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            socket->readAll();
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nETag: \"abc\"\r\nConnection: close\r\n\r\nok");
            socket->disconnectFromHost();
        });
    });
    QtNetworkTransport transport;
    transport.setIdleTimeouts(2000, 2000);
    Transport::Request request;
    request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
    bool done = false;
    Transport::Reply reply;
    transport.send(request, [&](Transport::Reply r) {
        reply = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(reply.status, 200);
    QCOMPARE(reply.body, QByteArray("ok"));
    QVERIFY(reply.error.isEmpty());
    QCOMPARE(reply.etag, QStringLiteral("\"abc\""));
}

QTEST_GUILESS_MAIN(TestQtNetworkTransport)
#include "tst_qtnetworktransport.moc"
