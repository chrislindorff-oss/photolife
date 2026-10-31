#include "net/TileCache.h"

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace pl::net;

// A tile server that's busy the first time it's asked for /1/0/0, never
// answers the first request for /1/0/1, and doesn't have /1/1/0.
class FlakyTileServer : public QObject
{
public:
    FlakyTileServer()
    {
        QImage image(256, 256, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QBuffer buffer(&m_png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { answer(socket); });
        });
    }
    QString urlTemplate() const
    {
        return QStringLiteral("http://127.0.0.1:%1/{z}/{x}/{y}.png").arg(m_server.serverPort());
    }
    QHash<QString, int> asked;

private:
    void answer(QTcpSocket *socket)
    {
        const QByteArray request = socket->readAll();
        const QString path = QString::fromLatin1(request.split(' ').value(1));
        const int n = ++asked[path];
        QByteArray reply;
        if (path == QLatin1String("/1/0/0.png") && n == 1) {
            reply = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\n";
        } else if (path == QLatin1String("/1/0/1.png") && n == 1) {
            return;   // hangs
        } else if (path == QLatin1String("/1/1/0.png")) {
            reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n";
        } else {
            reply = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: "
                    + QByteArray::number(m_png.size()) + "\r\n";
        }
        reply += "Connection: close\r\n\r\n";
        if (reply.startsWith("HTTP/1.1 200"))
            reply += m_png;
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QByteArray m_png;
};

class TestTileCache : public QObject
{
    Q_OBJECT

private slots:
    // A busy server, a hung download and a missing tile: the first two are
    // tried again and arrive; the missing one isn't asked for again.
    void failedTilesAreRetried()
    {
        FlakyTileServer server;
        QTemporaryDir dir;
        TileCache cache(dir.path(), "PhotoLife-test");
        cache.setUrlTemplate(server.urlTemplate());
        cache.setTimeoutMs(500);
        cache.setRetryBaseMs(200);

        // Asked again on each try, as a view repainting on ready() does.
        QTRY_VERIFY_WITH_TIMEOUT(!cache.tile(1, 0, 0).isNull() && !cache.tile(1, 0, 1).isNull()
                                     && (cache.tile(1, 1, 0), true),
                                 15000);
        QCOMPARE(server.asked.value(QStringLiteral("/1/0/0.png")), 2);   // busy, then fine
        QCOMPARE(server.asked.value(QStringLiteral("/1/0/1.png")), 2);   // hung, then fine
        QTest::qWait(800);
        cache.tile(1, 1, 0);
        QCOMPARE(server.asked.value(QStringLiteral("/1/1/0.png")), 1);   // not there: not asked again
        // ...and the ones that came are on disk now.
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("1/0/0.png"))));
    }
};

QTEST_MAIN(TestTileCache)
#include "tst_tilecache.moc"
